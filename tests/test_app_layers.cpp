// The layer panel and editing the active layer (PLAN §5, Phase 3b): the
// Layers dock and the Layer menu, each change an undo step, the tools
// drawing on the active layer, erase, pixel layers, the alpha lock and
// sample merged.

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "layers_dock.h"
#include "main_window.h"
#include "scratch_settings.h"
#include "select_tool.h"
#include "tool.h"
#include "tool_options.h"

#include "blockwright/ansi.h"
#include "blockwright/composite.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/project.h"
#include "blockwright/ramp.h"
#include "blockwright/text.h"
#include "blockwright/text_style.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QScrollArea>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>

#include <limits>
#include <type_traits>

using namespace bw;

namespace {

// Counts the focus-outs `watched` gets while `doc` is alive and after it
// is gone. Installed after the item delegate's filter, it sees each one
// before the delegate commits on it.
struct FocusOutProbe : QObject {
    FocusOutProbe(QWidget* watched, Document* doc) {
        watched->installEventFilter(this);
        connect(doc, &QObject::destroyed, this, [this] { docGone = true; });
    }
    bool eventFilter(QObject*, QEvent* e) override {
        if (e->type() == QEvent::FocusOut)
            ++(docGone ? late : early);
        return false;
    }
    bool docGone = false;
    int early = 0;
    int late = 0;
};

// Opens the active layer's rename editor in a shown, active window (the
// Layers dock's own when it floats) and types `text` over the name, as a
// user would. The editor keeps the focus.
QLineEdit* typeInRenameEditor(MainWindow& w, const QString& text) {
    w.resize(900, 700);
    w.show();
    QWidget* active = w.layersDock()->isFloating() ? static_cast<QWidget*>(w.layersDock()) : &w;
    active->activateWindow();
    QApplication::processEvents();
    REQUIRE(QApplication::activeWindow() == active);
    QTreeWidget* tree = w.layersWidget()->tree();
    QTreeWidgetItem* item = tree->currentItem() ? tree->currentItem() : tree->topLevelItem(0);
    REQUIRE(item);
    tree->editItem(item, LayersDock::NameColumn);
    QApplication::processEvents();
    auto* editor = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    REQUIRE(editor);
    REQUIRE(tree->isAncestorOf(editor));
    editor->selectAll();
    for (QChar ch : text) {
        QKeyEvent press(QEvent::KeyPress, ch.toUpper().unicode(), Qt::NoModifier, QString(ch));
        QApplication::sendEvent(editor, &press);
        QKeyEvent release(QEvent::KeyRelease, ch.toUpper().unicode(), Qt::NoModifier, QString(ch));
        QApplication::sendEvent(editor, &release);
    }
    REQUIRE(editor->text() == text);
    return editor;
}

// A window that closes saves its layout, and the next window opens with
// it. Forgets that layout, from the scratch settings only.
struct ForgetLayout {
    static QString dir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }
    ForgetLayout() { REQUIRE(bwtest::settingsInScratch(dir())); }
    ~ForgetLayout() {
        if (!bwtest::settingsInScratch(dir()))
            return;
        bw::appSettings()->remove(QStringLiteral("window/geometry"));
        bw::appSettings()->remove(QStringLiteral("window/state"));
    }
};

// Answers the next message box with `button`; `asked` says one came.
void answer(QWidget* context, QMessageBox::StandardButton button, bool* asked) {
    *asked = false;
    QTimer::singleShot(0, context, [=] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box)
            return;
        *asked = true;
        box->button(button)->click();
    });
}

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

// The view pixel in the middle of half-block pixel (x, py).
QPointF pixel(CanvasView* v, int x, int py) {
    const QRect r = v->cellRect(QPoint(x, py >> 1));
    return QPointF(r.center().x(), r.top() + r.height() * ((py & 1) ? 0.75 : 0.25));
}

void mouse(CanvasView* v, QEvent::Type type, int x, int py, Qt::MouseButton button = Qt::LeftButton) {
    const QPointF at = pixel(v, x, py);
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held,
                  Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}

void drag(CanvasView* v, std::initializer_list<QPoint> points, Qt::MouseButton button = Qt::LeftButton) {
    const QPoint* p = points.begin();
    mouse(v, QEvent::MouseButtonPress, p->x(), p->y(), button);
    for (++p; p != points.end(); ++p)
        mouse(v, QEvent::MouseMove, p->x(), p->y(), button);
    const QPoint last = *(points.end() - 1);
    mouse(v, QEvent::MouseButtonRelease, last.x(), last.y(), button);
}

void key(QWidget* w, int k, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, Qt::NoModifier, text);
    QApplication::sendEvent(w, &release);
}

// Types `ch` at cell (x, y) in keyboard mode.
void typeAt(MainWindow& w, int x, int y, QChar ch) {
    act(w, QStringLiteral("keyboardTool"))->trigger();
    w.canvasView()->setCellCursor({x, y});
    key(w.canvasView(), ch.toUpper().unicode(), QString(ch));
}

void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

QStringList rowNames(const LayersDock* d) {
    QStringList names;
    for (int i = 0; i < d->tree()->topLevelItemCount(); ++i)
        names << d->tree()->topLevelItem(i)->text(LayersDock::NameColumn);
    return names;
}

QTreeWidgetItem* rowOf(const LayersDock* d, LayerId id) {
    for (int i = 0; i < d->tree()->topLevelItemCount(); ++i)
        if (LayersDock::layerOf(d->tree()->topLevelItem(i)) == id)
            return d->tree()->topLevelItem(i);
    return nullptr;
}

std::size_t indexOf(Document* doc, LayerId id) { return doc->layers().indexOf(id).value(); }

const Layer& layerById(const Document* doc, LayerId id) {
    const Layer* l = doc->layers().find(id);
    REQUIRE(l);
    return *l;
}

void checkKindHelp(LayerKind kind, const QString& tip) {
    INFO("kind: " << layerKindName(kind));
    REQUIRE_FALSE(tip.isEmpty());
    QStringList terms;
    switch (kind) {
    case LayerKind::Cells:
        terms = {QStringLiteral("glyph plus foreground and background"), QStringLiteral("Keyboard (typing)"),
                 QStringLiteral("Glyph Brush"), QStringLiteral("shade and ramp brushes"),
                 QStringLiteral("pixel tools"), QStringLiteral("transparent cells show through"),
                 QStringLiteral("Exports flatten")};
        break;
    case LayerKind::Pixels:
        terms = {QStringLiteral("two half-block colours per cell"), QStringLiteral("Half-Block Brush"),
                 QStringLiteral("Line, Rectangle, Ellipse or Fill"), QStringLiteral("space, █, ▀ or ▄"),
                 QStringLiteral("shade-producing tools and other glyphs are refused"),
                 QStringLiteral("transparent halves show through"), QStringLiteral("Text layers stay in front"),
                 QStringLiteral("Exports flatten")};
        break;
    case LayerKind::Text:
        terms = {QStringLiteral("text rendered as glyph cells"), QStringLiteral("foreground and background"),
                 QStringLiteral("Text tool"), QStringLiteral("painting tools cannot change it"),
                 QStringLiteral("Without a live text source"), QStringLiteral("cannot be covered by Pixels layers"),
                 QStringLiteral("Exports flatten")};
        break;
    case LayerKind::Generator:
        terms = {QStringLiteral("cached output as glyph cells"), QStringLiteral("foreground and background"),
                 QStringLiteral("parameters and Run"), QStringLiteral("painting tools cannot edit"),
                 QStringLiteral("composite like a Cells layer"), QStringLiteral("export"),
                 QStringLiteral("does not run the generator")};
        break;
    case LayerKind::Reference:
        terms = {QStringLiteral("picture to trace"), QStringLiteral("saved with the project"),
                 QStringLiteral("Painting tools cannot edit"),
                 QStringLiteral("skipped by the compositor"), QStringLiteral("never exported")};
        break;
    default: FAIL("Add help expectations for the new core layer kind");
    }
    for (const QString& term : terms) {
        INFO("required explanation: " << term.toStdString());
        CHECK(tip.contains(term));
    }
}

} // namespace

TEST_CASE("layer creation actions and buttons explain what their kind holds", "[layers][help]") {
    MainWindow w;
    LayersDock* d = w.layersWidget();
    int adding = 0;
    // Include any future add action in the dock, not just today's two accessors.
    for (QAction* a : d->findChildren<QAction*>()) {
        if (!a->objectName().startsWith(QStringLiteral("new")) ||
            !a->objectName().endsWith(QStringLiteral("Layer")))
            continue;
        INFO(a->objectName().toStdString());
        ++adding;
        a->trigger();
        const LayerKind kind = w.document()->active()->props.kind;
        checkKindHelp(kind, a->toolTip());
        CHECK(a->toolTip().contains(QStringLiteral("above the active layer")));
        auto* button = d->findChild<QToolButton*>(a->objectName() + QStringLiteral("Button"));
        REQUIRE(button);
        CHECK(button->defaultAction() == a);
        CHECK(button->toolTip() == a->toolTip());
        auto* row = rowOf(d, w.document()->activeLayer());
        REQUIRE(row);
        checkKindHelp(kind, row->toolTip(LayersDock::KindColumn));
        CHECK(a->toolTip().contains(row->toolTip(LayersDock::KindColumn)));
    }
    REQUIRE(adding >= 2);
}

TEST_CASE("every core layer kind has explanatory help in the kind column", "[layers][help]") {
    MainWindow w;
    LayersDock* d = w.layersWidget();
    int kinds = 0;
    // LayerKind has a fixed uint8_t underlying type and no Count sentinel.
    // Ask the core which values have stable names, so a future serialized
    // kind is tested even when it is added after Reference or leaves a gap.
    using KindValue = std::underlying_type_t<LayerKind>;
    for (unsigned value = 0; value <= std::numeric_limits<KindValue>::max(); ++value) {
        const auto kind = static_cast<LayerKind>(value);
        if (layerKindName(kind).empty())
            continue;
        ++kinds;
        const LayerId id = w.document()->addLayer(kind);
        auto* row = rowOf(d, id);
        REQUIRE(row);
        CHECK_FALSE(row->text(LayersDock::KindColumn).isEmpty());
        checkKindHelp(kind, row->toolTip(LayersDock::KindColumn));
    }
    REQUIRE(kinds >= 5);
}

TEST_CASE("the Layers dock's hint tells Cells and Pixels apart", "[layers][help]") {
    MainWindow w;
    LayersDock* d = w.layersWidget();
    auto* hint = d->findChild<QLabel*>(QStringLiteral("layerKindHint"));
    REQUIRE(hint);
    CHECK(hint->isVisibleTo(d));
    CHECK(hint->wordWrap());
    CHECK(hint->text().contains(QStringLiteral("Cells: glyphs and shades")));
    CHECK(hint->text().contains(QStringLiteral("Pixels: half-block drawing")));
}

TEST_CASE("the add actions and their undo steps name the kind as the kind column does", "[layers][help]") {
    MainWindow w;
    LayersDock* d = w.layersWidget();
    const struct {
        QAction* action;
        QString kind, text, undo;
    } adds[] = {
        {d->newCellsAction(), QStringLiteral("Cells"), QStringLiteral("New &Cells Layer"),
         QStringLiteral("New Cells Layer")},
        {d->newPixelsAction(), QStringLiteral("Pixels"), QStringLiteral("New &Pixels Layer"),
         QStringLiteral("New Pixels Layer")},
    };
    for (const auto& add : adds) {
        INFO(add.kind.toStdString());
        CHECK(add.action->text() == add.text);
        CHECK(add.action->iconText() == QStringLiteral("+ ") + add.kind);
        add.action->trigger();
        CHECK(w.document()->undoStack()->undoText() == add.undo);
        auto* row = rowOf(d, w.document()->activeLayer());
        REQUIRE(row);
        CHECK(row->text(LayersDock::KindColumn) == add.kind);
        CHECK(add.action->toolTip().startsWith(QStringLiteral("Add a %1 layer").arg(add.kind)));
    }
}

TEST_CASE("the Layer menu shows the add actions' explanations", "[layers][help]") {
    MainWindow w;
    LayersDock* d = w.layersWidget();
    QMenu* layerMenu = nullptr;
    for (QAction* m : w.menuBar()->actions())
        if (m->menu() && m->text() == QStringLiteral("&Layer"))
            layerMenu = m->menu();
    REQUIRE(layerMenu);
    // QMenu hides tooltips unless asked to show them
    CHECK(layerMenu->toolTipsVisible());
    const struct {
        QAction* action;
        QString status;
    } adds[] = {
        {d->newCellsAction(), QStringLiteral("Add a Cells layer above the active layer: glyphs and shades")},
        {d->newPixelsAction(), QStringLiteral("Add a Pixels layer above the active layer: half-block drawing")},
    };
    for (const auto& add : adds) {
        INFO(add.action->objectName().toStdString());
        CHECK(layerMenu->actions().contains(add.action));
        CHECK_FALSE(add.action->toolTip().isEmpty());
        CHECK(add.action->statusTip() == add.status);
        // hovering the item puts it in the status bar: QMenu sends the tip
        // to the menu bar that opened it
        w.statusBar()->clearMessage();
        add.action->showStatusText(w.menuBar());
        CHECK(w.statusBar()->currentMessage() == add.status);
    }
}

TEST_CASE("the Layers dock lists the layers top first, the active one current", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    LayersDock* d = w.layersWidget();
    REQUIRE(d);
    auto* scroll = qobject_cast<QScrollArea*>(w.layersDock()->widget());
    REQUIRE(scroll);
    CHECK(scroll->widget() == d);
    CHECK(rowNames(d) == QStringList{QStringLiteral("Background")});
    CHECK(LayersDock::layerOf(d->tree()->currentItem()) == doc->activeLayer());
    // one layer: it can't be deleted or moved
    CHECK_FALSE(d->deleteAction()->isEnabled());
    CHECK_FALSE(d->raiseAction()->isEnabled());
    CHECK_FALSE(d->lowerAction()->isEnabled());

    const LayerId bg = doc->activeLayer();
    d->newCellsAction()->trigger();
    const LayerId cells = doc->activeLayer();
    CHECK(cells != bg);
    d->newPixelsAction()->trigger();
    const LayerId pixels = doc->activeLayer();
    CHECK(doc->active()->props.kind == LayerKind::Pixels);
    CHECK(rowNames(d) == QStringList{QStringLiteral("Layer 2"), QStringLiteral("Layer 1"), QStringLiteral("Background")});
    CHECK(d->tree()->topLevelItem(0)->text(LayersDock::KindColumn) == QStringLiteral("Pixels"));
    CHECK(d->tree()->topLevelItem(1)->text(LayersDock::KindColumn) == QStringLiteral("Cells"));
    CHECK(LayersDock::layerOf(d->tree()->currentItem()) == pixels);
    // a new layer is the canvas's size and all transparent
    CHECK(doc->active()->bounds() == CellRect{0, 0, doc->canvas().width(), doc->canvas().height()});
    CHECK(doc->active()->cells.at(3, 3) == transparentCell());
    CHECK(d->deleteAction()->isEnabled());
    CHECK_FALSE(d->raiseAction()->isEnabled());
    CHECK(d->lowerAction()->isEnabled());

    // picking a row makes its layer active, and it is no undo step
    const int steps = doc->undoStack()->count();
    d->tree()->setCurrentItem(rowOf(d, bg));
    CHECK(doc->activeLayer() == bg);
    CHECK(doc->undoStack()->count() == steps);
    CHECK_FALSE(d->lowerAction()->isEnabled());
    // a new layer goes just above the active one
    d->newCellsAction()->trigger();
    CHECK(indexOf(doc, doc->activeLayer()) == 1);
    CHECK(rowNames(d).back() == QStringLiteral("Background"));
    CHECK(rowNames(d).at(2) == QStringLiteral("Layer 3"));
    doc->undoStack()->undo();
    CHECK(doc->activeLayer() == bg);
    CHECK(doc->layers().size() == 3);
    CHECK(d->tree()->topLevelItemCount() == 3);
}

TEST_CASE("adding, deleting and moving layers are undo steps", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    LayersDock* d = w.layersWidget();
    QUndoStack* undo = doc->undoStack();
    const LayerId bg = doc->activeLayer();

    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId a = doc->activeLayer();
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("New Cells Layer"));
    CHECK(doc->isModified());
    undo->undo();
    CHECK(doc->layers().size() == 1);
    CHECK(doc->activeLayer() == bg);
    CHECK(rowNames(d) == QStringList{QStringLiteral("Background")});
    undo->redo();
    CHECK(doc->activeLayer() == a);
    CHECK(doc->layers().size() == 2);

    // delete: the layer below becomes active; undo puts it back, active
    typeAt(w, 0, 0, QLatin1Char('B'));
    act(w, QStringLiteral("deleteLayer"))->trigger();
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Delete Layer"));
    CHECK(doc->layers().size() == 1);
    CHECK(doc->activeLayer() == bg);
    CHECK(doc->canvas().at(0, 0) == Cell{});
    undo->undo();
    CHECK(doc->activeLayer() == a);
    CHECK(indexOf(doc, a) == 1);
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    // the last layer can't go
    doc->setActiveLayer(bg);
    CHECK(doc->removeLayer(a));
    CHECK_FALSE(doc->removeLayer(bg));
    CHECK_FALSE(act(w, QStringLiteral("deleteLayer"))->isEnabled());
    undo->undo();

    // raise and lower
    doc->setActiveLayer(bg);
    act(w, QStringLiteral("raiseLayer"))->trigger();
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Raise Layer"));
    CHECK(indexOf(doc, bg) == 1);
    CHECK(rowNames(d) == QStringList{QStringLiteral("Background"), QStringLiteral("Layer 1")});
    CHECK(doc->canvas().at(0, 0) == Cell{});  // the opaque background now covers the B
    act(w, QStringLiteral("lowerLayer"))->trigger();
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Lower Layer"));
    CHECK(indexOf(doc, bg) == 0);
    undo->undo();
    CHECK(indexOf(doc, bg) == 1);
    undo->undo();
    CHECK(indexOf(doc, bg) == 0);
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    CHECK_FALSE(doc->moveLayer(bg, 0));  // already there
}

TEST_CASE("renaming, hiding and alpha-locking a layer in the dock are undo steps", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    LayersDock* d = w.layersWidget();
    QUndoStack* undo = doc->undoStack();
    typeAt(w, 0, 0, QLatin1Char('A'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId top = doc->activeLayer();
    typeAt(w, 0, 0, QLatin1Char('B'));
    REQUIRE(doc->canvas().at(0, 0).ch == U'B');

    QTreeWidgetItem* row = rowOf(d, top);
    REQUIRE(row);
    row->setText(LayersDock::NameColumn, QStringLiteral("  ink "));
    CHECK(doc->active()->props.name == "ink");
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Rename Layer"));
    CHECK(rowOf(d, top)->text(LayersDock::NameColumn) == QStringLiteral("ink"));
    // an empty name is refused and the row put back
    const int steps = undo->count();
    rowOf(d, top)->setText(LayersDock::NameColumn, QStringLiteral("   "));
    CHECK(undo->count() == steps);
    CHECK(rowOf(d, top)->text(LayersDock::NameColumn) == QStringLiteral("ink"));

    // hiding it shows what is under it
    rowOf(d, top)->setCheckState(LayersDock::NameColumn, Qt::Unchecked);
    CHECK_FALSE(doc->active()->props.visible);
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Hide Layer"));
    CHECK(doc->canvas().at(0, 0).ch == U'A');
    undo->undo();
    CHECK(doc->active()->props.visible);
    CHECK(rowOf(d, top)->checkState(LayersDock::NameColumn) == Qt::Checked);
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    undo->undo();
    CHECK(doc->active()->props.name == "Layer 1");
    CHECK(rowOf(d, top)->text(LayersDock::NameColumn) == QStringLiteral("Layer 1"));

    // the alpha lock is the layer's, and Tool Options shows the active one's
    use(w, "halfBlock");
    QCheckBox* lock = w.toolOptions()->alphaLockBox();
    CHECK_FALSE(lock->isChecked());
    rowOf(d, top)->setCheckState(LayersDock::AlphaLockColumn, Qt::Checked);
    CHECK(doc->active()->props.alphaLocked);
    CHECK(undo->text(undo->index() - 1) == QStringLiteral("Alpha Lock On"));
    CHECK(lock->isChecked());
    CHECK(w.canvasView()->lockTransparent());
    const LayerId bg = doc->layers()[0].props.id;
    d->tree()->setCurrentItem(rowOf(d, bg));
    CHECK_FALSE(lock->isChecked());
    CHECK_FALSE(w.canvasView()->lockTransparent());
    lock->setChecked(true);
    CHECK(doc->layers()[0].props.alphaLocked);
    CHECK(rowOf(d, bg)->checkState(LayersDock::AlphaLockColumn) == Qt::Checked);
    undo->undo();
    CHECK_FALSE(doc->layers()[0].props.alphaLocked);
    CHECK_FALSE(lock->isChecked());
}

TEST_CASE("the tools draw on the active layer, and erasing shows the layer beneath", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));
    typeAt(w, 1, 0, QLatin1Char('a'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    typeAt(w, 0, 0, QLatin1Char('B'));
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    CHECK(doc->layers()[0].cells.at(0, 0).ch == U'A');
    CHECK(doc->layerCanvas().at(0, 0).ch == U'B');
    CHECK(doc->layerCanvas().at(1, 0) == transparentCell());
    CHECK(doc->canvas().at(1, 0).ch == U'a');  // the layer's hole shows the background

    // Delete clears the top layer's cell to transparent: the A shows again
    v->setCellCursor({0, 0});
    key(v, Qt::Key_Delete);
    CHECK(doc->layerCanvas().at(0, 0) == transparentCell());
    CHECK(doc->canvas().at(0, 0).ch == U'A');
    doc->undoStack()->undo();
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    CHECK(doc->layers()[0].cells.at(0, 0).ch == U'A');

    // the eraser (the glyph brush's right button) leaves the same hole
    use(w, "glyph");
    drag(v, {{0, 0}}, Qt::RightButton);
    CHECK(doc->layerCanvas().at(0, 0) == transparentCell());
    CHECK(doc->canvas().at(0, 0).ch == U'A');

    // on the bottom layer, erasing leaves a blank cell, as before layers
    doc->setActiveLayer(doc->layers()[0].props.id);
    CHECK(doc->blankCell() == Cell{});
    v->setCellCursor({1, 0});
    act(w, QStringLiteral("keyboardTool"))->trigger();
    key(v, Qt::Key_Delete);
    CHECK(doc->layers()[0].cells.at(1, 0) == Cell{});
    CHECK(doc->canvas().at(1, 0) == Cell{});
}

TEST_CASE("a pixel layer keeps each pixel's transparency and refuses glyphs", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 2, 0, QLatin1Char('Z'));
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "halfBlock");
    v->setForeground(LightGreen);
    drag(v, {{2, 1}, {4, 1}});  // the bottom halves of row 0
    for (int x = 2; x <= 4; ++x) {
        INFO(x);
        CHECK(layerPixels(doc->active()->cells.at(x, 0)) == LayerPixels{std::nullopt, LightGreen});
        CHECK(doc->active()->cells.at(x, 0).transparent);
    }
    // the composite: the pixel below, the background's own above
    const auto halves = halfBlocks(doc->canvas().at(3, 0));
    REQUIRE(halves);
    CHECK(halves->bottom == LightGreen);
    CHECK(halves->top == Black);

    // a pixel layer holds no glyph: typing and a shade do nothing
    const int steps = doc->undoStack()->count();
    const Canvas before = doc->layerCanvas();
    typeAt(w, 6, 0, QLatin1Char('Q'));
    use(w, "shade");
    drag(v, {{6, 0}, {8, 0}});
    CHECK(doc->undoStack()->count() == steps);
    CHECK(doc->layerCanvas() == before);
    // the glyph brush's █ is two pixels, which it can hold
    use(w, "glyph");
    v->setForeground(Red);
    drag(v, {{6, 0}});
    CHECK(layerPixels(doc->active()->cells.at(6, 0)) == LayerPixels{Red, Red});

    // the glyph brush's eraser clears a cell of pixels back to transparent
    drag(v, {{3, 0}}, Qt::RightButton);
    CHECK(doc->active()->cells.at(3, 0) == transparentCell());
    CHECK(doc->canvas().at(3, 0) == Cell{});
}

TEST_CASE("a hidden or locked layer takes no edits", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId top = doc->activeLayer();
    REQUIRE(doc->editable());
    REQUIRE(doc->setLayerVisible(top, false));
    CHECK_FALSE(doc->editable());
    const int steps = doc->undoStack()->count();
    typeAt(w, 0, 0, QLatin1Char('X'));
    CHECK(doc->undoStack()->count() == steps);
    CHECK(doc->layerCanvas().at(0, 0) == transparentCell());
    doc->undoStack()->undo();
    CHECK(doc->editable());

    LayerProperties p = doc->active()->props;
    p.locked = true;
    REQUIRE(doc->setLayerProperties(top, p, QStringLiteral("Lock Layer")));
    CHECK_FALSE(doc->editable());
    typeAt(w, 0, 0, QLatin1Char('X'));
    CHECK(doc->layerCanvas().at(0, 0) == transparentCell());
    CHECK(doc->canvas().at(0, 0) == Cell{});
}

TEST_CASE("a live text layer is redrawn from its source, not edited, and its source undoes", "[layers][text]") {
    MainWindow w;
    Document* doc = w.document();
    const LayerId id = doc->addLayer(LayerKind::Text);
    REQUIRE(doc->editable());  // no source: cells read as text
    const Layer plain = *doc->active();

    TextSource source;
    source.text = U"Hi";
    source.colour = Yellow;
    LayerProperties p = plain.props;
    p.text = source;
    const int steps = doc->undoStack()->count();
    REQUIRE(doc->setLayerProperties(id, p, QStringLiteral("Text")));
    CHECK(doc->undoStack()->count() == steps + 1);
    CHECK_FALSE(doc->editable());
    const Layer live = *doc->active();
    CHECK(live.cells == renderText(source, blockFace()).cells);
    // the composite shows the drawn text
    bool inked = false;
    for (int y = 0; y < live.height(); ++y)
        for (int x = 0; x < live.width(); ++x)
            if (!live.cells.at(x, y).transparent) {
                CHECK(doc->canvas().at(x, y) == live.cells.at(x, y));
                inked = true;
            }
    CHECK(inked);

    // typing on it changes nothing
    typeAt(w, 0, 0, QLatin1Char('X'));
    CHECK(*doc->active() == live);
    CHECK(doc->undoStack()->count() == steps + 1);

    // a source this build can't draw, or on a cells layer, is refused
    LayerProperties bad = live.props;
    bad.text->face.name = "nope";
    CHECK_FALSE(doc->setLayerProperties(id, bad, QStringLiteral("Text")));
    bad = live.props;
    bad.kind = LayerKind::Cells;
    CHECK_FALSE(doc->setLayerProperties(id, bad, QStringLiteral("Text")));
    CHECK(doc->undoStack()->count() == steps + 1);

    // a new text redraws; undo gives each source and its cells back
    LayerProperties next = live.props;
    next.text->text = U"Hi there";
    REQUIRE(doc->setLayerProperties(id, next, QStringLiteral("Text")));
    CHECK(doc->active()->width() > live.width());
    doc->undoStack()->undo();
    CHECK(*doc->active() == live);
    doc->undoStack()->undo();
    CHECK(*doc->active() == plain);
    CHECK(doc->canvas().at(0, 0) == Cell{});
    doc->undoStack()->redo();
    CHECK(*doc->active() == live);
}

// Codex round 1: a live text layer saved with "A", its source then edited to
// "B" in the project directory. It opens with the cells it saved, and a
// change that leaves the source as it is keeps them, as does the undo of any
// step.
TEST_CASE("an opened text layer keeps its saved cells until its source changes", "[layers][text]") {
    TextSource a;
    a.text = U"A";
    project::Project p;
    p.layers = LayerStack(20, 6);
    const LayerId id = p.layers.add(makeTextLayer(a, "Title", 2, 1));
    const Canvas cachedA = p.layers[0].cells;
    QTemporaryDir tmp;
    const QString dir = tmp.filePath(QStringLiteral("piece"));
    project::writeDirectory(p, dir.toStdString());
    {
        QFile f(dir + QStringLiteral("/layers/%1.json").arg(id));
        REQUIRE(f.open(QIODevice::ReadOnly));
        QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QJsonObject src = o.value("source").toObject();
        src["text"] = "B";
        o["source"] = src;
        REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(o).toJson());
    }

    MainWindow w;
    QString error;
    REQUIRE(w.openFile(dir, &error));
    Document* doc = w.document();
    const auto shows = [&](const Canvas& cells) {
        CHECK(doc->layers().find(id)->cells == cells);
        CHECK(doc->canvas() == composite(doc->layers(), {doc->project().ice, Cell{}}));
        bool inked = false;
        for (int y = 0; y < cells.height(); ++y)
            for (int x = 0; x < cells.width(); ++x)
                if (!cells.at(x, y).transparent) {
                    CHECK(doc->canvas().at(2 + x, 1 + y) == cells.at(x, y));
                    inked = true;
                }
        CHECK(inked);
    };
    const Layer opened = *doc->layers().find(id);
    REQUIRE(opened.props.text);
    CHECK(opened.props.text->text == U"B");
    shows(cachedA);

    // a rename, and its undo and redo, keep the saved cells
    REQUIRE(doc->renameLayer(id, QStringLiteral("Renamed")));
    shows(cachedA);
    doc->undoStack()->undo();
    CHECK(*doc->layers().find(id) == opened);
    shows(cachedA);
    doc->undoStack()->redo();
    shows(cachedA);
    doc->undoStack()->undo();

    // a new source redraws, in the same box; its undo puts the saved cells back
    TextSource c = *opened.props.text;
    c.text = U"C";
    LayerProperties next = opened.props;
    next.text = c;
    REQUIRE(doc->setLayerProperties(id, next, QStringLiteral("Text")));
    const Canvas drawnC = renderText(c, blockFace()).cells;
    REQUIRE(drawnC.width() == cachedA.width());
    REQUIRE(drawnC.height() == cachedA.height());
    REQUIRE(drawnC != cachedA);
    shows(drawnC);
    doc->undoStack()->undo();
    CHECK(*doc->layers().find(id) == opened);
    shows(cachedA);

    // a source too large to draw is refused before it is drawn
    TextSource big = c;
    big.text = std::u32string(2048, U'W') + std::u32string(2048, U'\n');
    big.face.name = "vga";
    big.letterSpacing = 64;
    next.text = big;
    CHECK_FALSE(doc->setLayerProperties(id, next, QStringLiteral("Text")));

    // and saved, the file has the cells it opened with
    REQUIRE(doc->saveProject(dir, FileForm::ProjectDirectory, &error));
    const project::Project back = project::read(dir.toStdString());
    CHECK(back.layers.find(id)->cells == cachedA);
}

TEST_CASE("sample merged: the eyedropper and fill read the active layer or the composite", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    // a red block on the background at (5, 0)
    Cell red;
    red.ch = kFullBlock;
    red.fg = Color::indexed(Red);
    REQUIRE(doc->edit(CellEdit{{{5, 0, Cell{}, red}}}));
    act(w, QStringLiteral("newPixelLayer"))->trigger();

    use(w, "eyedropper");
    QCheckBox* merged = w.toolOptions()->sampleMergedBox();
    CHECK(merged->isVisibleTo(w.toolOptions()));
    CHECK_FALSE(merged->isChecked());
    v->setForeground(White);
    mouse(v, QEvent::MouseButtonPress, 5, 0);
    mouse(v, QEvent::MouseButtonRelease, 5, 0);
    CHECK(v->pen().fg == Color::indexed(White));  // the layer is clear there
    merged->setChecked(true);
    CHECK(v->sampleMerged());
    mouse(v, QEvent::MouseButtonPress, 5, 0);
    mouse(v, QEvent::MouseButtonRelease, 5, 0);
    CHECK(v->pen().fg == Color::indexed(Red));

    // fill on the layer: merged, the red block bounds the black region
    use(w, "fill");
    CHECK(merged->isVisibleTo(w.toolOptions()));
    v->setForeground(Blue);
    drag(v, {{0, 0}});
    CHECK(layerPixels(doc->active()->cells.at(0, 0)) == LayerPixels{Blue, Blue});
    CHECK(doc->active()->cells.at(5, 0) == transparentCell());
    doc->undoStack()->undo();
    // not merged, the clear pixels of the layer are one region
    merged->setChecked(false);
    drag(v, {{0, 0}});
    CHECK(layerPixels(doc->active()->cells.at(5, 0)) == LayerPixels{Blue, Blue});
    CHECK(doc->layers()[0].cells.at(5, 0) == red);
}

TEST_CASE("shade under a pixel layer composites with no specks, drawn with the tools", "[layers][golden]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    const LayerId bg = doc->activeLayer();
    // a band of shade on the background, built up a rung a pass: ░ in
    // columns 0-2, ▒ in 3-5, ▓ in 6-9
    use(w, "shade");
    v->setForeground(Blue);
    v->setBackground(Black);
    drag(v, {{0, 0}, {9, 0}});
    drag(v, {{3, 0}, {9, 0}});
    drag(v, {{6, 0}, {9, 0}});
    const Canvas shaded = doc->layers()[0].cells;
    REQUIRE(shaded.at(0, 0).ch == kShadeLight);
    REQUIRE(shaded.at(3, 0).ch == kShadeMedium);
    REQUIRE(shaded.at(6, 0).ch == kShadeDark);

    // The cells of `shown` the line cuts that are specks: the covered half
    // must be the pixel and the free half the colour the band's shade
    // mostly shows (░ its background, ▒ its darker colour,
    // here the black background, on top, ▓ and █ their foreground), never
    // the fallback a flat plot leaves.
    const bool ice = doc->ansi().ice;
    const auto shows = [](const Cell& shade) {
        REQUIRE(shade.bg == Color::indexed(Black));
        return shade.ch == kShadeDark || shade.ch == kFullBlock ? shade.fg.paletteIndex() : std::uint8_t(Black);
    };
    const auto specks = [&](const Canvas& band, const Canvas& shown) {
        int n = 0;
        for (int x = 0; x < 10; ++x) {
            const auto halves = halfBlocks(shown.at(x, 0), ice);
            if (!halves || halves->bottom != Yellow || halves->top != shows(band.at(x, 0)))
                ++n;
        }
        return n;
    };
    // the check catches the specks: the same yellow line plotted flat into
    // the band leaves the ▓ cells' fallback, black, where blue should show
    Canvas flat = shaded;
    for (int x = 0; x < 10; ++x)
        flat.at(x, 0) = setHalfBlock(flat.at(x, 0), true, Yellow, ice);
    CHECK(specks(shaded, flat) == 4);

    // a yellow line of pixels on a pixel layer above it, cutting the band's
    // cells in half
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "halfBlock");
    v->setForeground(Yellow);
    drag(v, {{0, 1}, {9, 1}});
    // the band is untouched: the pixels went to their own layer
    CHECK(doc->layers()[0].cells == shaded);
    const auto check = [&] {
        CHECK(specks(doc->layers()[0].cells, doc->canvas()) == 0);
        CompositeOptions o;
        o.ice = ice;
        CHECK(composite(doc->layers(), o) == doc->canvas());
    };
    check();

    // shading the background again under the pixels leaves them alone
    doc->setActiveLayer(bg);
    use(w, "shade");
    v->setForeground(LightBlue);
    drag(v, {{0, 0}, {9, 0}});
    CHECK_FALSE(doc->layers()[0].cells == shaded);
    for (int x = 0; x < 10; ++x)
        CHECK(layerPixels(doc->layers()[1].cells.at(x, 0)) == LayerPixels{std::nullopt, Yellow});
    check();
}

TEST_CASE("a drag in progress lands on its layer before the layers change", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId first = doc->activeLayer();
    use(w, "shade");
    mouse(v, QEvent::MouseButtonPress, 0, 0);
    mouse(v, QEvent::MouseMove, 4, 0);
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    mouse(v, QEvent::MouseButtonRelease, 4, 0);
    const Layer* l = doc->layers().find(first);
    REQUIRE(l);
    CHECK(l->cells.at(2, 0).ch != transparentCell().ch);
    CHECK_FALSE(l->cells.at(2, 0).transparent);
    CHECK(doc->active()->cells.at(2, 0) == transparentCell());
}

TEST_CASE("the Layer menu holds the dock's actions, with no shortcuts", "[layers]") {
    MainWindow w;
    QMenu* menu = nullptr;
    for (QAction* m : w.menuBar()->actions())
        if (m->text() == QStringLiteral("&Layer"))
            menu = m->menu();
    REQUIRE(menu);
    LayersDock* d = w.layersWidget();
    for (QAction* a : {d->newCellsAction(), d->newPixelsAction(), d->deleteAction(), d->raiseAction(),
                       d->lowerAction()}) {
        INFO(a->objectName().toStdString());
        CHECK(menu->actions().contains(a));
        CHECK(a->shortcut().isEmpty());
        CHECK_FALSE(a->toolTip().isEmpty());
    }
}

TEST_CASE("saving writes the visible composite", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    // lower case: the SAUCE record has capitals of its own
    typeAt(w, 0, 0, QLatin1Char('q'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    typeAt(w, 0, 0, QLatin1Char('z'));
    std::string bytes = doc->encode().bytes;
    CHECK(bytes.find('z') != std::string::npos);
    CHECK(bytes.find('q') == std::string::npos);
    doc->setLayerVisible(doc->activeLayer(), false);
    bytes = doc->encode().bytes;
    CHECK(bytes.find('q') != std::string::npos);
    CHECK(bytes.find('z') == std::string::npos);
}

TEST_CASE("moving a half pixel on a pixel layer keeps the pixel under its clear half", "[layers][pixels][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "halfBlock");
    v->setForeground(Blue);
    drag(v, {{3, 0}, {3, 1}});  // both pixels of cell (3, 0)
    v->setForeground(Red);
    drag(v, {{0, 0}});  // the top pixel of cell (0, 0) only
    REQUIRE(layerPixels(doc->active()->cells.at(3, 0)) == LayerPixels{Blue, Blue});
    REQUIRE(layerPixels(doc->active()->cells.at(0, 0)) == LayerPixels{Red, std::nullopt});

    // lift the red pixel and move it onto the blue cell: it lands on top,
    // and the clear bottom half leaves the blue pixel under it
    use(w, "select");
    v->setSelection(Selection(CellRect{0, 0, 1, 1}));
    for (int i = 0; i < 3; ++i)
        key(v, Qt::Key_Right);
    key(v, Qt::Key_Return);
    CHECK(layerPixels(doc->active()->cells.at(3, 0)) == LayerPixels{Red, Blue});
    CHECK(doc->active()->cells.at(0, 0) == transparentCell());
}

TEST_CASE("the alpha lock keeps the glyph brush's erase off opaque cells and set pixels", "[layers][pixels]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    typeAt(w, 0, 0, QLatin1Char('B'));
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    use(w, "glyph");
    drag(v, {{0, 0}}, Qt::RightButton);
    // cells layer: the opaque B stays opaque under the lock
    CHECK_FALSE(doc->active()->cells.at(0, 0).transparent);
    CHECK(doc->canvas().at(0, 0).ch == U'B');

    // pixel layer: the lock is per pixel, so set pixels stay set
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    use(w, "halfBlock");
    v->setForeground(Blue);
    drag(v, {{2, 0}, {2, 1}});
    REQUIRE(layerPixels(doc->active()->cells.at(2, 0)) == LayerPixels{Blue, Blue});
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    use(w, "glyph");
    drag(v, {{2, 0}}, Qt::RightButton);
    CHECK(layerPixels(doc->active()->cells.at(2, 0)) == LayerPixels{Blue, Blue});

    // unlocked, the same erase clears the cell
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), false));
    drag(v, {{2, 0}}, Qt::RightButton);
    CHECK(doc->active()->cells.at(2, 0) == transparentCell());
}

TEST_CASE("a sample-merged fill on a cells layer changes only the region it found", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    // background cell (0, 0): red top pixel, blue bottom pixel
    Cell two;
    two.ch = kUpperHalf;
    two.fg = Color::indexed(Red);
    two.bg = Color::indexed(Blue);
    REQUIRE(doc->edit(CellEdit{{{0, 0, Cell{}, two}}}));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    use(w, "fill");
    w.toolOptions()->sampleMergedBox()->setChecked(true);
    REQUIRE(v->sampleMerged());
    v->setForeground(Yellow);
    drag(v, {{0, 0}});  // the red pixel: a region of one pixel in the composite
    const auto halves = halfBlocks(doc->canvas().at(0, 0));
    REQUIRE(halves);
    CHECK(halves->top == Yellow);
    // the blue pixel was not in the region and still shows
    CHECK(halves->bottom == Blue);
    CHECK_FALSE(doc->active()->cells.at(0, 0).transparent);
    // the rest of the layer stays clear
    CHECK(doc->active()->cells.at(1, 0) == transparentCell());
}

TEST_CASE("a sample-merged fill takes nothing from the layers above the active one", "[layers]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    // bottom (0, 0): blue over blue
    Cell blue;
    blue.ch = kFullBlock;
    blue.fg = Color::indexed(Blue);
    REQUIRE(doc->edit(CellEdit{{{0, 0, Cell{}, blue}}}));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId mid = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId top = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(Red);
    drag(v, {{0, 0}});  // a red top pixel on the top layer
    REQUIRE(layerPixels(layerById(doc, top).cells.at(0, 0)) == LayerPixels{Red, std::nullopt});
    REQUIRE(doc->setActiveLayer(mid));
    // fill the blue bottom pixel, sample merged, on the empty middle layer
    use(w, "fill");
    w.toolOptions()->sampleMergedBox()->setChecked(true);
    v->setForeground(Yellow);
    const int steps = doc->undoStack()->count();
    drag(v, {{0, 1}});
    CHECK(doc->undoStack()->count() == steps + 1);
    auto h = halfBlocks(doc->canvas().at(0, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Yellow);
    // only the middle layer changed
    CHECK(layerPixels(layerById(doc, top).cells.at(0, 0)) == LayerPixels{Red, std::nullopt});
    CHECK(doc->layers()[0].cells.at(0, 0) == blue);
    // the middle layer's other half is what showed under it (blue), not the
    // top layer's red copied down
    const auto mh = halfBlocks(layerById(doc, mid).cells.at(0, 0));
    REQUIRE(mh);
    INFO("middle layer cell top " << int(mh->top) << " bottom " << int(mh->bottom));
    CHECK(mh->top == Blue);
    CHECK(mh->bottom == Yellow);
    REQUIRE(doc->setLayerVisible(top, false));
    h = halfBlocks(doc->canvas().at(0, 0));
    REQUIRE(h);
    CHECK(h->top == Blue);
    CHECK(h->bottom == Yellow);
    // undo (visibility, then the fill) restores the middle layer in one step
    doc->undoStack()->undo();
    doc->undoStack()->undo();
    CHECK(layerById(doc, mid).cells.at(0, 0) == transparentCell());
    CHECK(doc->undoStack()->count() == steps + 2);
    CHECK(doc->undoStack()->index() == steps);
}

TEST_CASE("a half pixel pasted onto a cells layer lands on top and punches no hole", "[layers][pixels][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));  // bottom: A at (0, 0)
    Cell blue;
    blue.ch = kFullBlock;
    blue.fg = Color::indexed(Blue);
    REQUIRE(doc->edit(CellEdit{{{6, 0, Cell{}, blue}}}));  // bottom: blue over blue at (6, 0)
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId cells = doc->activeLayer();
    typeAt(w, 2, 0, QLatin1Char('B'));  // cells layer: B at (2, 0)
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId pixels = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(Red);
    drag(v, {{5, 0}});  // a red top pixel at cell (5, 0)
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(pixels, false));
    REQUIRE(doc->setActiveLayer(cells));
    const auto pasteAt = [&](int x) {
        v->setCellCursor({x, 0});
        w.selectTool()->paste();
        w.selectTool()->stamp();
    };

    // over the cells layer's B: the pixel replaces it, B's background below
    pasteAt(2);
    const Cell& overB = layerById(doc, cells).cells.at(2, 0);
    INFO("ch " << int(overB.ch) << " transparent " << overB.transparent);
    CHECK_FALSE(overB.transparent);
    auto h = halfBlocks(doc->canvas().at(2, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Black);

    // over a clear cell of the cells layer: the pixel shows, over the blank
    // ground the bottom layer shows there
    pasteAt(4);
    CHECK_FALSE(layerById(doc, cells).cells.at(4, 0).transparent);
    h = halfBlocks(doc->canvas().at(4, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Black);

    // over a clear cell above the bottom's blue: the clear half keeps the
    // blue that showed through it
    pasteAt(6);
    h = halfBlocks(doc->canvas().at(6, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Blue);
    CHECK(doc->layers()[0].cells.at(6, 0) == blue);

    // onto the bottom layer's A: the bottom layer stays opaque
    REQUIRE(doc->setActiveLayer(doc->layers()[0].props.id));
    pasteAt(0);
    CHECK_FALSE(doc->layers()[0].cells.at(0, 0).transparent);
    h = halfBlocks(doc->canvas().at(0, 0));
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == Black);
}

TEST_CASE("the alpha lock keeps every edit off the layer's transparency", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));
    typeAt(w, 3, 0, QLatin1Char('Z'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId upper = doc->activeLayer();
    typeAt(w, 0, 0, QLatin1Char('B'));
    typeAt(w, 1, 0, QLatin1Char('C'));
    REQUIRE(doc->setLayerAlphaLocked(upper, true));
    const int steps = doc->undoStack()->count();

    // clearing a selection leaves the opaque B
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({0, 0, 1, 1});
    w.selectTool()->clear();
    CHECK_FALSE(doc->active()->cells.at(0, 0).transparent);
    CHECK(doc->canvas().at(0, 0).ch == U'B');
    w.selectTool()->deselect();

    // Delete and Backspace in keyboard mode leave the opaque C
    act(w, QStringLiteral("keyboardTool"))->trigger();
    v->setCellCursor({1, 0});
    key(v, Qt::Key_Delete);
    v->setCellCursor({2, 0});
    key(v, Qt::Key_Backspace);
    CHECK(doc->canvas().at(1, 0).ch == U'C');

    // typing onto a clear cell writes nothing; over an opaque one it types
    typeAt(w, 3, 0, QLatin1Char('Q'));
    CHECK(layerById(doc, upper).cells.at(3, 0) == transparentCell());
    CHECK(doc->canvas().at(3, 0).ch == U'Z');
    typeAt(w, 1, 0, QLatin1Char('D'));
    CHECK(doc->canvas().at(1, 0).ch == U'D');
    CHECK(doc->undoStack()->count() == steps + 1);

    // a paste lands on the opaque cells and not on the clear ones
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({0, 0, 2, 1});  // B D
    w.selectTool()->copy();
    w.selectTool()->deselect();
    v->setCellCursor({1, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    CHECK(doc->canvas().at(1, 0).ch == U'B');
    CHECK(layerById(doc, upper).cells.at(2, 0) == transparentCell());

    // unlocked, the same clear goes through
    REQUIRE(doc->setLayerAlphaLocked(upper, false));
    w.selectTool()->select({0, 0, 1, 1});
    w.selectTool()->clear();
    CHECK(layerById(doc, upper).cells.at(0, 0) == transparentCell());
    CHECK(doc->canvas().at(0, 0).ch == U'A');

    // on a locked bottom layer an erase leaves a blank, which stays opaque
    w.selectTool()->deselect();
    REQUIRE(doc->setActiveLayer(doc->layers()[0].props.id));
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    act(w, QStringLiteral("keyboardTool"))->trigger();
    v->setCellCursor({3, 0});
    key(v, Qt::Key_Delete);
    CHECK(doc->layers()[0].cells.at(3, 0) == Cell{});
}

TEST_CASE("with iCE off a pasted half pixel writes no bright background on a cells layer", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->setIce(false));
    const LayerId bottom = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(Yellow);
    drag(v, {{2, 1}});  // bottom: a yellow bottom pixel at cell (2, 0)
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId pixels = doc->activeLayer();
    v->setForeground(LightRed);
    drag(v, {{5, 0}});  // a light red top pixel at (5, 0)
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(pixels, false));
    REQUIRE(doc->setActiveLayer(bottom));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId cells = doc->activeLayer();
    // the half-block brush's rule: the bright red goes dark, the yellow stays
    const Cell brushed = setHalfBlock(doc->canvas().at(2, 0), false, LightRed, false);
    // (a) onto the clear cell above the yellow pixel: fitLayerEdit's half-pixel path
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    const Cell got = layerById(doc, cells).cells.at(2, 0);
    INFO("(a) ch " << int(got.ch) << " fg " << int(got.fg.paletteIndex()) << " bg " << int(got.bg.paletteIndex()));
    CHECK(got.bg.paletteIndex() < 8);
    CHECK(got == brushed);
    // what the save writes there (Document::encode applies iceCell)
    CHECK_FALSE(iceCell(doc->canvas().at(2, 0), false).attrs & attr::Blink);
    // (b) onto the bottom layer's own pixel cell: pasteCell's half-pixel path
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(cells, false));
    REQUIRE(doc->setActiveLayer(bottom));
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    const Cell got2 = doc->layers()[0].cells.at(2, 0);
    INFO("(b) ch " << int(got2.ch) << " fg " << int(got2.fg.paletteIndex()) << " bg " << int(got2.bg.paletteIndex()));
    CHECK(got2.bg.paletteIndex() < 8);
    CHECK(got2 == brushed);
}

TEST_CASE("a pasted half pixel keeps the background a blinking letter shows under iCE", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    // an iCE-off file with a blinking A on blue, opened, then iCE turned on
    REQUIRE(doc->setIce(false));
    const Cell a{U'A', Color::indexed(White), Color::indexed(Blue), attr::Blink, false};
    REQUIRE(doc->edit(CellEdit{{{2, 0, Cell{}, a}}}));
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("blink.ans"));
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        const std::string bytes = doc->encode().bytes;
        f.write(bytes.data(), qint64(bytes.size()));
    }
    QString err;
    REQUIRE(doc->load(path, &err));
    REQUIRE_FALSE(doc->ansi().ice);
    REQUIRE((doc->canvas().at(2, 0).attrs & attr::Blink));
    REQUIRE(doc->setIce(true));
    REQUIRE(iceCell(doc->canvas().at(2, 0), true).bg.paletteIndex() == LightBlue);  // shows on light blue
    const Cell loaded = doc->canvas().at(2, 0);
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId pixels = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(Red);
    drag(v, {{5, 0}});
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(pixels, false));
    REQUIRE(doc->setActiveLayer(doc->layers()[0].props.id));
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    // the half-block tools keep the background the letter shows
    REQUIRE(halfBlocks(setHalfBlock(loaded, false, Red, true), true)->bottom == LightBlue);
    const auto h = halfBlocks(iceCell(doc->canvas().at(2, 0), true), true);
    REQUIRE(h);
    CHECK(h->top == Red);
    CHECK(h->bottom == LightBlue);
}

TEST_CASE("a move the alpha lock would cut is refused whole", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    typeAt(w, 0, 0, QLatin1Char('A'));
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId upper = doc->activeLayer();
    typeAt(w, 0, 0, QLatin1Char('B'));
    typeAt(w, 1, 0, QLatin1Char('C'));  // upper: B C, clear from (2, 0)
    REQUIRE(doc->setLayerAlphaLocked(upper, true));
    const int steps = doc->undoStack()->count();
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({0, 0, 2, 1});
    w.selectTool()->moveBy(1, 0);
    w.selectTool()->stamp();
    const auto& cells = layerById(doc, upper).cells;
    std::string row;
    for (int x = 0; x < 3; ++x)
        row += cells.at(x, 0).transparent ? '.' : char(cells.at(x, 0).ch);
    INFO("upper row after the move: " << row);
    CHECK(row == "BC.");
    CHECK(doc->undoStack()->count() == steps);
    // the selection stays on the cells that didn't move
    CHECK(v->selection().bounds().x == 0);
    CHECK(v->selection().bounds().width == 2);

    // a flip in place (B C . to . C B) would clear B's cell too
    w.selectTool()->select({0, 0, 3, 1});
    w.selectTool()->transform(Transform::FlipHorizontal);
    CHECK(cells.at(0, 0).ch == U'B');
    CHECK(cells.at(2, 0).transparent);
    CHECK(doc->undoStack()->count() == steps);
    // turned, the selection would change shape: it stays as it was
    w.selectTool()->select({0, 0, 2, 1});
    w.selectTool()->transform(Transform::RotateClockwise);
    CHECK(cells.at(1, 0).ch == U'C');
    CHECK(v->selection().bounds().width == 2);
    CHECK(doc->undoStack()->count() == steps);

    // on the locked bottom layer a lift leaves blanks, which are opaque: it moves
    w.selectTool()->deselect();
    REQUIRE(doc->setActiveLayer(doc->layers()[0].props.id));
    REQUIRE(doc->setLayerAlphaLocked(doc->activeLayer(), true));
    w.selectTool()->select({0, 0, 1, 1});
    w.selectTool()->moveBy(1, 0);
    w.selectTool()->stamp();
    CHECK(doc->layers()[0].cells.at(0, 0) == Cell{});
    CHECK(doc->layers()[0].cells.at(1, 0).ch == U'A');
}

TEST_CASE("with iCE off an opaque pixel cell pasted onto a cells layer writes no bright background", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->setIce(false));
    const LayerId bottom = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId px = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(LightRed);
    drag(v, {{5, 0}});  // top pixel
    v->setForeground(Yellow);
    drag(v, {{5, 1}});  // bottom pixel: stored as ▀ fg 12 bg 14 (iCE on), composited as ▀ fg 12 bg 6
    REQUIRE(layerById(doc, px).cells.at(5, 0).bg.paletteIndex() == Yellow);
    REQUIRE(doc->canvas().at(5, 0).bg.paletteIndex() < 8);
    const Cell shown = doc->canvas().at(5, 0);
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(px, false));
    REQUIRE(doc->setActiveLayer(bottom));
    // (a) onto the bottom layer's own cell
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    const Cell got = doc->layers()[0].cells.at(2, 0);
    INFO("(a) ch " << int(got.ch) << " fg " << int(got.fg.paletteIndex()) << " bg " << int(got.bg.paletteIndex()));
    CHECK(got.bg.paletteIndex() < 8);
    // what the save writes there (Document::encode applies iceCell)
    CHECK_FALSE(iceCell(doc->canvas().at(2, 0), false).attrs & attr::Blink);
    // as the pixel layer showed it
    CHECK(got == shown);
    // (b) onto a clear cell of an upper cells layer
    w.selectTool()->deselect();
    act(w, QStringLiteral("newCellsLayer"))->trigger();
    const LayerId cells = doc->activeLayer();
    v->setCellCursor({8, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    const Cell got2 = layerById(doc, cells).cells.at(8, 0);
    INFO("(b) ch " << int(got2.ch) << " fg " << int(got2.fg.paletteIndex()) << " bg " << int(got2.bg.paletteIndex()));
    CHECK_FALSE(got2.transparent);
    CHECK(got2.bg.paletteIndex() < 8);
    CHECK_FALSE(iceCell(doc->canvas().at(8, 0), false).attrs & attr::Blink);
    CHECK(got2 == shown);
}

TEST_CASE("an opaque pixel cell pasted with iCE on or onto a pixel layer keeps both bright colours", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->ansi().ice);
    const LayerId bottom = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId px = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(LightRed);
    drag(v, {{5, 0}});
    v->setForeground(Yellow);
    drag(v, {{5, 1}});
    const Cell stored = layerById(doc, px).cells.at(5, 0);
    REQUIRE(stored.bg.paletteIndex() == Yellow);
    act(w, QStringLiteral("selectTool"))->trigger();
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    // iCE on: the cells layer takes the cell as the pixel layer stores it
    REQUIRE(doc->setActiveLayer(bottom));
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    CHECK(doc->layers()[0].cells.at(2, 0) == stored);
    // iCE off, onto the pixel layer: both pixels keep their colours
    REQUIRE(doc->setIce(false));
    w.selectTool()->deselect();
    REQUIRE(doc->setActiveLayer(px));
    v->setCellCursor({8, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    CHECK(layerPixels(layerById(doc, px).cells.at(8, 0)) == LayerPixels{LightRed, Yellow});
}

TEST_CASE("with iCE off a pixel block flipped or turned and pasted onto a cells layer writes no blink", "[layers][selection]") {
    MainWindow w;
    Document* doc = w.document();
    CanvasView* v = w.canvasView();
    REQUIRE(doc->setIce(false));
    const LayerId bottom = doc->activeLayer();
    act(w, QStringLiteral("newPixelLayer"))->trigger();
    const LayerId px = doc->activeLayer();
    use(w, "halfBlock");
    v->setForeground(LightRed);
    drag(v, {{5, 0}});
    v->setForeground(Yellow);
    drag(v, {{5, 1}});
    act(w, QStringLiteral("selectTool"))->trigger();
    const auto blinkFree = [&](int x, const char* what) {
        const Cell got = doc->layers()[0].cells.at(x, 0);
        INFO(what << ": ch " << int(got.ch) << " fg " << int(got.fg.paletteIndex()) << " bg "
                  << int(got.bg.paletteIndex()));
        CHECK(got.bg.paletteIndex() < 8);
        CHECK_FALSE(iceCell(doc->canvas().at(x, 0), false).attrs & attr::Blink);
    };
    // lifted off the pixel layer, flipped, copied from the float
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->moveBy(0, 0);
    REQUIRE(w.selectTool()->floating());
    w.selectTool()->transform(Transform::FlipVertical);
    w.selectTool()->copy();
    w.selectTool()->cancel();
    w.selectTool()->deselect();
    REQUIRE(layerPixels(layerById(doc, px).cells.at(5, 0)) == LayerPixels{LightRed, Yellow});
    REQUIRE(doc->setLayerVisible(px, false));
    REQUIRE(doc->setActiveLayer(bottom));
    v->setCellCursor({2, 0});
    w.selectTool()->paste();
    w.selectTool()->stamp();
    blinkFree(2, "flipped");
    // pasted, then turned while it floats on the cells layer
    REQUIRE(doc->setLayerVisible(px, true));
    REQUIRE(doc->setActiveLayer(px));
    w.selectTool()->select({5, 0, 1, 1});
    w.selectTool()->copy();
    w.selectTool()->deselect();
    REQUIRE(doc->setLayerVisible(px, false));
    REQUIRE(doc->setActiveLayer(bottom));
    v->setCellCursor({8, 0});
    w.selectTool()->paste();
    w.selectTool()->transform(Transform::RotateClockwise);
    w.selectTool()->stamp();
    blinkFree(8, "turned");
}

// Codex round 1: a style drawn at composite time depends on the whole layer,
// so painting, the preview, a style change, undo and redo, and an iCE
// toggle each leave canvas() what a fresh composite of the stack gives.
TEST_CASE("a styled layer's composite stays fresh through painting and preview and style changes and undo",
          "[layers][style]") {
    MainWindow w;
    Document* doc = w.document();
    const LayerId id = doc->addLayer(LayerKind::Cells);
    auto fresh = [&] {
        const Canvas want = composite(doc->layers(), CompositeOptions{doc->format().ice, Cell{}});
        int stale = 0;
        for (int y = 0; y < want.height(); ++y)
            for (int x = 0; x < want.width(); ++x)
                stale += !(doc->canvas().at(x, y) == want.at(x, y));
        return stale;
    };
    const Cell block{kFullBlock, Color::indexed(Grey), Color::indexed(Black), 0, false};
    auto put = [&](int x, int y, const Cell& c) {
        CellEdit e;
        e.changes.push_back({x, y, doc->layerCanvas().at(x, y), c});
        return e;
    };
    REQUIRE(doc->edit(put(2, 2, block)));
    CHECK(fresh() == 0);

    // the style set: the cell goes yellow, and a blue outline round it
    LayerProperties p = layerById(doc, id).props;
    p.style = TextStyle{};
    p.style->effects = {Effect{true, OutlineEffect{OutlineForm::Cells, Blue}},
                        Effect{true, FillEffect{FillMode::Flat, Yellow, "fire"}},
                        Effect{true, GlowEffect{"fire", 3, 3}}};
    REQUIRE(doc->setLayerProperties(id, p, QStringLiteral("Style")));
    CHECK(doc->canvas().at(1, 2).bg.paletteIndex() == Blue);
    CHECK(fresh() == 0);

    // painting another block outlines it too
    REQUIRE(doc->edit(put(8, 2, block)));
    CHECK(doc->canvas().at(9, 2).bg.paletteIndex() == Blue);
    CHECK(fresh() == 0);

    // the preview of an erase shows the outline gone with the block
    const CellEdit erase = put(2, 2, doc->blankCell());
    Canvas shown = doc->canvas();
    for (const CellChange& c : doc->previewEdit(erase).changes)
        shown.at(c.x, c.y) = c.after;
    LayerStack erased = doc->layers();
    erased.find(id)->cells.at(2, 2) = transparentCell();
    CHECK(shown == composite(erased, CompositeOptions{doc->format().ice, Cell{}}));
    CHECK(fresh() == 0);

    REQUIRE(doc->edit(erase));
    CHECK(doc->canvas().at(1, 2) == Cell{});
    CHECK(fresh() == 0);

    // iCE off redraws the glow without a bright background
    DocumentFormat f = doc->format();
    f.ice = false;
    REQUIRE(doc->setFormat(f, QStringLiteral("iCE")));
    CHECK(fresh() == 0);

    // every step undone and redone
    const int steps = doc->undoStack()->count();
    for (int i = 0; i < steps; ++i) {
        INFO("undo " << i);
        doc->undoStack()->undo();
        CHECK(fresh() == 0);
    }
    for (int i = 0; i < steps; ++i) {
        INFO("redo " << i);
        doc->undoStack()->redo();
        CHECK(fresh() == 0);
    }
}

// Codex round 2: a style set on a live text layer through the document is
// drawn with the document's iCE setting, and so is its redo.
TEST_CASE("live style edits use the document's iCE setting", "[layers][style][text]") {
    TextSource source;
    source.text = U"funk";
    project::Project p;
    p.ice = false;
    p.layers = LayerStack(80, 40);
    const LayerId id = p.layers.add(makeTextLayer(source));

    MainWindow w;
    Document* doc = w.document();
    doc->replace(p, QStringLiteral("style.bwp"), FileForm::ProjectZip);
    REQUIRE_FALSE(doc->format().ice);
    LayerProperties props = doc->layers().find(id)->props;
    props.style = TextStyle{{Effect{true, GlowEffect{"fire", 12, 12}}}};
    REQUIRE(doc->setLayerProperties(id, props, QStringLiteral("Style")));

    const Canvas expected = renderStyledText(source, *props.style, *findFace(source.face), false).cells;
    REQUIRE_FALSE(expected == renderStyledText(source, *props.style, *findFace(source.face), true).cells);
    CHECK(doc->layers().find(id)->cells == expected);
    doc->undoStack()->undo();
    doc->undoStack()->redo();
    CHECK(doc->layers().find(id)->cells == expected);
}

// Codex round 3: the document checks a style before it pushes the step
// (layerStyleProblem), so a style that can't be drawn on the layer is
// refused, the layer and the undo history as they were, instead of the
// step's redo throwing.
TEST_CASE("the document refuses a style it can't draw on the layer and leaves the layer and undo as they were",
          "[layers][style][text]") {
    TextSource source;
    source.text = U"x";
    source.wrapWidth = TextSource::kMaxWrapWidth;
    REQUIRE_FALSE(textSourceProblem(source));

    project::Project p;
    p.layers = LayerStack(80, 40);
    const LayerId text = p.layers.add(makeTextLayer(source));
    const LayerId cells = p.layers.add(makeLayer(LayerKind::Cells, 80, 40));
    const LayerId generator = p.layers.add(makeLayer(LayerKind::Generator, 80, 40));

    MainWindow w;
    Document* doc = w.document();
    doc->replace(p, QStringLiteral("style.bwp"), FileForm::ProjectZip);

    LayerId id = 0;
    LayerProperties props;
    SECTION("an outline grows a valid text box past a layer's size") {
        id = text;
        props = doc->layers().find(id)->props;
        props.style = TextStyle{{Effect{true, OutlineEffect{OutlineForm::Cells, Blue}}}};
        REQUIRE_FALSE(textStyleProblem(*props.style));
    }
    SECTION("a style that can't be drawn on a cells layer") {
        id = cells;
        props = doc->layers().find(id)->props;
        props.style = TextStyle{{Effect{true, OutlineEffect{OutlineForm::Cells, 16}}}};
        REQUIRE(textStyleProblem(*props.style));
    }
    SECTION("a style on a layer kind that holds none") {
        id = generator;
        props = doc->layers().find(id)->props;
        props.style = TextStyle{{Effect{true, OutlineEffect{OutlineForm::Cells, Blue}}}};
        REQUIRE_FALSE(textStyleProblem(*props.style));
    }
    REQUIRE(layerStyleProblem(props, doc->fontSources()));

    const Layer before = *doc->layers().find(id);
    const Canvas canvas = doc->canvas();
    const int steps = doc->undoStack()->count();
    const int index = doc->undoStack()->index();
    bool pushed = true;
    CHECK_NOTHROW(pushed = doc->setLayerProperties(id, props, QStringLiteral("Style")));
    CHECK_FALSE(pushed);
    CHECK(doc->layers().find(id)->props == before.props);
    CHECK(doc->layers().find(id)->cells == before.cells);
    CHECK(doc->canvas() == canvas);
    CHECK(doc->undoStack()->count() == steps);
    CHECK(doc->undoStack()->index() == index);
}

TEST_CASE("a window deleted with the Layers dock's rename editor open commits the rename while the document is "
          "alive", "[layers][teardown]") {
    auto* w = new MainWindow;
    // A floating dock is a window of its own.
    const bool floating = GENERATE(false, true);
    CAPTURE(floating);
    w->layersDock()->setFloating(floating);
    Document* doc = w->document();
    const LayerId id = doc->activeLayer();
    QLineEdit* editor = typeInRenameEditor(*w, QStringLiteral("zz"));
    FocusOutProbe probe(editor, doc);
    int renamed = 0;
    QObject::connect(doc->undoStack(), &QUndoStack::indexChanged, &probe, [&] {
        if (const Layer* l = doc->layers().find(id); l && l->props.name == "zz")
            ++renamed;
    });
    // Before #241 the editor lost the focus when ~QWidget hid the window,
    // after doc_ was destroyed, and the delegate's commit renamed a layer
    // of the dead document.
    delete w;
    CHECK(probe.late == 0);
    CHECK(probe.early == 1);
    CHECK(renamed == 1);
}

TEST_CASE("New and closing with the Layers dock's rename editor open ask about the rename", "[layers][teardown]") {
    const ForgetLayout forget;
    MainWindow w;
    Document* doc = w.document();
    const LayerId id = doc->activeLayer();
    const std::string before = doc->layers().find(id)->props.name;
    bool asked = false;
    typeInRenameEditor(w, QStringLiteral("xx"));
    REQUIRE_FALSE(doc->isModified());
    answer(&w, QMessageBox::Cancel, &asked);
    w.newFile();
    CHECK(asked);
    CHECK(doc->layers().find(id)->props.name == "xx");
    CHECK(doc->isModified());
    doc->undoStack()->undo();
    REQUIRE_FALSE(doc->isModified());
    typeInRenameEditor(w, QStringLiteral("zz"));
    REQUIRE_FALSE(doc->isModified());
    // The rename is committed before the unsaved-changes check, so the
    // check sees it: Cancel keeps the window and the renamed layer.
    answer(&w, QMessageBox::Cancel, &asked);
    CHECK_FALSE(w.close());
    CHECK(asked);
    CHECK(doc->layers().find(id)->props.name == "zz");
    CHECK(doc->isModified());
    // With Discard too, the rename lands before the check and nothing lands
    // after it.
    doc->undoStack()->undo();
    CHECK(doc->layers().find(id)->props.name == before);
    CHECK_FALSE(doc->isModified());
    typeInRenameEditor(w, QStringLiteral("yy"));
    answer(&w, QMessageBox::Discard, &asked);
    CHECK(w.close());
    CHECK(asked);
    const int steps = doc->undoStack()->count();
    QApplication::processEvents();
    CHECK(doc->undoStack()->count() == steps);
    doc->undoStack()->setClean();
}
