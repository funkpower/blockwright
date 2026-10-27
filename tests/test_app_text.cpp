// The Text tool (PLAN §15, #18): a click makes or edits a text layer,
// typing edits its source a run at a time, the Tool Options set its
// settings, the face picker, and Bake Text.

#include "canvas_view.h"
#include "document.h"
#include "face_picker.h"
#include "layers_dock.h"
#include "main_window.h"
#include "preferences.h"
#include "settings.h"
#include "text_options.h"
#include "text_tool.h"
#include "tool.h"
#include "tool_options.h"
#include "toolbox.h"

#include "blockwright/font_store.h"
#include "blockwright/big_text.h"
#include "blockwright/layer.h"
#include "blockwright/text.h"

#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QProxyStyle>
#include <QSpinBox>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <cstdlib>
#include <new>
#include <utility>

using namespace bw;

// While not 0, an allocation on this thread of more bytes than this throws
// std::bad_alloc. The whole app test binary allocates through this, which
// otherwise is malloc.
namespace bwtest {
thread_local std::size_t largestAllocation = 0;
}

void* operator new(std::size_t size) {
    if (bwtest::largestAllocation && size > bwtest::largestAllocation)
        throw std::bad_alloc();
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

int faceCount(const QListView* list) { return list->model()->rowCount(); }
QModelIndex faceRow(const QListView* list, int row) { return list->model()->index(row, 0); }
QString faceLabel(const QListView* list, int row) { return faceRow(list, row).data(Qt::ToolTipRole).toString(); }
QIcon faceIcon(const QListView* list, int row) { return qvariant_cast<QIcon>(faceRow(list, row).data(Qt::DecorationRole)); }
void selectFaceRow(QListView* list, int row) { list->setCurrentIndex(faceRow(list, row)); }

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

// A left click in the middle of canvas cell (x, y).
void click(CanvasView* v, int x, int y) {
    const QPointF at = v->cellRect(QPoint(x, y)).center();
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent e(type, at, v->mapToGlobal(at), Qt::LeftButton,
                      type == QEvent::MouseButtonPress ? Qt::MouseButtons(Qt::LeftButton) : Qt::NoButton,
                      Qt::NoModifier);
        QApplication::sendEvent(v, &e);
    }
}

void key(QWidget* w, int k, const QString& text = {}, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, k, mods, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, mods, text);
    QApplication::sendEvent(w, &release);
}

void type(QWidget* w, const QString& text) {
    for (QChar ch : text)
        key(w, ch.toUpper().unicode(), QString(ch));
}

const TextSource& sourceOf(Document* d) {
    const Layer* l = d->active();
    REQUIRE(l);
    REQUIRE(l->props.text);
    return *l->props.text;
}

TextTool* textTool(MainWindow& w) {
    auto* t = qobject_cast<TextTool*>(w.tools()->find(QStringLiteral("text")));
    REQUIRE(t);
    return t;
}

FaceRef pixel(const char* name) {
    FaceRef r;
    r.name = name;
    return r;
}

// A text layer at (2, 1) holding `text`, made with the tool.
void makeText(MainWindow& w, const QString& text) {
    use(w, "text");
    click(w.canvasView(), 2, 1);
    type(w.canvasView(), text);
}

} // namespace

TEST_CASE("the Text tool has its shortcut, a Tools menu entry and a toolbox button", "[text]") {
    MainWindow w;
    QAction* a = act(w, QStringLiteral("textTool"));
    CHECK(a->shortcut() == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X));
    CHECK(QString(a->text()).remove(QLatin1Char('&')) == QStringLiteral("Text"));
    QMenu* tools = nullptr;
    for (QAction* m : w.menuBar()->actions())
        if (m->text() == QStringLiteral("&Tools"))
            tools = m->menu();
    REQUIRE(tools);
    CHECK(tools->actions().contains(a));
    REQUIRE(w.toolbox()->buttonFor(QStringLiteral("text")));
    CHECK(w.toolbox()->buttonFor(QStringLiteral("text"))->defaultAction() == a);

    // no other action has the key (Ctrl+X is Cut)
    for (QAction* other : w.findChildren<QAction*>())
        if (other != a) {
            INFO(other->objectName().toStdString());
            CHECK(other->shortcut() != a->shortcut());
        }
    a->trigger();
    CHECK(w.tools()->active()->id() == QStringLiteral("text"));
    bool known = false;
    CHECK(ToolOptions::optionsFor(QStringLiteral("text"), &known) == ToolOptions::Options(ToolOptions::Option::Text));
    CHECK(known);
    CHECK(w.toolOptions()->shown().testFlag(ToolOptions::Option::Text));
    REQUIRE(w.toolOptions()->textOptions());
    CHECK(w.toolOptions()->textOptions()->isVisibleTo(w.toolOptions()));
}

TEST_CASE("a click with the Text tool puts a text cursor at the cell, and typing makes a text layer there", "[text]") {
    MainWindow w;
    Document* d = w.document();
    const size_t layers = d->layers().size();
    const int steps = d->undoStack()->count();
    use(w, "text");
    click(w.canvasView(), 5, 3);
    // nothing yet: the cursor waits for the first character
    CHECK(d->layers().size() == layers);
    CHECK(d->undoStack()->count() == steps);
    CHECK(textTool(w)->editing() == 0);
    REQUIRE(textTool(w)->spot());
    CHECK(*textTool(w)->spot() == QPoint(5, 3));
    CHECK(textTool(w)->caretCell() == QPoint(5, 3));
    CHECK_FALSE(w.canvasView()->grab().isNull());
    // keys that type nothing leave it so
    for (const int k : {Qt::Key_Backspace, Qt::Key_Delete, Qt::Key_Left, Qt::Key_Right, Qt::Key_Home, Qt::Key_End,
                        Qt::Key_Up, Qt::Key_Down})
        key(w.canvasView(), k);
    CHECK(d->layers().size() == layers);
    CHECK(textTool(w)->spot());

    type(w.canvasView(), QStringLiteral("A"));
    REQUIRE(d->layers().size() == layers + 1);
    const Layer* l = d->active();
    REQUIRE(l->props.text);
    CHECK(l->props.x == 5);
    CHECK(l->props.y == 3);
    CHECK(l->props.text->text == U"A");
    CHECK(textTool(w)->editing() == l->props.id);
    CHECK_FALSE(textTool(w)->spot());
    CHECK(textTool(w)->caret() == 1);
    // the new layer, then its typing
    CHECK(d->undoStack()->count() == steps + 2);

    // a click and Escape leave nothing
    click(w.canvasView(), 40, 15);
    REQUIRE(textTool(w)->spot());
    key(w.canvasView(), Qt::Key_Escape);
    CHECK_FALSE(textTool(w)->spot());
    CHECK(textTool(w)->editing() == 0);
    CHECK(d->layers().size() == layers + 1);
    CHECK(d->undoStack()->count() == steps + 2);

    // an input method's commit makes the layer too, and Enter
    click(w.canvasView(), 40, 15);
    QInputMethodEvent im;
    im.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(w.canvasView(), &im);
    CHECK(sourceOf(d).text == U"é");
    CHECK(d->active()->props.x == 40);
    click(w.canvasView(), 60, 15);
    key(w.canvasView(), Qt::Key_Return, QStringLiteral("\r"));
    CHECK(sourceOf(d).text == U"\n");
    CHECK(d->active()->props.x == 60);
    CHECK(d->layers().size() == layers + 3);
}

TEST_CASE("typing edits the text layer, a run of it one undo step", "[text]") {
    MainWindow w;
    Document* d = w.document();
    const size_t layers = d->layers().size();
    makeText(w, QStringLiteral("Hi"));
    CHECK(sourceOf(d).text == U"Hi");
    const int steps = d->undoStack()->count();
    const LayerId id = d->activeLayer();

    // Enter is a newline and Backspace deletes, in the same run
    key(w.canvasView(), Qt::Key_Return, QStringLiteral("\r"));
    type(w.canvasView(), QStringLiteral("yo"));
    key(w.canvasView(), Qt::Key_Backspace, QStringLiteral("\b"));
    CHECK(sourceOf(d).text == U"Hi\ny");
    CHECK(d->undoStack()->count() == steps);
    CHECK(textTool(w)->caret() == 4);
    // the cells are drawn from it: two lines of the block face
    const Layer* l = d->layers().find(id);
    CHECK(l->height() == 2 * (blockFace().height() / 2));
    // an input method's text goes in at the cursor too (a missing
    // character, laid out blank); the view draws the box and cursor
    QInputMethodEvent im;
    im.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(w.canvasView(), &im);
    CHECK(sourceOf(d).text == U"Hi\nyé");
    CHECK(d->undoStack()->count() == steps);
    CHECK_FALSE(w.canvasView()->grab().isNull());

    // undo takes the whole run back; the layer, made by the click, stays
    d->undoStack()->undo();
    REQUIRE(d->layers().find(id));
    CHECK(d->layers().find(id)->props.text->text.empty());
    CHECK(textTool(w)->caret() == 0);
    d->undoStack()->undo();
    CHECK_FALSE(d->layers().find(id));
    CHECK(d->layers().size() == layers);
}

TEST_CASE("moving the text cursor starts a new run of typing", "[text]") {
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral("ac"));
    const int steps = d->undoStack()->count();
    key(w.canvasView(), Qt::Key_Left);
    type(w.canvasView(), QStringLiteral("b"));
    CHECK(sourceOf(d).text == U"abc");
    CHECK(d->undoStack()->count() == steps + 1);
    key(w.canvasView(), Qt::Key_Home);
    key(w.canvasView(), Qt::Key_Delete);
    CHECK(sourceOf(d).text == U"bc");
    d->undoStack()->undo();
    CHECK(sourceOf(d).text == U"abc");
    d->undoStack()->undo();
    CHECK(sourceOf(d).text == U"ac");

    // with the edit over, a key with the tool on the text layer edits it
    // again, from its end
    use(w, "keyboard");
    use(w, "text");
    CHECK(textTool(w)->editing() == 0);
    key(w.canvasView(), Qt::Key_Backspace, QStringLiteral("\b"));
    CHECK(sourceOf(d).text == U"a");
    CHECK(textTool(w)->editing() == d->activeLayer());
}

TEST_CASE("a click on a text layer edits it, at the character clicked", "[text]") {
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral("abc"));
    const LayerId id = d->activeLayer();
    const size_t layers = d->layers().size();
    // another tool ends the edit
    use(w, "keyboard");
    CHECK(textTool(w)->editing() == 0);
    use(w, "text");
    // the block face is 8 columns a letter: column 2 + 8 is the left of b
    click(w.canvasView(), 2 + 8, 1);
    CHECK(d->layers().size() == layers);
    CHECK(textTool(w)->editing() == id);
    CHECK(textTool(w)->caret() == 1);
    type(w.canvasView(), QStringLiteral("X"));
    CHECK(sourceOf(d).text == U"aXbc");

    // a click off it and typing makes another
    click(w.canvasView(), 40, 15);
    type(w.canvasView(), QStringLiteral("Y"));
    CHECK(d->layers().size() == layers + 1);
    CHECK(d->activeLayer() != id);
    CHECK(d->layers().find(id)->props.text->text == U"aXbc");
}

TEST_CASE("each Text tool option sets the text layer's source, one undo step each", "[text]") {
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral("ab cd"));
    TextOptions* o = w.toolOptions()->textOptions();
    REQUIRE(o);
    const TextSource start = sourceOf(d);

    const auto check = [&](const char* what, auto set, auto changed) {
        INFO(what);
        const int steps = d->undoStack()->index();
        set();
        CHECK(changed(sourceOf(d)));
        CHECK(d->undoStack()->index() == steps + 1);
        CHECK(sourceOf(d).text == start.text);
        d->undoStack()->undo();
        CHECK(sourceOf(d) == start);
    };
    check("face", [&] {
        QListView* list = o->facePicker()->list();
        for (int i = 0; i < faceCount(list); ++i)
            if (faceLabel(list, i).startsWith(QStringLiteral("vga")))
                selectFaceRow(list, i);
    }, [](const TextSource& s) { return s.face == pixel("vga"); });
    check("case", [&] { o->caseBox()->setCurrentIndex(o->caseBox()->findData(int(TextCase::Capitals))); },
          [](const TextSource& s) { return s.textCase == TextCase::Capitals; });
    check("letter spacing", [&] { o->letterSpacingBox()->setValue(2); },
          [](const TextSource& s) { return s.letterSpacing == 2; });
    check("line spacing", [&] { o->lineSpacingBox()->setValue(1); },
          [](const TextSource& s) { return s.lineSpacing == 1; });
    check("alignment", [&] { o->alignBox()->setCurrentIndex(o->alignBox()->findData(int(TextAlign::Right))); },
          [](const TextSource& s) { return s.align == TextAlign::Right; });
    check("wrap width", [&] { o->wrapWidthBox()->setValue(20); },
          [](const TextSource& s) { return s.wrapWidth == 20; });
    check("colour", [&] { o->colourBox()->setCurrentIndex(o->colourBox()->findData(12)); },
          [](const TextSource& s) { return s.colour == 12; });

    // the controls follow the layer, undo included
    o->letterSpacingBox()->setValue(3);
    d->undoStack()->undo();
    CHECK(o->letterSpacingBox()->value() == 0);

    // a new layer starts with the settings last set
    o->colourBox()->setCurrentIndex(o->colourBox()->findData(10));
    click(w.canvasView(), 40, 15);
    type(w.canvasView(), QStringLiteral("Z"));
    CHECK(sourceOf(d).colour == 10);
    CHECK(sourceOf(d).text == U"Z");
}

TEST_CASE("while the text cursor waits for a new layer the Tool Options set what it starts with", "[text]") {
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral("ab"));
    const LayerId e = d->activeLayer();
    TextOptions* o = w.toolOptions()->textOptions();
    o->colourBox()->setCurrentIndex(o->colourBox()->findData(12));
    REQUIRE(sourceOf(d).colour == 12);
    o->wrapWidthBox()->setValue(30);
    REQUIRE(sourceOf(d).wrapWidth == 30);
    const TextSource source = sourceOf(d);
    const int steps = d->undoStack()->count();

    click(w.canvasView(), 40, 15);
    REQUIRE(textTool(w)->spot());
    REQUIRE(d->activeLayer() == e);
    // the controls show the new layer's settings, the last set
    CHECK(o->colourBox()->currentData().toInt() == 12);
    CHECK(o->wrapWidthBox()->value() == 30);
    // and set them, not the active layer's
    o->colourBox()->setCurrentIndex(o->colourBox()->findData(9));
    CHECK(*d->layers().find(e)->props.text == source);
    CHECK(d->undoStack()->count() == steps);
    CHECK(textTool(w)->defaults().colour == 9);
    type(w.canvasView(), QStringLiteral("Z"));
    REQUIRE(d->activeLayer() != e);
    CHECK(sourceOf(d).colour == 9);
    CHECK(sourceOf(d).wrapWidth == 30);

    // the cursor put away, they show the active layer again
    click(w.canvasView(), 60, 15);
    REQUIRE(d->setActiveLayer(e));
    CHECK_FALSE(textTool(w)->spot());
    CHECK(o->colourBox()->currentData().toInt() == 12);
}

TEST_CASE("the face picker lists the bundled faces with previews, searches them and keeps favourites", "[text]") {
    {
        const auto s = appSettings();
        s->remove(FacePicker::favouritesKey());
    }
    FacePicker p;
    CHECK(p.listed() == QStringList{QStringLiteral("block"), QStringLiteral("vga"), QStringLiteral("mini")});
    for (int i = 0; i < faceCount(p.list()); ++i)
        CHECK_FALSE(faceIcon(p.list(), i).isNull());
    CHECK(p.current() == pixel("block"));

    p.searchField()->setText(QStringLiteral("V"));
    CHECK(p.listed() == QStringList{QStringLiteral("vga")});
    p.searchField()->clear();

    // a favourite, the current face's, is kept in the settings and listed
    // first, here and in the next picker
    p.setCurrent(pixel("mini"));
    p.favouriteButton()->click();
    CHECK(p.isFavourite(pixel("mini")));
    CHECK(p.listed().first() == QStringLiteral("mini"));
    CHECK(appSettings()->value(FacePicker::favouritesKey()).toStringList() == QStringList{QStringLiteral("pixel:mini")});
    FacePicker next;
    CHECK(next.isFavourite(pixel("mini")));
    CHECK(next.listed() == QStringList{QStringLiteral("mini"), QStringLiteral("block"), QStringLiteral("vga")});
    next.setCurrent(pixel("mini"));
    CHECK(next.favouriteButton()->isChecked());
    next.favouriteButton()->click();
    CHECK_FALSE(next.isFavourite(pixel("mini")));
    CHECK(appSettings()->value(FacePicker::favouritesKey()).toStringList().isEmpty());

    // picking a face emits it
    FaceRef picked;
    QObject::connect(&next, &FacePicker::picked, [&](const FaceRef& r) { picked = r; });
    selectFaceRow(next.list(), 1);
    CHECK(picked == pixel("vga"));
}

TEST_CASE("the face picker marks the characters a face lacks and its confusables in the text", "[text]") {
    FacePicker p;
    p.setText(U"SO50 é");
    for (const char* name : {"block", "vga", "mini"}) {
        INFO(name);
        CHECK(p.missingIn(pixel(name)) == QStringLiteral("é"));
    }
    CHECK(p.confusablesIn(pixel("block")).isEmpty());
    CHECK(p.confusablesIn(pixel("mini")) == QStringLiteral("S5 O0"));
    const QListView* list = p.list();
    for (int i = 0; i < faceCount(list); ++i) {
        const QString label = faceLabel(list, i);
        INFO(label.toStdString());
        CHECK(label.contains(QStringLiteral("missing é")));
        CHECK(label.startsWith(QStringLiteral("mini")) == label.contains(QStringLiteral("confusable S5 O0")));
    }
    p.setText(U"funk");
    CHECK(p.missingIn(pixel("mini")).isEmpty());
    CHECK(faceLabel(p.list(), 0) == QStringLiteral("block"));

    // the dock's picker marks the active text layer's text, in its case
    MainWindow w;
    makeText(w, QStringLiteral("so"));
    FacePicker* dock = w.toolOptions()->textOptions()->facePicker();
    CHECK(dock->confusablesIn(pixel("mini")).isEmpty());
    w.toolOptions()->textOptions()->caseBox()->setCurrentIndex(1);
    CHECK(dock->confusablesIn(pixel("mini")).isEmpty());  // S and O aren't a pair
    type(w.canvasView(), QStringLiteral("50"));
    CHECK(dock->confusablesIn(pixel("mini")) == QStringLiteral("S5 O0"));
}

// Records the text of every list row the style is asked to draw.
class RowTextRecorder : public QProxyStyle {
public:
    mutable QStringList texts;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override {
        if (element == CE_ItemViewItem)
            if (const auto* item = qstyleoption_cast<const QStyleOptionViewItem*>(option))
                texts << item->text;
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};

TEST_CASE("the face list paints each row with its missing and confusable marks", "[text][round2]") {
    RowTextRecorder style;
    FacePicker p;
    p.list()->setStyle(&style);
    p.setText(U"SO50 é");
    p.resize(400, 400);
    p.show();
    QCoreApplication::processEvents();
    p.list()->viewport()->repaint();
    REQUIRE(faceCount(p.list()) == 3);
    for (int i = 0; i < faceCount(p.list()); ++i) {
        const QString label = faceLabel(p.list(), i);
        INFO(label.toStdString());
        CHECK(label.contains(QStringLiteral("missing é")));
        CHECK(style.texts.contains(label));
    }
    CHECK(style.texts.contains(QStringLiteral("mini\nmissing é · confusable S5 O0")));

    // a text the faces cover paints the bare names again
    style.texts.clear();
    p.setText(U"funk");
    p.list()->viewport()->repaint();
    CHECK(style.texts.contains(QStringLiteral("block")));
    for (const QString& text : std::as_const(style.texts))
        CHECK_FALSE(text.contains(QLatin1Char('\n')));
    p.list()->setStyle(nullptr);
}

TEST_CASE("Bake Text makes the text layer a cells layer, one undo step", "[text]") {
    MainWindow w;
    Document* d = w.document();
    QAction* bake = act(w, QStringLiteral("bakeText"));
    CHECK_FALSE(bake->isEnabled());  // the background isn't a text layer
    makeText(w, QStringLiteral("Hi"));
    REQUIRE(bake->isEnabled());
    const LayerId id = d->activeLayer();
    const Canvas cells = d->active()->cells;
    const TextSource source = sourceOf(d);
    const int steps = d->undoStack()->count();

    bake->trigger();
    CHECK(d->undoStack()->count() == steps + 1);
    const Layer* l = d->layers().find(id);
    REQUIRE(l);
    CHECK_FALSE(l->props.text);
    CHECK(l->props.kind == LayerKind::Cells);
    CHECK(l->cells == cells);
    CHECK_FALSE(bake->isEnabled());
    CHECK(textTool(w)->editing() == 0);
    // the edit is over: a key goes to keyboard mode, the layer kept baked
    w.canvasView()->setCellCursor({2, 1});
    type(w.canvasView(), QStringLiteral("z"));
    CHECK(textTool(w)->editing() == 0);
    CHECK_FALSE(d->layers().find(id)->props.text);
    CHECK(d->undoStack()->index() == steps + 2);
    CHECK(d->layers().find(id)->cells.at(0, 0).ch == U'z');
    d->undoStack()->undo();

    d->undoStack()->undo();
    l = d->layers().find(id);
    REQUIRE(l->props.text);
    CHECK(*l->props.text == source);
    CHECK(l->cells == cells);
    CHECK(bake->isEnabled());

    // the Layer menu has it
    QMenu* layer = nullptr;
    for (QAction* m : w.menuBar()->actions())
        if (m->text() == QStringLiteral("&Layer"))
            layer = m->menu();
    REQUIRE(layer);
    CHECK(layer->actions().contains(bake));
}

TEST_CASE("undo and redo end the run of typing and keep the text cursor in the text", "[text]") {
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral(" "));
    CHECK(sourceOf(d).text == U" ");
    // a space changes no cell the canvas shows: the undo stack, not the
    // cells, ends the run
    d->undoStack()->undo();
    CHECK(sourceOf(d).text.empty());
    CHECK(textTool(w)->caret() == 0);
    d->undoStack()->redo();
    CHECK(sourceOf(d).text == U" ");
    type(w.canvasView(), QStringLiteral("A"));
    CHECK(sourceOf(d).text == U"A ");
    d->undoStack()->undo();
    CHECK(sourceOf(d).text == U" ");
    CHECK(textTool(w)->caret() <= 1);
}

TEST_CASE("locking or hiding the text layer ends its edit", "[text]") {
    MainWindow w;
    Document* d = w.document();
    SECTION("locked") {
        makeText(w, QStringLiteral("A"));
        const LayerId id = d->activeLayer();
        LayerProperties p = d->active()->props;
        p.locked = true;
        REQUIRE(d->setLayerProperties(id, p, QStringLiteral("Lock")));
        CHECK(textTool(w)->editing() == 0);
        type(w.canvasView(), QStringLiteral("B"));
        CHECK(d->layers().find(id)->props.text->text == U"A");
    }
    SECTION("hidden") {
        // (an empty layer the edit made goes before the hide instead: "a
        // command from elsewhere takes back the empty layer...")
        makeText(w, QStringLiteral("A"));
        const LayerId id = d->activeLayer();
        REQUIRE(textTool(w)->editing() == id);
        REQUIRE(d->setLayerVisible(id, false));
        CHECK(textTool(w)->editing() == 0);
        type(w.canvasView(), QStringLiteral("B"));
        REQUIRE(d->layers().find(id));
        CHECK(d->layers().find(id)->props.text->text == U"A");
    }
}

TEST_CASE("an empty text layer the edit made goes with all its steps when the edit ends", "[text]") {
    MainWindow w;
    Document* d = w.document();
    const size_t layers = d->layers().size();
    const LayerId background = d->activeLayer();
    const int steps = d->undoStack()->count();
    use(w, "text");
    click(w.canvasView(), 2, 1);
    CHECK(d->layers().size() == layers);
    SECTION("typed and deleted, then Escape") {
        type(w.canvasView(), QStringLiteral("A"));
        key(w.canvasView(), Qt::Key_Backspace, QStringLiteral("\b"));
        key(w.canvasView(), Qt::Key_Escape);
    }
    SECTION("typed and undone, then Escape") {
        type(w.canvasView(), QStringLiteral("A"));
        d->undoStack()->undo();
        REQUIRE(d->layers().size() == layers + 1);
        key(w.canvasView(), Qt::Key_Escape);
    }
    SECTION("typed and deleted, then another tool") {
        type(w.canvasView(), QStringLiteral("A"));
        key(w.canvasView(), Qt::Key_Backspace, QStringLiteral("\b"));
        use(w, "keyboard");
    }
    SECTION("another layer made active") {
        REQUIRE(d->setActiveLayer(background));
    }
    SECTION("typed and deleted, then another layer made active") {
        type(w.canvasView(), QStringLiteral("A"));
        key(w.canvasView(), Qt::Key_Backspace, QStringLiteral("\b"));
        REQUIRE(d->setActiveLayer(background));
    }
    SECTION("typed and undone, then another layer made active") {
        type(w.canvasView(), QStringLiteral("A"));
        d->undoStack()->undo();
        REQUIRE(d->layers().size() == layers + 1);
        REQUIRE(d->setActiveLayer(background));
    }
    CHECK(textTool(w)->editing() == 0);
    CHECK(d->layers().size() == layers);
    CHECK(d->activeLayer() == background);
    CHECK(d->undoStack()->count() == steps);
    CHECK(d->undoStack()->index() == steps);
    CHECK_FALSE(d->undoStack()->canRedo());
}

TEST_CASE("an edit resumed on another layer keeps its text cursor after a click elsewhere", "[text]") {
    MainWindow w;
    Document* d = w.document();
    const LayerId background = d->activeLayer();
    makeText(w, QStringLiteral("AB"));
    const LayerId e = d->activeLayer();
    const size_t layers = d->layers().size();
    REQUIRE(d->setActiveLayer(background));
    const int steps = d->undoStack()->count();
    SECTION("nothing typed") { click(w.canvasView(), 40, 15); }
    SECTION("typed and deleted") {
        click(w.canvasView(), 40, 15);
        type(w.canvasView(), QStringLiteral("Q"));
        key(w.canvasView(), Qt::Key_Backspace, QStringLiteral("\b"));
    }
    REQUIRE(d->setActiveLayer(e));
    key(w.canvasView(), Qt::Key_Home);
    REQUIRE(textTool(w)->editing() == e);
    REQUIRE(textTool(w)->caret() == 0);
    QCoreApplication::processEvents();
    type(w.canvasView(), QStringLiteral("X"));
    CHECK(d->layers().find(e)->props.text->text == U"XAB");
    CHECK(d->activeLayer() == e);
    CHECK(d->layers().size() == layers);
    // the typing is the only step after the click's
    CHECK(d->undoStack()->count() == steps + 1);
    d->undoStack()->undo();
    CHECK(d->layers().find(e)->props.text->text == U"AB");
    CHECK(d->undoStack()->index() == steps);
}

TEST_CASE("a command between a click and the edit's end survives, and no empty layer is left", "[text]") {
    MainWindow w;
    Document* d = w.document();
    const LayerId background = d->activeLayer();
    const size_t layers = d->layers().size();
    const int steps = d->undoStack()->count();
    use(w, "text");
    click(w.canvasView(), 2, 1);
    REQUIRE(d->setActiveLayer(background));
    REQUIRE(d->renameLayer(background, QStringLiteral("Renamed")));
    QCoreApplication::processEvents();
    use(w, "keyboard");
    CHECK(d->layers().size() == layers);
    CHECK(d->activeLayer() == background);
    CHECK(d->layers().find(background)->props.name == "Renamed");
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->undoStack()->index() == steps + 1);
    d->undoStack()->undo();
    CHECK(d->layers().find(background)->props.name != "Renamed");
    CHECK(d->undoStack()->index() == steps);
}

TEST_CASE("a save or a new document between a click and the edit's end leaves no empty layer", "[text]") {
    MainWindow w;
    Document* d = w.document();
    const size_t layers = d->layers().size();
    const int steps = d->undoStack()->count();
    use(w, "text");
    SECTION("a save, then Escape") {
        QTemporaryDir tmp;
        REQUIRE(tmp.isValid());
        click(w.canvasView(), 2, 1);
        QString error;
        REQUIRE(d->save(tmp.filePath(QStringLiteral("a.ans")), d->encode(), &error));
        QCoreApplication::processEvents();
        key(w.canvasView(), Qt::Key_Escape);
        CHECK(d->layers().size() == layers);
        CHECK(d->undoStack()->count() == steps);
        CHECK_FALSE(d->isModified());
        CHECK(textTool(w)->editing() == 0);
    }
    SECTION("a save, then typing") {
        QTemporaryDir tmp;
        REQUIRE(tmp.isValid());
        click(w.canvasView(), 2, 1);
        QString error;
        REQUIRE(d->save(tmp.filePath(QStringLiteral("a.ans")), d->encode(), &error));
        QCoreApplication::processEvents();
        type(w.canvasView(), QStringLiteral("A"));
        CHECK(d->layers().size() == layers + 1);
        CHECK(sourceOf(d).text == U"A");
        CHECK(d->active()->props.x == 2);
        CHECK(d->isModified());
    }
    SECTION("a new document") {
        click(w.canvasView(), 2, 1);
        project::Project p = d->project();
        d->replace(std::move(p), QString(), FileForm::ProjectZip);
        const size_t replaced = d->layers().size();
        QCoreApplication::processEvents();
        CHECK(textTool(w)->editing() == 0);
        type(w.canvasView(), QStringLiteral("A"));
        CHECK(d->layers().size() == replaced);
        CHECK(textTool(w)->editing() == 0);
        CHECK_FALSE(d->active()->props.text);
    }
}

TEST_CASE("a click that moves the text cursor to the other line of a wrap starts a new run", "[text]") {
    MainWindow w;
    Document* d = w.document();
    use(w, "text");
    w.toolOptions()->textOptions()->wrapWidthBox()->setValue(16);  // two block letters a line
    makeText(w, QStringLiteral("AC"));
    key(w.canvasView(), Qt::Key_Left);
    type(w.canvasView(), QStringLiteral("B"));
    REQUIRE(sourceOf(d).text == U"ABC");
    REQUIRE(textTool(w)->caret() == 2);
    const int rows = blockFace().height() / 2;
    REQUIRE(textTool(w)->caretCell() == QPoint(2, 1 + rows));
    // the right half of B: the end of the first line, the same index
    click(w.canvasView(), 17, 1);
    REQUIRE(textTool(w)->caret() == 2);
    REQUIRE(textTool(w)->caretCell() == QPoint(2 + 16, 1));
    type(w.canvasView(), QStringLiteral("D"));
    CHECK(sourceOf(d).text == U"ABDC");
    d->undoStack()->undo();
    CHECK(sourceOf(d).text == U"ABC");
}

TEST_CASE("an input method's replacement with the same text moves the text cursor", "[text]") {
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral("AB"));
    key(w.canvasView(), Qt::Key_Home);
    key(w.canvasView(), Qt::Key_Right);
    REQUIRE(textTool(w)->caret() == 1);
    const int steps = d->undoStack()->count();
    QInputMethodEvent im;
    im.setCommitString(QStringLiteral("B"), 0, 1);
    QApplication::sendEvent(w.canvasView(), &im);
    CHECK(sourceOf(d).text == U"AB");
    CHECK(textTool(w)->caret() == 2);
    CHECK(d->undoStack()->count() == steps);
    type(w.canvasView(), QStringLiteral("X"));
    CHECK(sourceOf(d).text == U"ABX");
}

TEST_CASE("an input method's replacement replaces text before the text cursor", "[text]") {
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral("ab"));
    SECTION("a replacing commit") {
        QInputMethodEvent im;
        im.setCommitString(QStringLiteral("X"), -1, 1);
        QApplication::sendEvent(w.canvasView(), &im);
        CHECK(sourceOf(d).text == U"aX");
        CHECK(textTool(w)->caret() == 2);
    }
    SECTION("a deleting commit") {
        QInputMethodEvent im;
        im.setCommitString(QString(), -1, 1);
        QApplication::sendEvent(w.canvasView(), &im);
        CHECK(sourceOf(d).text == U"a");
        CHECK(textTool(w)->caret() == 1);
    }
    SECTION("offsets in UTF-16, a character outside the BMP two of them") {
        QInputMethodEvent add;
        add.setCommitString(QString::fromUcs4(U"\U0001F600"));
        QApplication::sendEvent(w.canvasView(), &add);
        CHECK(sourceOf(d).text == U"ab\U0001F600");
        QInputMethodEvent im;
        im.setCommitString(QStringLiteral("X"), -3, 3);
        QApplication::sendEvent(w.canvasView(), &im);
        CHECK(sourceOf(d).text == U"aX");
        CHECK(textTool(w)->caret() == 2);
    }
}

TEST_CASE("End and Home keep the text cursor on its line at a wrap", "[text]") {
    MainWindow w;
    Document* d = w.document();
    use(w, "text");
    w.toolOptions()->textOptions()->wrapWidthBox()->setValue(16);  // two block letters a line
    makeText(w, QStringLiteral("ABCD"));
    REQUIRE(sourceOf(d).wrapWidth == 16);
    for (int i = 0; i < 3; ++i)
        key(w.canvasView(), Qt::Key_Left);
    REQUIRE(textTool(w)->caret() == 1);
    const int rows = blockFace().height() / 2;
    key(w.canvasView(), Qt::Key_End);
    CHECK(textTool(w)->caret() == 2);
    CHECK(textTool(w)->caretCell() == QPoint(2 + 16, 1));  // after B, on the first line
    key(w.canvasView(), Qt::Key_End);
    CHECK(textTool(w)->caret() == 2);
    CHECK(textTool(w)->caretCell() == QPoint(2 + 16, 1));
    key(w.canvasView(), Qt::Key_Home);
    CHECK(textTool(w)->caret() == 0);
    // from the second line's start, Home stays there and End goes to its end
    key(w.canvasView(), Qt::Key_End);
    key(w.canvasView(), Qt::Key_Right);
    REQUIRE(textTool(w)->caret() == 3);
    key(w.canvasView(), Qt::Key_Left);
    key(w.canvasView(), Qt::Key_Home);
    CHECK(textTool(w)->caret() == 2);
    CHECK(textTool(w)->caretCell() == QPoint(2, 1 + rows));  // before C, on the second line
    key(w.canvasView(), Qt::Key_End);
    CHECK(textTool(w)->caret() == 4);
    // typing at the first line's end goes in there
    key(w.canvasView(), Qt::Key_Up);
    key(w.canvasView(), Qt::Key_End);
    type(w.canvasView(), QStringLiteral("x"));
    CHECK(sourceOf(d).text == U"ABxCD");
}

TEST_CASE("a new document ends the edit, the layer ids the same or not", "[text]") {
    MainWindow w;
    Document* d = w.document();
    const LayerId background = d->activeLayer();
    makeText(w, QStringLiteral("Hi"));
    const LayerId id = d->activeLayer();
    project::Project p = d->project();
    REQUIRE(p.layers.remove(background));
    d->replace(std::move(p), QString(), FileForm::ProjectZip);
    REQUIRE(d->activeLayer() == id);
    CHECK(textTool(w)->editing() == 0);
    CHECK(textTool(w)->caret() == 0);
    CHECK(d->undoStack()->count() == 0);
}

TEST_CASE("the face picker finds missing characters without laying out the text", "[text]") {
    FacePicker p;
    // wrapped at 16 columns this is a small layer; laid out unwrapped its
    // masks would be hundreds of MiB
    std::u32string text(2048, U'A');
    text.append(2048, U'\n');
    text += U'\u00e9';
    bwtest::largestAllocation = 4 << 20;
    p.setText(text);
    const QString missing = p.missingIn(pixel("vga"));
    bwtest::largestAllocation = 0;
    CHECK(missing == QStringLiteral("é"));
}

TEST_CASE("an input method's replacement with the same text after typing starts a new run", "[text]") {
    // Codex round 3 (#167): the caret moved without a new run, so the
    // typing before and after it was one undo step.
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral("AB"));
    key(w.canvasView(), Qt::Key_Home);
    type(w.canvasView(), QStringLiteral("X"));
    REQUIRE(sourceOf(d).text == U"XAB");
    REQUIRE(textTool(w)->caret() == 1);
    QInputMethodEvent im;
    im.setCommitString(QStringLiteral("B"), 1, 1);
    QApplication::sendEvent(w.canvasView(), &im);
    REQUIRE(sourceOf(d).text == U"XAB");
    REQUIRE(textTool(w)->caret() == 3);
    type(w.canvasView(), QStringLiteral("Y"));
    REQUIRE(sourceOf(d).text == U"XABY");
    d->undoStack()->undo();
    CHECK(sourceOf(d).text == U"XAB");
    d->undoStack()->undo();
    CHECK(sourceOf(d).text == U"AB");
}

TEST_CASE("a Tool Options change at a waiting text cursor shows in the status bar", "[text]") {
    // Codex round 3 (#167): the status kept the face before.
    MainWindow w;
    use(w, "text");
    auto* status = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(status);
    click(w.canvasView(), 2, 1);
    REQUIRE(textTool(w)->spot());
    REQUIRE(status->text() == QStringLiteral("Face: block"));
    int changes = 0;
    QObject::connect(textTool(w), &Tool::changed, [&] { ++changes; });
    QListView* list = w.toolOptions()->textOptions()->facePicker()->list();
    for (int i = 0; i < faceCount(list); ++i)
        if (faceLabel(list, i).startsWith(QStringLiteral("mini")))
            selectFaceRow(list, i);
    REQUIRE(textTool(w)->defaults().face == pixel("mini"));
    CHECK(status->text() == QStringLiteral("Face: mini"));
    CHECK(changes > 0);  // the canvas repaints the text cursor
    CHECK(w.document()->undoStack()->count() == 0);
}

TEST_CASE("a command from elsewhere takes back the empty layer the edit made before it runs", "[text]") {
    // Codex round 3 (#167): New Cells Layer was pushed after the empty
    // layer's steps, so the layer could no longer be taken back.
    MainWindow w;
    Document* d = w.document();
    const LayerId background = d->activeLayer();
    REQUIRE(d->addLayer(LayerKind::Cells));
    const LayerId cells = d->activeLayer();
    REQUIRE(d->setActiveLayer(background));
    const size_t layers = d->layers().size();
    const int steps = d->undoStack()->count();
    const std::vector<LayerId> order = [&] {
        std::vector<LayerId> ids;
        for (const Layer& l : d->layers().layers())
            ids.push_back(l.props.id);
        return ids;
    }();
    const auto ids = [&] {
        std::vector<LayerId> out;
        for (const Layer& l : d->layers().layers())
            out.push_back(l.props.id);
        return out;
    };
    use(w, "text");
    click(w.canvasView(), 2, 1);
    type(w.canvasView(), QStringLiteral("A"));
    key(w.canvasView(), Qt::Key_Backspace, QStringLiteral("\b"));
    const LayerId made = d->activeLayer();
    REQUIRE(made != background);
    REQUIRE(d->layers().size() == layers + 1);
    REQUIRE(d->layers().indexOf(made) == size_t(1));  // above the background, below the cells layer
    REQUIRE(sourceOf(d).text.empty());

    // what the command adds to the undo stack and to the layers: 0 for one
    // refused
    int added = 0, newLayers = 0;
    SECTION("New Cells Layer") {
        act(w, QStringLiteral("newCellsLayer"))->trigger();
        added = newLayers = 1;
        // just above the layer active before the edit, which undo makes active again
        CHECK(d->layers().indexOf(d->activeLayer()) == size_t(1));
        CHECK(d->active()->props.kind == LayerKind::Cells);
        CHECK(d->layers().size() == layers + 1);
        d->undoStack()->undo();
        CHECK(ids() == order);
        CHECK(d->activeLayer() == background);
        CHECK(d->undoStack()->index() == steps);
        d->undoStack()->redo();
    }
    SECTION("New Pixels Layer") {
        act(w, QStringLiteral("newPixelLayer"))->trigger();
        added = newLayers = 1;
        CHECK(d->active()->props.kind == LayerKind::Pixels);
    }
    SECTION("Delete Layer") {
        act(w, QStringLiteral("deleteLayer"))->trigger();
        CHECK(ids() == order);
    }
    SECTION("Raise Layer") {
        act(w, QStringLiteral("raiseLayer"))->trigger();
        CHECK(ids() == order);
    }
    SECTION("Lower Layer") {
        act(w, QStringLiteral("lowerLayer"))->trigger();
        CHECK(ids() == order);
    }
    SECTION("hidden in the Layers dock") {
        auto* tree = w.findChild<QTreeWidget*>(QStringLiteral("layerList"));
        REQUIRE(tree);
        QTreeWidgetItem* item = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (LayersDock::layerOf(tree->topLevelItem(i)) == made)
                item = tree->topLevelItem(i);
        REQUIRE(item);
        item->setCheckState(LayersDock::NameColumn, Qt::Unchecked);
        QCoreApplication::processEvents();
        CHECK(tree->topLevelItemCount() == int(layers));
    }
    SECTION("renamed") {
        CHECK_FALSE(d->renameLayer(made, QStringLiteral("Named")));
    }
    SECTION("Bake Text") {
        act(w, QStringLiteral("bakeText"))->trigger();
    }
    SECTION("SAUCE") {
        Sauce fields = Sauce::fresh(80, 25);
        fields.title = "Title";
        REQUIRE(d->setSauceFields(fields));
        added = 1;
    }
    SECTION("IRC budget") {
        project::IrcBudget budget = d->extras().ircBudget;
        budget.profile = "custom";
        budget.bytes = budget.bytes == 400 ? 300 : 400;
        REQUIRE(d->setIrcBudget(budget));
        added = 1;
    }
    QCoreApplication::processEvents();
    CHECK(textTool(w)->editing() == 0);
    CHECK_FALSE(d->layers().find(made));
    CHECK(d->layers().size() == layers + size_t(newLayers));
    CHECK(d->undoStack()->count() == steps + added);
    CHECK(d->undoStack()->index() == steps + added);
    CHECK_FALSE(d->undoStack()->canRedo());
    CHECK(d->layers().find(cells));
}

TEST_CASE("a command from elsewhere leaves an edit whose layer holds text", "[text]") {
    MainWindow w;
    Document* d = w.document();
    makeText(w, QStringLiteral("A"));
    const LayerId made = d->activeLayer();
    const int steps = d->undoStack()->count();
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    CHECK(d->layers().find(made));
    CHECK(d->layers().find(made)->props.text->text == U"A");
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(textTool(w)->editing() == 0);
}

namespace {

// A fonts folder in `tmp` holding bw-block.tdf.
QString blockFontFolder(const QTemporaryDir& tmp) {
    const QString dir = tmp.filePath(QStringLiteral("fonts"));
    REQUIRE(QDir().mkpath(dir));
    const std::string bytes = bwtest::readFile(bwtest::dataDir() / "tdf" / "bw-block.tdf");
    QFile f(dir + QStringLiteral("/bw-block.tdf"));
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(bytes.data(), qint64(bytes.size())) == qint64(bytes.size()));
    return dir;
}

int rowOf(const FacePicker* p, const QString& name) {
    const QStringList names = p->listed();
    for (int i = 0; i < names.size(); ++i)
        if (names[i] == name)
            return i;
    return -1;
}

} // namespace

TEST_CASE("the face picker lists the fonts folder's TheDraw fonts, and picking one sets the text layer in it",
          "[text][fonts]") {
    {
        const auto s = appSettings();
        s->remove(FacePicker::favouritesKey());
    }
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    Document* d = w.document();
    d->setFontsFolder(loadFontsFolder(blockFontFolder(tmp)));
    REQUIRE(d->fontsFolder());
    REQUIRE(d->fontsFolder()->fonts.size() == 1);
    const FontCatalogue::Font font = d->fontsFolder()->fonts.front();
    const QString name = QStringLiteral("BW Block (bw-block.tdf)");
    REQUIRE(QString::fromStdString(font.displayName) == name);

    makeText(w, QStringLiteral("funk é"));
    FacePicker* picker = w.toolOptions()->textOptions()->facePicker();
    // listed after the bundled faces, with a preview, searchable
    CHECK(picker->listed() ==
          QStringList{QStringLiteral("block"), QStringLiteral("vga"), QStringLiteral("mini"), name});
    CHECK_FALSE(faceIcon(picker->list(), 3).isNull());
    picker->searchField()->setText(QStringLiteral("bw b"));
    CHECK(picker->listed() == QStringList{name});
    picker->searchField()->clear();
    // marked as the bundled faces are
    CHECK(picker->missingIn(font.ref) == QStringLiteral("é"));
    CHECK(faceLabel(picker->list(), 3).contains(QStringLiteral("missing é")));
    CHECK(picker->confusablesIn(font.ref) ==
          [&] {
              QStringList pairs;
              for (const ConfusablePair& p : confusablesIn(*font.resolvedFace(), U"funk é"))
                  pairs << QString::fromUcs4(&p.a, 1) + QString::fromUcs4(&p.b, 1);
              return pairs.join(QLatin1Char(' '));
          }());
    // a favourite, by the font's file and place in it
    picker->setFavourite(font.ref, true);
    CHECK(rowOf(picker, name) == 0);
    CHECK(faceLabel(picker->list(), 0).startsWith(QStringLiteral("★ ")));
    picker->setFavourite(font.ref, false);
    REQUIRE(rowOf(picker, name) == 3);

    // picking it sets the layer in the font, one undo step, the file copied
    // into the project
    const TextSource before = sourceOf(d);
    const Canvas cellsBefore = d->active()->cells;
    const int steps = d->undoStack()->count();
    selectFaceRow(picker->list(), rowOf(picker, name));
    CHECK(sourceOf(d).face == font.ref);
    CHECK(d->undoStack()->count() == steps + 1);
    TextSource after = before;
    after.face = font.ref;
    CHECK(d->active()->cells == renderText(after, *font.resolvedFace()).cells);
    CHECK(d->extras().fonts.file(font.ref.file));
    CHECK(textTool(w)->status() == QStringLiteral("Face: BW Block"));
    // typing goes on in it
    type(w.canvasView(), QStringLiteral("y"));
    CHECK(sourceOf(d).text == U"funk éy");
    d->undoStack()->undo();

    d->undoStack()->undo();
    CHECK(sourceOf(d) == before);
    CHECK(d->active()->cells == cellsBefore);
    CHECK(picker->current() == pixel("block"));
    d->undoStack()->redo();
    CHECK(sourceOf(d).face == font.ref);
    CHECK(picker->current() == font.ref);

    // with the folder gone, the project's copy is still listed and draws
    d->setFontsFolder(nullptr);
    CHECK(picker->listed().contains(QStringLiteral("BW Block (in the project)")));
    CHECK(picker->current() == font.ref);
    type(w.canvasView(), QStringLiteral("z"));
    CHECK(sourceOf(d).text == U"funk éz");
    CHECK(d->active()->cells == [&] {
        TextSource s = after;
        s.text = U"funk éz";
        return renderText(s, *font.resolvedFace()).cells;
    }());
}

TEST_CASE("a new text layer starts in a TheDraw font picked at the waiting text cursor", "[text][fonts]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    Document* d = w.document();
    d->setFontsFolder(loadFontsFolder(blockFontFolder(tmp)));
    const FontCatalogue::Font font = d->fontsFolder()->fonts.front();
    use(w, "text");
    click(w.canvasView(), 2, 1);
    FacePicker* picker = w.toolOptions()->textOptions()->facePicker();
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("BW Block (bw-block.tdf)")));
    REQUIRE(textTool(w)->defaults().face == font.ref);
    CHECK(d->undoStack()->count() == 0);
    // Codex round 4 (#167): the folder cleared before the first keystroke
    // left the font in nothing the document looks in, and the keystroke made
    // no layer; the tool keeps the font it was picked from, still listed
    d->setFontsFolder(nullptr);
    CHECK(picker->current() == font.ref);
    CHECK(rowOf(picker, QStringLiteral("BW Block (kept for new text)")) >= 0);
    click(w.canvasView(), 2, 1);
    type(w.canvasView(), QStringLiteral("Ab"));
    REQUIRE(d->active()->props.text);
    CHECK(sourceOf(d).text == U"Ab");
    CHECK(sourceOf(d).face == font.ref);
    CHECK(d->extras().fonts.file(font.ref.file));
    TextSource s = sourceOf(d);
    CHECK(d->active()->cells == renderText(s, *font.resolvedFace()).cells);
    CHECK(textTool(w)->status() == QStringLiteral("Face: BW Block"));
    // the project's copy now lists it, once
    CHECK(rowOf(picker, QStringLiteral("BW Block (in the project)")) >= 0);
    CHECK(rowOf(picker, QStringLiteral("BW Block (kept for new text)")) < 0);
}

TEST_CASE("a TheDraw font picked for new text outlasts a fonts folder changed to one without it", "[text][fonts]") {
    QTemporaryDir tmp, other;
    REQUIRE(tmp.isValid());
    REQUIRE(other.isValid());
    MainWindow w;
    Document* d = w.document();
    d->setFontsFolder(loadFontsFolder(blockFontFolder(tmp)));
    const FontCatalogue::Font font = d->fontsFolder()->fonts.front();
    use(w, "text");
    click(w.canvasView(), 2, 1);
    FacePicker* picker = w.toolOptions()->textOptions()->facePicker();
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("BW Block (bw-block.tdf)")));
    REQUIRE(textTool(w)->defaults().face == font.ref);
    d->setFontsFolder(loadFontsFolder(other.path()));
    REQUIRE(d->fontsFolder());
    CHECK_FALSE(d->fontSources().file(font.ref.file));
    // the waiting text cursor still draws in it
    click(w.canvasView(), 2, 1);
    CHECK(textTool(w)->caretCell() == QPoint(2, 1));
    type(w.canvasView(), QStringLiteral("A"));
    REQUIRE(d->active()->props.text);
    CHECK(sourceOf(d).face == font.ref);
    CHECK(d->extras().fonts.file(font.ref.file));
}

TEST_CASE("a TheDraw font kept for new text leaves the face picker when another face is picked", "[text][fonts]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    Document* d = w.document();
    d->setFontsFolder(loadFontsFolder(blockFontFolder(tmp)));
    use(w, "text");
    click(w.canvasView(), 2, 1);
    FacePicker* picker = w.toolOptions()->textOptions()->facePicker();
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("BW Block (bw-block.tdf)")));
    d->setFontsFolder(nullptr);
    const QString kept = QStringLiteral("BW Block (kept for new text)");
    REQUIRE(rowOf(picker, kept) >= 0);
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("vga")));
    CHECK(textTool(w)->defaults().face == pixel("vga"));
    for (int i = 0; i < 5 && rowOf(picker, kept) >= 0; ++i)
        QCoreApplication::processEvents();
    CHECK(rowOf(picker, kept) < 0);
    // and the tool drops its file with the entry
    CHECK(textTool(w)->keptFonts().empty());
    CHECK(picker->current() == pixel("vga"));
    type(w.canvasView(), QStringLiteral("A"));
    REQUIRE(d->active()->props.text);
    CHECK(sourceOf(d).face == pixel("vga"));
}

TEST_CASE("a TheDraw font kept for new text can be picked again before the face picker lists the faces again",
          "[text][fonts]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    Document* d = w.document();
    d->setFontsFolder(loadFontsFolder(blockFontFolder(tmp)));
    const FontCatalogue::Font font = d->fontsFolder()->fonts.front();
    use(w, "text");
    click(w.canvasView(), 2, 1);
    FacePicker* picker = w.toolOptions()->textOptions()->facePicker();
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("BW Block (bw-block.tdf)")));
    d->setFontsFolder(nullptr);
    const QString kept = QStringLiteral("BW Block (kept for new text)");
    REQUIRE(rowOf(picker, kept) >= 0);
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("vga")));
    CHECK(textTool(w)->defaults().face == pixel("vga"));
    // Codex round 5 (#167): the kept row, still listed until the picker
    // lists the faces again, picked again before that found the file nowhere
    REQUIRE(rowOf(picker, kept) >= 0);
    CHECK(textTool(w)->keptFonts().file(font.ref.file));
    selectFaceRow(picker->list(), rowOf(picker, kept));
    REQUIRE(textTool(w)->defaults().face == font.ref);
    QCoreApplication::processEvents();
    CHECK(rowOf(picker, kept) >= 0);
    CHECK(picker->current() == font.ref);
    type(w.canvasView(), QStringLiteral("A"));
    REQUIRE(d->active()->props.text);
    CHECK(sourceOf(d).face == font.ref);
    CHECK(d->extras().fonts.file(font.ref.file));
    CHECK(d->active()->cells == renderText(sourceOf(d), *font.resolvedFace()).cells);
}

TEST_CASE("a TheDraw font kept for new text can be picked for a text layer", "[text][fonts]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    Document* d = w.document();
    d->setFontsFolder(loadFontsFolder(blockFontFolder(tmp)));
    const FontCatalogue::Font font = d->fontsFolder()->fonts.front();
    FacePicker* picker = w.toolOptions()->textOptions()->facePicker();
    makeText(w, QStringLiteral("A"));
    const TextSource before = sourceOf(d);
    REQUIRE(before.face.kind != FaceRef::Kind::Tdf);
    // the font picked for new text at a waiting text cursor, then the folder
    // cleared and the cursor taken away: the layer's face shows again
    click(w.canvasView(), 20, 10);
    REQUIRE(textTool(w)->spot());
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("BW Block (bw-block.tdf)")));
    d->setFontsFolder(nullptr);
    key(w.canvasView(), Qt::Key_Escape);
    REQUIRE_FALSE(textTool(w)->spot());
    REQUIRE(sourceOf(d) == before);
    CHECK(picker->current() == before.face);
    const QString kept = QStringLiteral("BW Block (kept for new text)");
    REQUIRE(rowOf(picker, kept) >= 0);
    CHECK(d->extras().fonts.empty());
    // Codex round 5 (#167): picking it set nothing, the file being in
    // nothing the document looked in
    const int steps = d->undoStack()->count();
    selectFaceRow(picker->list(), rowOf(picker, kept));
    CHECK(sourceOf(d).face == font.ref);
    CHECK(sourceOf(d).text == U"A");
    CHECK(d->undoStack()->count() == steps + 1);
    CHECK(d->extras().fonts.file(font.ref.file));
    CHECK(d->active()->cells == renderText(sourceOf(d), *font.resolvedFace()).cells);
    QCoreApplication::processEvents();
    CHECK(picker->current() == font.ref);
    CHECK(rowOf(picker, QStringLiteral("BW Block (in the project)")) >= 0);
    CHECK(rowOf(picker, kept) < 0);
    // one undo step takes it back
    d->undoStack()->undo();
    CHECK(sourceOf(d) == before);
}

TEST_CASE("a project's copy of a TheDraw font picked for new text outlasts a new document", "[text][fonts]") {
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    MainWindow w;
    Document* d = w.document();
    d->setFontsFolder(loadFontsFolder(blockFontFolder(tmp)));
    const FontCatalogue::Font font = d->fontsFolder()->fonts.front();
    FacePicker* picker = w.toolOptions()->textOptions()->facePicker();
    // a layer in the font copies it into the project; then the folder goes
    makeText(w, QStringLiteral("A"));
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("BW Block (bw-block.tdf)")));
    REQUIRE(sourceOf(d).face == font.ref);
    textTool(w)->endEdit();
    d->setFontsFolder(nullptr);
    // the project's copy picked at a waiting text cursor
    click(w.canvasView(), 20, 10);
    selectFaceRow(picker->list(), rowOf(picker, QStringLiteral("BW Block (in the project)")));
    REQUIRE(textTool(w)->defaults().face == font.ref);
    // a new document holds no copy, and no folder is set
    d->clear();
    REQUIRE(d->extras().fonts.empty());
    CHECK(picker->current() == font.ref);
    CHECK(rowOf(picker, QStringLiteral("BW Block (kept for new text)")) >= 0);
    click(w.canvasView(), 2, 1);
    type(w.canvasView(), QStringLiteral("A"));
    REQUIRE(d->active()->props.text);
    CHECK(sourceOf(d).face == font.ref);
    CHECK(d->extras().fonts.file(font.ref.file));
}

TEST_CASE("the face picker paints only requested previews and keeps them cached", "[text][fonts]") {
    std::vector<FaceEntry> entries;
    for (int i = 0; i < 200; ++i) {
        FaceRef ref;
        ref.name = "face " + std::to_string(i);
        entries.push_back({ref, ref.name, bundledFaces().front(), {}, "pixel"});
    }
    FacePicker picker(entries);
    CHECK(faceCount(picker.list()) == 200);
    CHECK(picker.previewsBuilt() == 0);
    CHECK_FALSE(faceIcon(picker.list(), 0).isNull());
    CHECK(picker.previewsBuilt() == 1);
    CHECK_FALSE(faceIcon(picker.list(), 0).isNull());
    CHECK(picker.previewsBuilt() == 1);
    picker.setEntries(entries);
    CHECK(picker.previewsBuilt() == 1);
    entries.pop_back();
    picker.setEntries(entries);
    CHECK(faceCount(picker.list()) == 199);
    CHECK_FALSE(faceIcon(picker.list(), 0).isNull());
    CHECK(picker.previewsBuilt() == 1);
}

TEST_CASE("an input-method commit with the tool on a text layer edits it, from its end", "[text]") {
    // Codex round 6 (#167): the commit fell through to typing in cells,
    // which the text layer refused, and was lost.
    MainWindow w;
    Document* d = w.document();
    const LayerId background = d->activeLayer();
    makeText(w, QStringLiteral("A"));
    use(w, "keyboard");
    use(w, "text");
    REQUIRE(textTool(w)->editing() == 0);
    REQUIRE_FALSE(textTool(w)->spot());
    QInputMethodEvent im;
    im.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(w.canvasView(), &im);
    CHECK(sourceOf(d).text == U"Aé");
    CHECK(textTool(w)->editing() == d->activeLayer());
    CHECK(textTool(w)->caret() == 2);
    // on a layer that isn't text it types in the cells, as before
    use(w, "keyboard");
    REQUIRE(d->setActiveLayer(background));
    use(w, "text");
    const Cursor at = w.canvasView()->cellCursor();
    QInputMethodEvent cell;
    cell.setCommitString(QStringLiteral("é"));
    QApplication::sendEvent(w.canvasView(), &cell);
    CHECK(textTool(w)->editing() == 0);
    CHECK(d->layers().find(background)->cells.at(at.x, at.y).ch == U'é');
}

TEST_CASE("the input method is told where the text cursor is", "[text]") {
    // Codex round 6 (#167): it was told the cell cursor's place, and not
    // told when the text cursor moved.
    MainWindow w;
    CanvasView* v = w.canvasView();
    const LayerId background = w.document()->activeLayer();
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    v->setFocus();
    REQUIRE(QApplication::focusObject() == v);
    const int rows = blockFace().height() / 2;
    // the text cursor's cells, a column the height of a line
    const auto caret = [&] {
        const QPoint at = textTool(w)->caretCell();
        return v->cellRect(at).united(v->cellRect(at + QPoint(0, rows - 1)));
    };
    const auto told = [&] { return v->inputMethodQuery(Qt::ImCursorRectangle).toRect(); };
    int updates = 0;
    QObject::connect(QGuiApplication::inputMethod(), &QInputMethod::cursorRectangleChanged, &w, [&] { ++updates; });

    // at a waiting text cursor
    use(w, "text");
    updates = 0;
    click(v, 30, 10);
    REQUIRE(textTool(w)->spot());
    CHECK(told() == caret());
    CHECK(updates > 0);
    // typing, and the arrows
    updates = 0;
    type(v, QStringLiteral("AB"));
    CHECK(told() == caret());
    CHECK(updates > 0);
    const QRect end = told();
    updates = 0;
    key(v, Qt::Key_Home);
    CHECK(told() == caret());
    CHECK(told() != end);
    CHECK(updates > 0);
    // with the edit over and the tool on the layer, where a key or a commit
    // would go: its end
    use(w, "keyboard");
    CHECK(told() == v->cellRect(v->cellCursor()));
    updates = 0;
    use(w, "text");
    REQUIRE(textTool(w)->editing() == 0);
    CHECK(told() == end);
    CHECK(updates > 0);
    // another active layer, with no edit open: the cell cursor
    updates = 0;
    REQUIRE(w.document()->setActiveLayer(background));
    CHECK(told() == v->cellRect(v->cellCursor()));
    CHECK(updates > 0);
}

TEST_CASE("a key that edits nothing neither starts an edit nor changes one", "[text]") {
    // Codex round 7 (#167): Ctrl, Shift, Alt or Meta pressed alone with the
    // tool on a text layer started editing it, and a Ctrl+Backspace after it
    // deleted a character. Round 8: so did Mode_switch, with its own flag or
    // none, and Kana_Lock.
    MainWindow w;
    Document* d = w.document();
    CanvasView* v = w.canvasView();
    makeText(w, QStringLiteral("AB"));
    // a modifier's own press may carry its flag or not
    const std::pair<int, Qt::KeyboardModifiers> presses[] = {
        {Qt::Key_Control, Qt::ControlModifier}, {Qt::Key_Control, Qt::NoModifier},
        {Qt::Key_Shift, Qt::ShiftModifier},     {Qt::Key_Shift, Qt::NoModifier},
        {Qt::Key_Alt, Qt::AltModifier},         {Qt::Key_Alt, Qt::NoModifier},
        {Qt::Key_Meta, Qt::MetaModifier},       {Qt::Key_Meta, Qt::NoModifier},
        {Qt::Key_AltGr, Qt::NoModifier},        {Qt::Key_AltGr, Qt::GroupSwitchModifier},
        {Qt::Key_Mode_switch, Qt::NoModifier},  {Qt::Key_Mode_switch, Qt::GroupSwitchModifier},
        {Qt::Key_Kana_Lock, Qt::NoModifier},    {Qt::Key_CapsLock, Qt::NoModifier},
        {Qt::Key_NumLock, Qt::NoModifier},      {Qt::Key_ScrollLock, Qt::NoModifier},
        {Qt::Key_Super_L, Qt::NoModifier},      {Qt::Key_Super_R, Qt::NoModifier},
        {Qt::Key_Hyper_L, Qt::NoModifier},      {Qt::Key_Hyper_R, Qt::NoModifier},
        {Qt::Key_Hangul, Qt::NoModifier},       {Qt::Key_Zenkaku_Hankaku, Qt::NoModifier},
        {Qt::Key_Eisu_toggle, Qt::NoModifier},  {Qt::Key_Dead_Acute, Qt::NoModifier},
        {Qt::Key_Multi_key, Qt::NoModifier},    {Qt::Key_F5, Qt::NoModifier},
        {Qt::Key_Print, Qt::NoModifier},       {Qt::Key_Insert, Qt::NoModifier},
        {Qt::Key_Left, Qt::AltModifier},        {Qt::Key_Home, Qt::AltModifier},
    };
    for (const auto& [k, mods] : presses) {
        INFO("key " << k << ", modifiers " << int(mods));
        use(w, "keyboard");
        use(w, "text");
        REQUIRE(textTool(w)->editing() == 0);
        key(v, k, {}, mods);
        CHECK(textTool(w)->editing() == 0);
        key(v, Qt::Key_Backspace, {}, Qt::ControlModifier);
        CHECK(sourceOf(d).text == U"AB");
    }
    // in an open edit it leaves the text, the cursor and the run alone
    key(v, Qt::Key_End);
    REQUIRE(textTool(w)->editing() == d->activeLayer());
    const LayerId id = d->activeLayer();
    const int steps = d->undoStack()->count();
    type(v, QStringLiteral("C"));
    for (const auto& [k, mods] : presses) {
        INFO("key " << k << ", modifiers " << int(mods));
        key(v, k, {}, mods);
        CHECK(textTool(w)->editing() == id);
        CHECK(textTool(w)->caret() == 3);
        CHECK(sourceOf(d).text == U"ABC");
    }
    type(v, QStringLiteral("D"));
    CHECK(sourceOf(d).text == U"ABCD");
    CHECK(d->undoStack()->count() == steps + 1);
    // a key that types or deletes still starts one, at the end of the text
    use(w, "keyboard");
    use(w, "text");
    REQUIRE(textTool(w)->editing() == 0);
    type(v, QStringLiteral("E"));
    CHECK(textTool(w)->editing() == id);
    CHECK(sourceOf(d).text == U"ABCDE");
    use(w, "keyboard");
    use(w, "text");
    REQUIRE(textTool(w)->editing() == 0);
    key(v, Qt::Key_Backspace, QStringLiteral("\b"));
    CHECK(textTool(w)->editing() == id);
    CHECK(sourceOf(d).text == U"ABCD");
}

TEST_CASE("big text options create each style and undo colours and modes", "[text][big-text]") {
    MainWindow w;
    w.show();
    use(w, "text");
    auto* d = w.document();
    auto* o = w.toolOptions()->textOptions();
    auto* tool = textTool(w);
    REQUIRE(o->bigStyleBox()->count() == 5);
    click(w.canvasView(), 2, 2);
    for (const auto style :
         {BigTextStyle::Shadow, BigTextStyle::ThreeD, BigTextStyle::Outline, BigTextStyle::Block}) {
        o->bigStyleBox()->setCurrentIndex(o->bigStyleBox()->findData(int(style)));
        CHECK(tool->defaults().bigStyle == style);
        CHECK_FALSE(o->facePicker()->isEnabled());
    }
    type(w.canvasView(), QStringLiteral("420"));
    REQUIRE(d->active());
    REQUIRE(sourceOf(d).bigStyle == BigTextStyle::Block);
    const auto rainbow = *d->active();
    CHECK(rainbow.cells == renderText(sourceOf(d), blockFace()).cells);
    CHECK(tool->caretRect()->height() == 8 * w.canvasView()->cellRect(QPoint(0, 0)).height());
    const int steps = d->undoStack()->count();
    o->bigColoursBox()->setText(QStringLiteral("ice"));
    QMetaObject::invokeMethod(o->bigColoursBox(), "editingFinished", Qt::DirectConnection);
    CHECK(sourceOf(d).bigColours == std::vector<std::uint8_t>{0, 11, 12, 2, 12, 11});
    CHECK(d->undoStack()->count() == steps + 1);
    d->undoStack()->undo();
    CHECK(*d->active() == rainbow);
    CHECK(o->bigColoursBox()->text() == QStringLiteral("0"));
    d->undoStack()->redo();
    CHECK(o->bigColoursBox()->text() == QStringLiteral("0,11,12,2,12,11"));
    const auto beforeBad = *d->active();
    o->bigColoursBox()->setText(QStringLiteral("16"));
    QMetaObject::invokeMethod(o->bigColoursBox(), "editingFinished", Qt::DirectConnection);
    CHECK(*d->active() == beforeBad);
    o->bigStyleBox()->setCurrentIndex(o->bigStyleBox()->findData(int(BigTextStyle::None)));
    CHECK(sourceOf(d).bigStyle == BigTextStyle::None);
    CHECK(o->facePicker()->isEnabled());
    CHECK(o->colourBox()->isEnabled());
    CHECK_FALSE(o->bigColoursBox()->isEnabled());
    d->undoStack()->undo();
    CHECK(*d->active() == beforeBad);
}

TEST_CASE("big text clicks stay on the first line above a low glyph", "[text][big-text][line-hit]") {
    MainWindow w;
    use(w, "text");
    auto* v = w.canvasView();
    auto* d = w.document();
    auto* o = w.toolOptions()->textOptions();
    o->bigStyleBox()->setCurrentIndex(o->bigStyleBox()->findData(int(BigTextStyle::Block)));
    click(v, 2, 2);
    type(v, QStringLiteral("A"));
    key(v, Qt::Key_Return);
    type(v, QStringLiteral("_"));
    REQUIRE(sourceOf(d).text == U"A\n_");
    REQUIRE(sourceOf(d).letterSpacing == 0);
    REQUIRE(sourceOf(d).lineSpacing == 0);
    REQUIRE(sourceOf(d).wrapWidth == 0);
    REQUIRE(sourceOf(d).align == TextAlign::Left);
    REQUIRE_FALSE(d->active()->cells.at(0, 2).transparent);  // the A's left stroke
    click(v, 2, 4);
    CHECK(textTool(w)->caret() == 0);
    CHECK(textTool(w)->caretCell() == QPoint(2, 2));
    type(v, QStringLiteral("B"));
    CHECK(sourceOf(d).text == U"BA\n_");
}

TEST_CASE("big text clicks use line boxes across gaps and below the last line", "[text][big-text][line-hit]") {
    for (const int spacing : {0, 3}) {
        INFO("line spacing " << spacing);
        MainWindow w;
        use(w, "text");
        auto* v = w.canvasView();
        auto* d = w.document();
        auto* tool = textTool(w);
        auto* o = w.toolOptions()->textOptions();
        o->bigStyleBox()->setCurrentIndex(o->bigStyleBox()->findData(int(BigTextStyle::Block)));
        o->lineSpacingBox()->setValue(spacing);
        click(v, 2, 2);
        type(v, QStringLiteral("A"));
        key(v, Qt::Key_Return);
        type(v, QStringLiteral("_"));
        REQUIRE(sourceOf(d).text == U"A\n_");
        const LayerId id = d->activeLayer();
        const size_t layers = d->layers().size();
        // A occupies rows 0..6, the gap 7..7+spacing, and _ row 8+spacing.
        for (int row = 7; row <= 7 + spacing; ++row) {
            click(v, 2, 2 + row);
            CHECK(tool->caret() == 0);
            CHECK(tool->caretCell() == QPoint(2, 2));
        }
        click(v, 2, 10 + spacing);
        CHECK(tool->caret() == 2);
        // The low glyph's cursor keeps the untrimmed eight-row font origin.
        CHECK(tool->caretCell() == QPoint(2, 3 + spacing));
        key(v, Qt::Key_Up);
        CHECK(tool->caret() == 0);
        CHECK(tool->caretCell() == QPoint(2, 2));
        key(v, Qt::Key_Down);
        CHECK(tool->caret() == 2);
        CHECK(tool->caretCell() == QPoint(2, 3 + spacing));
        // A click below the tight layer starts a new text cursor.
        click(v, 2, 11 + spacing);
        CHECK(tool->editing() == 0);
        CHECK(tool->spot() == QPoint(2, 11 + spacing));
        type(v, QStringLiteral("B"));
        CHECK(d->layers().size() == layers + 1);
        CHECK(d->activeLayer() != id);
        CHECK(sourceOf(d).text == U"B");
        CHECK(d->layers().find(id)->props.text->text == U"A\n_");
    }
}

TEST_CASE("wrapped big text clicks and vertical arrows use the visible line", "[text][big-text][line-hit]") {
    for (const auto style : {BigTextStyle::Block, BigTextStyle::Shadow, BigTextStyle::ThreeD,
                             BigTextStyle::Outline}) {
        INFO(bigTextStyleName(style));
        MainWindow w;
        use(w, "text");
        auto* v = w.canvasView();
        auto* d = w.document();
        auto* tool = textTool(w);
        auto* o = w.toolOptions()->textOptions();
        o->bigStyleBox()->setCurrentIndex(o->bigStyleBox()->findData(int(style)));
        o->wrapWidthBox()->setValue(10);  // one eight-column glyph plus the effect room
        click(v, 2, 2);
        type(v, QStringLiteral("A_"));
        REQUIRE(sourceOf(d).text == U"A_");
        const auto drawn = renderBigText(sourceOf(d));
        REQUIRE(drawn.lines.size() == 2);
        click(v, 3, 4);
        CHECK(tool->caret() == 0);
        key(v, Qt::Key_Down);
        CHECK(tool->caret() == 1);
        CHECK(tool->caretCell() == QPoint(2 + drawn.lines[1].left, 2 + drawn.lines[1].top));
        key(v, Qt::Key_Up);
        CHECK(tool->caret() == 0);
        CHECK(tool->caretCell() == QPoint(2 + drawn.lines[0].left, 2 + drawn.lines[0].top));
        type(v, QStringLiteral("B"));
        CHECK(sourceOf(d).text == U"BA_");
    }
}

TEST_CASE("TheDraw text line clicks retain native boundaries with and without wrapping", "[text][fonts][line-hit]") {
    const auto folder = loadFontsFolder(QString::fromStdString((bwtest::dataDir() / "tdf").string()));
    REQUIRE(folder);
    REQUIRE_FALSE(folder->fonts.empty());
    for (const auto& font : folder->fonts)
        for (const bool wrapped : {false, true}) {
            INFO(font.displayName << " wrapped " << wrapped);
            MainWindow w;
            auto* v = w.canvasView();
            auto* d = w.document();
            auto* tool = textTool(w);
            d->setFontsFolder(folder);
            use(w, "text");
            TextSource s;
            s.face = font.ref;
            s.lineSpacing = 3;
            s.wrapWidth = wrapped ? 1 : 0;
            tool->setSettings(s, QStringLiteral("Text settings"), d);
            click(v, 2, 2);
            type(v, QStringLiteral("A"));
            if (!wrapped)
                key(v, Qt::Key_Return);
            type(v, QStringLiteral("_"));
            REQUIRE(sourceOf(d).text == (wrapped ? U"A_" : U"A\n_"));
            const int rows = font.resolvedFace()->height() / 2;
            REQUIRE(rows >= 3);
            click(v, 2, 3);  // inside A
            CHECK(tool->caret() == 0);
            click(v, 2, 2 + rows + 2);  // last row of the gap belongs to the first line
            CHECK(tool->caret() == 0);
            click(v, 2, 2 + rows + 3);  // second line starts here
            CHECK(tool->caret() == (wrapped ? 1 : 2));
            CHECK(tool->caretCell() == QPoint(2, 2 + rows + 3));
            key(v, Qt::Key_Up);
            CHECK(tool->caret() == 0);
            CHECK(tool->caretCell() == QPoint(2, 2));
            type(v, QStringLiteral("B"));
            CHECK(sourceOf(d).text == (wrapped ? U"BA_" : U"BA\n_"));
        }
}
