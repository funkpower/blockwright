// In-app help (#153): every tool in the registry has help text, and the
// toolbox tooltips, the Tool Options help panel, Help > Manual and What's
// This all read it from the one store (help_text.h); every tool but those
// that change no cells has a picture made by running it (tool_illustration.h). main() is in
// test_app.cpp.

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "face_picker.h"
#include "help_text.h"
#include "main_window.h"
#include "manual_dialog.h"
#include "scratch_settings.h"
#include "shortcuts_dialog.h"
#include "tool.h"
#include "tool_illustration.h"
#include "tool_options.h"
#include "whats_this.h"

#include "system_input.h"

#include "blockwright/glyph_sets.h"
#include "blockwright/layer.h"
#include "blockwright/palette.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QTabletEvent>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QToolButton>
#include <QUndoStack>
#include <QWhatsThis>

#include <map>

// QtGui's shortcut dispatch, as QTest's key clicks use it: the focus
// widget sees ShortcutOverride, then the window's shortcuts get the key.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods,
                                               const QString& text = QString(), bool autorep = false,
                                               ushort count = 1);

using namespace bw;

namespace {

QStringList registryIds(const MainWindow& w) {
    QStringList ids;
    for (const Tool* t : w.tools()->tools())
        ids << t->id();
    return ids;
}

QString plainName(const Tool* t) { return QString(t->name()).remove(QLatin1Char('&')); }

QString scratchDir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }

QMenu* menuHolding(MainWindow& w, QAction* action) {
    for (QAction* a : w.menuBar()->actions())
        if (QMenu* m = QMenu::menuInAction(a); m && m->actions().contains(action))
            return m;
    return nullptr;
}

QString plainOf(const QString& html) {
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText();
}

// The What's This popup Qt shows (a private class), if one is up.
bool whatsThisShown() {
    for (QWidget* t : QApplication::topLevelWidgets())
        if (t->isVisible() && QByteArray(t->metaObject()->className()) == "QWhatsThat")
            return true;
    return false;
}

// A help event as Qt's What's This mode sends it to the widget clicked or
// hovered, passed up its parents until one takes it; whether one did. It
// starts accepted, as Qt's does: a widget with nothing to say ignores it.
bool helpEvent(QWidget* w, QEvent::Type type) {
    const QPoint pos = w->rect().center();
    QHelpEvent e(type, pos, w->mapToGlobal(pos));
    QApplication::sendEvent(w, &e);
    return e.isAccepted();
}

// A key as the application delivers it: a shortcut takes it if one
// matches and the focus widget doesn't claim it, else the widget gets it.
void key(QWidget* w, int k, Qt::KeyboardModifiers mods) {
    if (qt_sendShortcutOverrideEvent(w, 0, k, mods))
        return;
    QKeyEvent press(QEvent::KeyPress, k, mods);
    QApplication::sendEvent(w, &press);
    QKeyEvent release(QEvent::KeyRelease, k, mods);
    QApplication::sendEvent(w, &release);
}

// The text of every label, check box, radio button, button and group box
// the Tool Options panel would show now, mnemonics and a closing colon
// taken off, but for the widgets named in `skip`.
QStringList toolOptionsLabels(QWidget* panel, const QSet<QString>& skip) {
    QStringList out;
    for (QWidget* child : panel->findChildren<QWidget*>()) {
        // A control shown only in a state the test doesn't put the panel in
        // (a hidden stacked page, a conditional row) is not covered.
        if (!child->isVisibleTo(panel) || skip.contains(child->objectName()))
            continue;
        QString text;
        if (auto* label = qobject_cast<QLabel*>(child))
            text = label->text();
        else if (auto* button = qobject_cast<QAbstractButton*>(child))
            text = button->text();
        else if (auto* group = qobject_cast<QGroupBox*>(child))
            text = group->title();
        else
            continue;
        text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
        text.remove(QLatin1Char('&'));
        text.replace(QLatin1Char('\x01'), QLatin1Char('&'));
        text = text.trimmed();
        if (text.endsWith(QLatin1Char(':')))
            text.chop(1);
        if (!text.isEmpty() && !out.contains(text))
            out << text;
    }
    return out;
}

} // namespace

TEST_CASE("every tool in the registry has help, and no help is left over", "[help]") {
    MainWindow w;
    const QStringList ids = registryIds(w);
    REQUIRE(!ids.isEmpty());
    for (const QString& id : ids) {
        INFO("tool " << id.toStdString());
        const QString text = help::toolText(id);
        CHECK(!text.isEmpty());
        const QString summary = help::toolSummary(id);
        CHECK(!summary.isEmpty());
        // The summary is one plain sentence or two: it goes into tooltips.
        CHECK(!summary.contains(QLatin1Char('\n')));
        CHECK(!summary.contains(QLatin1Char('*')));
        CHECK(!summary.contains(QLatin1Char('`')));
        CHECK(text.startsWith(summary));
    }
    QStringList sorted = ids;
    sorted.sort();
    CHECK(help::toolIds() == sorted);
    // and every file in the source tree is bundled: one left out of the
    // resource list in app/CMakeLists.txt is help no tool shows
    QStringList sources;
    for (const QString& f :
         QDir(QStringLiteral(BW_HELP_SOURCE "/tools")).entryList({QStringLiteral("*.md")}, QDir::Files))
        sources << f.chopped(3);
    sources.sort();
    CHECK(sources.join(QLatin1Char(' ')).toStdString() == sorted.join(QLatin1Char(' ')).toStdString());
    CHECK(help::toolText(QStringLiteral("noSuchTool")).isEmpty());
    CHECK(help::toolText(QStringLiteral("../layers/cells")).isEmpty());
}

TEST_CASE("every setting a tool shows in Tool Options is named on its help page", "[help]") {
    // Widgets in the panel whose text names no setting, by object name.
    const QSet<QString> notSettings{
        QStringLiteral("toolOptionsTitle"),  // the tool's name
        QStringLiteral("toolHelpToggle"),    // folds the help away
        QStringLiteral("toolHelp"),          // the help page itself
        QStringLiteral("toolOptionsNone"),   // "This tool has no options."
        QStringLiteral("brushGlyph"),        // a readout of the glyph brush's glyph
        QStringLiteral("transformNote"),     // a note on what the active layer lets Move/Transform do
        QStringLiteral("faceFavourite"),     // the ☆/★ mark, which the page names as ★
        QStringLiteral("fontsScanning"),     // "Scanning fonts…" while the fonts folder is read
    };
    // Settings every page that names **Symmetry** leaves to the page that
    // says what symmetry does: the label and that page.
    const std::map<QString, QString> namedOnce{
        {QStringLiteral("Folds"), QStringLiteral("halfBlock")},
        {QStringLiteral("Radial centre at"), QStringLiteral("halfBlock")},
    };
    MainWindow w;
    ToolOptions* panel = w.toolOptions();
    REQUIRE(panel);
    for (const QString& id : registryIds(w)) {
        INFO("tool " << id.toStdString());
        bool known = false;
        ToolOptions::optionsFor(id, &known);
        CHECK(known);  // a tool with no entry shows every brush setting
        w.tools()->setActive(id);
        const QString text = help::toolText(id);
        const QStringList shown = toolOptionsLabels(panel, notSettings);
        if (id != QStringLiteral("keyboard"))
            CHECK(!shown.isEmpty());
        for (const QString& name : shown) {
            INFO("setting " << name.toStdString());
            const QString bold = QStringLiteral("**%1**").arg(name);
            if (const auto once = namedOnce.find(name); once != namedOnce.end() && !text.contains(bold)) {
                CHECK(text.contains(QStringLiteral("**Symmetry**")));
                CHECK(help::toolText(once->second).contains(bold));
                continue;
            }
            CHECK(text.contains(bold));
        }
    }
}

TEST_CASE("every kind of layer has help", "[help]") {
    for (LayerKind k : {LayerKind::Cells, LayerKind::Pixels, LayerKind::Text, LayerKind::Generator,
                        LayerKind::Reference}) {
        INFO("kind " << layerKindName(k));
        CHECK(!help::layerText(k).isEmpty());
    }
}

TEST_CASE("plain text drops the markup and keeps a paragraph a line", "[help]") {
    CHECK(help::plainText(QStringLiteral("One **bold**\nline.\n\nTwo.\n")) == QStringLiteral("One bold line.\nTwo."));
}

TEST_CASE("each tool's toolbox tooltip and status tip carry its summary", "[help]") {
    MainWindow w;
    for (const Tool* t : w.tools()->tools()) {
        INFO("tool " << t->id().toStdString());
        auto* a = w.findChild<QAction*>(t->id() + QStringLiteral("Tool"));
        REQUIRE(a);
        const QString summary = help::toolSummary(t->id());
        CHECK(a->toolTip().startsWith(plainName(t)));
        CHECK(a->toolTip().contains(summary));
        CHECK(a->statusTip() == summary);
    }
}

TEST_CASE("the Tool Options help panel follows the active tool and folds away", "[help]") {
    REQUIRE(bwtest::settingsInScratch(scratchDir()));
    appSettings()->remove(QStringLiteral("toolOptions/showHelp"));
    {
        MainWindow w;
        ToolOptions* o = w.toolOptions();
        QLabel* help = o->helpLabel();
        QToolButton* toggle = o->helpToggle();
        CHECK(help->textFormat() == Qt::MarkdownText);
        // Shown by default.
        CHECK(toggle->isChecked());
        for (const Tool* t : w.tools()->tools()) {
            INFO("tool " << t->id().toStdString());
            w.tools()->setActive(t->id());
            CHECK(help->text().startsWith(help::toolText(t->id())));
            CHECK(help->text().contains(QStringLiteral("](shortcuts)")));
            CHECK(help->isVisibleTo(o));
            CHECK(toggle->isVisibleTo(o));
        }
        toggle->setChecked(false);
        CHECK(!help->isVisibleTo(o));
        CHECK(toggle->isVisibleTo(o));
        // Hidden stays hidden across a change of tool.
        w.tools()->setActive(QStringLiteral("line"));
        CHECK(!help->isVisibleTo(o));
        CHECK(help->text().startsWith(help::toolText(QStringLiteral("line"))));
        CHECK(appSettings()->value(QStringLiteral("toolOptions/showHelp")).toBool() == false);
    }
    {
        // And across a restart.
        MainWindow w;
        CHECK(!w.toolOptions()->helpToggle()->isChecked());
        CHECK(!w.toolOptions()->helpLabel()->isVisibleTo(w.toolOptions()));
        w.toolOptions()->helpToggle()->setChecked(true);
        CHECK(w.toolOptions()->helpLabel()->isVisibleTo(w.toolOptions()));
    }
    appSettings()->remove(QStringLiteral("toolOptions/showHelp"));
}

TEST_CASE("the help panel's link opens the keyboard shortcuts", "[help]") {
    MainWindow w;
    CHECK(!w.findChild<ShortcutsDialog*>());
    emit w.toolOptions()->helpLabel()->linkActivated(QStringLiteral("shortcuts"));
    auto* d = w.findChild<ShortcutsDialog*>();
    REQUIRE(d);
    CHECK(d->isVisible());
    d->close();
}

TEST_CASE("Help > Manual shows every tool's and layer's help, offline", "[help]") {
    MainWindow w;
    auto* manual = w.findChild<QAction*>(QStringLiteral("manual"));
    REQUIRE(manual);
    QMenu* helpMenu = menuHolding(w, manual);
    REQUIRE(helpMenu);
    CHECK(helpMenu->title().remove(QLatin1Char('&')) == QStringLiteral("Help"));
    CHECK(manual->shortcuts().isEmpty());

    manual->trigger();
    auto* d = w.findChild<ManualDialog*>();
    REQUIRE(d);
    CHECK(d->isVisible());
    manual->trigger();
    CHECK(w.findChildren<ManualDialog*>().size() == 1);

    const QString md = manualMarkdown(*w.tools(), &w);
    const QString shown = d->text()->toPlainText();
    for (const Tool* t : w.tools()->tools()) {
        INFO("tool " << t->id().toStdString());
        CHECK(md.contains(QStringLiteral("## ") + plainName(t)));
        CHECK(md.contains(help::toolText(t->id()).trimmed()));
        CHECK(shown.contains(help::toolSummary(t->id())));
    }
    for (LayerKind k : {LayerKind::Cells, LayerKind::Pixels, LayerKind::Text, LayerKind::Generator,
                        LayerKind::Reference}) {
        INFO("kind " << layerKindName(k));
        CHECK(md.contains(help::layerText(k).trimmed()));
    }

    // Its button opens the keyboard shortcuts.
    d->shortcutsButton()->click();
    auto* s = w.findChild<ShortcutsDialog*>();
    REQUIRE(s);
    CHECK(s->isVisible());
    s->close();
    d->close();
}

TEST_CASE("Help > What's This is Shift+F1 and starts What's This mode", "[help]") {
    MainWindow w;
    auto* whatsThis = w.findChild<QAction*>(QStringLiteral("whatsThis"));
    REQUIRE(whatsThis);
    QMenu* helpMenu = menuHolding(w, whatsThis);
    REQUIRE(helpMenu);
    CHECK(helpMenu->title().remove(QLatin1Char('&')) == QStringLiteral("Help"));
    CHECK(whatsThis->shortcuts() == QList<QKeySequence>{QKeySequence(Qt::SHIFT | Qt::Key_F1)});
    CHECK(!whatsThis->statusTip().isEmpty());

    CHECK(!QWhatsThis::inWhatsThisMode());
    whatsThis->trigger();
    CHECK(QWhatsThis::inWhatsThisMode());
    QWhatsThis::leaveWhatsThisMode();
    CHECK(!QWhatsThis::inWhatsThisMode());
}

TEST_CASE("Shift+F1 starts What's This over the canvas; Shift+F2 still types", "[help]") {
    MainWindow w;
    w.tools()->setActive(QStringLiteral("keyboard"));
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    CanvasView* v = w.canvasView();
    v->setFocus();
    REQUIRE(QApplication::activeWindow() == &w);

    key(v, Qt::Key_F1, Qt::ShiftModifier);
    CHECK(QWhatsThis::inWhatsThisMode());
    QWhatsThis::leaveWhatsThisMode();
    // It typed nothing: the shortcut took it.
    CHECK(w.document()->canvas().at(0, 0) == Cell{});
    CHECK(v->cellCursor() == Cursor{0, 0});

    key(v, Qt::Key_F2, Qt::ShiftModifier);
    CHECK(!QWhatsThis::inWhatsThisMode());
    CHECK(w.document()->canvas().at(0, 0).ch == glyphSetGlyph(v->glyphSet(), 1));
    // and F1 alone is still a glyph key
    key(v, Qt::Key_F1, Qt::NoModifier);
    CHECK(!QWhatsThis::inWhatsThisMode());
    CHECK(w.document()->canvas().at(1, 0).ch == glyphSetGlyph(v->glyphSet(), 0));
}

TEST_CASE("What's This on a tool's action or toolbox button shows all its help", "[help]") {
    MainWindow w;
    QList<QToolButton*> buttons;
    for (QToolButton* b : w.findChildren<QToolButton*>())
        if (b->objectName().startsWith(QStringLiteral("toolbox_")))
            buttons << b;
    REQUIRE(!buttons.isEmpty());
    for (const Tool* t : w.tools()->tools()) {
        INFO("tool " << t->id().toStdString());
        auto* a = w.findChild<QAction*>(t->id() + QStringLiteral("Tool"));
        REQUIRE(a);
        CHECK(a->whatsThis() == toolWhatsThis(t->id()));
        // Rich text with all of the help, not the tooltip's one line.
        CHECK(Qt::mightBeRichText(a->whatsThis()));
        const QString shown = plainOf(a->whatsThis());
        CHECK(shown.startsWith(help::toolSummary(t->id())));
        CHECK(shown.size() > help::toolSummary(t->id()).size());
        CHECK(!shown.contains(QStringLiteral("**")));
        // The button that shows the tool (a group's follows the tool picked)
        // says the same.
        w.tools()->setActive(t->id());
        QToolButton* shownBy = nullptr;
        for (QToolButton* b : buttons)
            if (b->defaultAction() == a)
                shownBy = b;
        REQUIRE(shownBy);
        CHECK(shownBy->whatsThis() == a->whatsThis());
        CHECK(helpEvent(shownBy, QEvent::QueryWhatsThis));
    }
    CHECK(toolWhatsThis(QStringLiteral("noSuchTool")).isEmpty());
}

TEST_CASE("What's This on a tool option shows its tooltip", "[help]") {
    MainWindow w;
    w.show();
    QApplication::processEvents();
    ToolOptions* o = w.toolOptions();

    // A spin box's line edit answers by the spin box, and a form row's
    // label by its field.
    w.tools()->setActive(QStringLiteral("shade"));
    auto* size = o->findChild<QSpinBox*>(QStringLiteral("brushSize"));
    REQUIRE(size);
    REQUIRE(!size->toolTip().isEmpty());
    QLineEdit* edit = size->findChild<QLineEdit*>();
    REQUIRE(edit);
    CHECK(toolTipWhatsThis(o, edit) == size->toolTip());
    QLabel* sizeLabel = nullptr;
    for (QLabel* l : o->findChildren<QLabel*>())
        if (l->buddy() == size)
            sizeLabel = l;
    REQUIRE(sizeLabel);
    CHECK(toolTipWhatsThis(o, sizeLabel) == size->toolTip());
    // A tooltip set later is the one shown.
    const QString tip = size->toolTip();
    size->setToolTip(QStringLiteral("changed"));
    CHECK(toolTipWhatsThis(o, edit) == QStringLiteral("changed"));
    size->setToolTip(tip);
    // The help panel itself has nothing to add.
    CHECK(toolTipWhatsThis(o, o->helpLabel()).isEmpty());

    // As Qt sends it: the query is taken (the cursor says there is help) and
    // the click shows it.
    REQUIRE(edit->isVisible());
    CHECK(helpEvent(edit, QEvent::QueryWhatsThis));
    CHECK(helpEvent(sizeLabel, QEvent::QueryWhatsThis));
    CHECK(!helpEvent(o->helpLabel(), QEvent::QueryWhatsThis));
    CHECK(!whatsThisShown());
    CHECK(helpEvent(edit, QEvent::WhatsThis));
    CHECK(whatsThisShown());
    QWhatsThis::hideText();
    QApplication::processEvents();

    // Every setting every tool shows says what it does.
    for (const Tool* t : w.tools()->tools()) {
        w.tools()->setActive(t->id());
        QApplication::processEvents();
        // A form row's label and its field, a composite one like the face
        // picker included.
        QSet<const QWidget*> rows;
        for (const QLabel* l : o->findChildren<QLabel*>())
            if (l->buddy())
                rows << l << l->buddy();
        for (QWidget* c : o->findChildren<QWidget*>()) {
            const bool setting = qobject_cast<QAbstractSpinBox*>(c) || qobject_cast<QComboBox*>(c) ||
                                 (qobject_cast<QAbstractButton*>(c) && c != o->helpToggle()) ||
                                 (qobject_cast<QLineEdit*>(c) && !qobject_cast<QAbstractSpinBox*>(c->parentWidget()) &&
                                  !qobject_cast<QComboBox*>(c->parentWidget())) ||
                                 rows.contains(c);
            if (!setting || !c->isVisibleTo(o))
                continue;
            INFO("tool " << t->id().toStdString() << ", " << c->metaObject()->className() << " "
                         << c->objectName().toStdString());
            CHECK(!toolTipWhatsThis(o, c).isEmpty());
        }
    }
}

TEST_CASE("What's This on the Text tool's face picker shows what it picks", "[help]") {
    MainWindow w;
    w.show();
    QApplication::processEvents();
    ToolOptions* o = w.toolOptions();
    w.tools()->setActive(QStringLiteral("text"));
    QApplication::processEvents();
    auto* picker = o->findChild<FacePicker*>(QStringLiteral("textFace"));
    REQUIRE(picker);
    QLabel* face = nullptr;
    for (QLabel* l : o->findChildren<QLabel*>())
        if (l->buddy() == picker)
            face = l;
    REQUIRE(face);
    CHECK(face->text() == QStringLiteral("Face"));
    CHECK(!toolTipWhatsThis(o, picker).isEmpty());
    CHECK(toolTipWhatsThis(o, face) == toolTipWhatsThis(o, picker));
    CHECK(helpEvent(face, QEvent::QueryWhatsThis));
}

namespace {

// The view pixel in the middle of cell (x, y).
QPointF cellAt(CanvasView* v, int x, int y) { return v->cellRect(QPoint(x, y)).center(); }

void mouseAt(CanvasView* v, QEvent::Type type, int x, int y, Qt::MouseButtons held) {
    bwtest::systemMouse(v, type, cellAt(v, x, y), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, held);
}

} // namespace

TEST_CASE("Shift+F1 during a drag is the drag's: the drag isn't left stuck", "[help]") {
    // Qt's What's This mode takes every mouse event, a drag's release too.
    // During a drag the view keeps Shift+F1, as it keeps the undo key: it is
    // F1 with Shift, and does to the drag what Shift+F2 to F10 do.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setFocus();
    REQUIRE(QApplication::activeWindow() == &w);
    QUndoStack* undo = w.document()->undoStack();

    SECTION("a line drag ends with its release, and a later click lands no line from its press") {
        w.tools()->setActive(QStringLiteral("line"));
        Tool* line = w.tools()->active();
        mouseAt(v, QEvent::MouseButtonPress, 0, 0, Qt::LeftButton);
        mouseAt(v, QEvent::MouseMove, 4, 0, Qt::LeftButton);
        REQUIRE(line->dragging());
        bwtest::systemKey(v, Qt::Key_F1, Qt::ShiftModifier);
        CHECK(!QWhatsThis::inWhatsThisMode());
        mouseAt(v, QEvent::MouseButtonRelease, 4, 0, Qt::NoButton);
        CHECK(!line->dragging());
        // The review's repro, steps 4 and 5: a click, a hover, a click.
        mouseAt(v, QEvent::MouseButtonPress, 8, 2, Qt::LeftButton);
        mouseAt(v, QEvent::MouseButtonRelease, 8, 2, Qt::NoButton);
        mouseAt(v, QEvent::MouseMove, 10, 3, Qt::NoButton);
        CHECK(!line->dragging());
        mouseAt(v, QEvent::MouseButtonPress, 20, 6, Qt::LeftButton);
        mouseAt(v, QEvent::MouseButtonRelease, 20, 6, Qt::NoButton);
        CHECK(!line->dragging());
        CHECK(w.document()->canvas().at(10, 3) == Cell{});
    }
    SECTION("the glyph brush re-picks F1's glyph mid-stroke, as F2 to F10 do") {
        w.tools()->setActive(QStringLiteral("glyph"));
        auto* brush = qobject_cast<GlyphTool*>(w.tools()->active());
        REQUIRE(brush);
        const char32_t f1 = glyphSetGlyph(v->glyphSet(), 0);
        REQUIRE(brush->glyph() != f1);
        mouseAt(v, QEvent::MouseButtonPress, 2, 2, Qt::LeftButton);
        mouseAt(v, QEvent::MouseMove, 3, 2, Qt::LeftButton);
        REQUIRE(brush->dragging());
        bwtest::systemKey(v, Qt::Key_F1, Qt::ShiftModifier);
        CHECK(!QWhatsThis::inWhatsThisMode());
        CHECK(brush->glyph() == f1);
        CHECK(brush->dragging());
        mouseAt(v, QEvent::MouseButtonRelease, 3, 2, Qt::NoButton);
        CHECK(!brush->dragging());
        CHECK(undo->count() == 1);
        CHECK(w.document()->canvas().at(3, 2).ch == f1);
    }
    SECTION("with no drag, Shift+F1 still starts What's This") {
        w.tools()->setActive(QStringLiteral("line"));
        bwtest::systemKey(v, Qt::Key_F1, Qt::ShiftModifier);
        CHECK(QWhatsThis::inWhatsThisMode());
    }
    QWhatsThis::leaveWhatsThisMode();
}

TEST_CASE("A stylus tap in What's This mode paints nothing and leaves the mode", "[help]") {
    // Qt's What's This mode takes mouse clicks only: the canvas leaves the
    // pen's tablet events unaccepted, so a mouse click is made of them (by
    // Qt, or by the platform), and the mode takes that.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    w.tools()->setActive(QStringLiteral("halfBlock"));
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    const QPointF at = cellAt(v, 2, 2);
    const auto tablet = [&](QEvent::Type type, Qt::MouseButtons held) {
        QTabletEvent e(type, bwtest::systemStylus(), at, v->mapToGlobal(at), held ? 1.0 : 0.0, 0, 0, 0, 0, 0,
                       Qt::NoModifier, Qt::LeftButton, held);
        e.setAccepted(false);
        QApplication::sendEvent(v, &e);
        return e.isAccepted();
    };

    QWhatsThis::enterWhatsThisMode();
    REQUIRE(QWhatsThis::inWhatsThisMode());
    CHECK(!tablet(QEvent::TabletPress, Qt::LeftButton));
    bwtest::systemMouse(v, QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, bwtest::systemStylus());
    CHECK(!tablet(QEvent::TabletRelease, Qt::NoButton));
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, bwtest::systemStylus());
    CHECK(undo->count() == 0);
    CHECK(!QWhatsThis::inWhatsThisMode());
    CHECK(!w.tools()->active()->dragging());
    QWhatsThis::leaveWhatsThisMode();

    // Out of the mode, the pen paints again.
    CHECK(tablet(QEvent::TabletPress, Qt::LeftButton));
    CHECK(tablet(QEvent::TabletRelease, Qt::NoButton));
    CHECK(undo->count() == 1);
}

TEST_CASE("A stylus stroke under way when What's This starts goes on and ends on its release", "[help]") {
    const bwtest::KeyboardMenus keyboardMenus;
    // A stroke kept through a menu opened from the keyboard leaves the focus
    // on the menu bar, so Shift+F1 there is the action's, and the mode starts
    // mid-stroke. The pen already drawing keeps its tablet events: without
    // them the stroke never gets its release and is left stuck.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    w.tools()->setActive(QStringLiteral("glyph"));
    CanvasView* v = w.canvasView();
    v->setFocus();
    QMenu* file = w.menuBar()->actions().at(0)->menu();
    REQUIRE(file);

    bwtest::systemTablet(v, cellAt(v, 2, 2), Qt::LeftButton, 1.0);
    REQUIRE(w.tools()->active()->dragging());
    bwtest::systemKey(v, Qt::Key_F, Qt::AltModifier, QStringLiteral("f"));
    bwtest::systemKey(file, Qt::Key_Escape);
    REQUIRE(!v->hasFocus());
    bwtest::systemKey(w.menuBar(), Qt::Key_F1, Qt::ShiftModifier);
    REQUIRE(QWhatsThis::inWhatsThisMode());
    bwtest::systemTablet(v, cellAt(v, 5, 2), Qt::LeftButton, 1.0);
    bwtest::systemTablet(v, cellAt(v, 5, 2), Qt::NoButton, 0.0);
    CHECK(!w.tools()->active()->dragging());
    CHECK(w.document()->undoStack()->count() == 1);
    CHECK(!(w.document()->canvas().at(5, 2) == Cell{}));
    QWhatsThis::leaveWhatsThisMode();
}


namespace {

// The glyphs of row `y`.
QString rowText(const Canvas& c, int y) {
    QString row;
    for (int x = 0; x < c.width(); ++x) {
        const char32_t ch = c.at(x, y).ch.codePoint().value();
        row += QString::fromUcs4(&ch, 1);
    }
    return row;
}

bool shows(const Cell& c, std::uint8_t colour) {
    return (c.ch != U' ' && c.fg == Color::indexed(colour)) || (c.ch != U'█' && c.bg == Color::indexed(colour));
}

QByteArray settingsBytes() {
    appSettings()->sync();
    QFile f(bwtest::scratchSettingsFile());
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

TEST_CASE("every tool in the registry has a picture or a reason it has none", "[help][illustration]") {
    MainWindow w;
    const QStringList ids = registryIds(w);
    QStringList none;
    for (const auto& [id, why] : help::toolsWithoutIllustration()) {
        INFO("tool " << id.toStdString());
        CHECK(!why.isEmpty());
        none << id;
    }
    for (const QString& id : ids) {
        INFO("tool " << id.toStdString());
        const help::Illustration* picture = help::toolIllustration(id);
        CHECK(bool(picture) != none.contains(id));
        if (!picture)
            continue;
        CHECK(help::scriptToolId(id) == id);
        CHECK(picture->cells.width() == help::kIllustrationSize.width());
        CHECK(picture->cells.height() == help::kIllustrationSize.height());
        CHECK(picture->image.size() == QSize(help::kIllustrationSize.width() * 8, help::kIllustrationSize.height() * 16));
        CHECK(picture->image.pixelColor(0, 0).isValid());
        // Not the document it started on: the tool did something.
        REQUIRE(picture->before.width() == picture->cells.width());
        REQUIRE(picture->before.height() == picture->cells.height());
        bool changed = false;
        for (int y = 0; y < picture->cells.height(); ++y)
            for (int x = 0; x < picture->cells.width(); ++x)
                changed = changed || !(picture->cells.at(x, y) == picture->before.at(x, y));
        CHECK(changed);
    }
    // No script or reason for a tool that isn't there.
    for (const QString& id : help::illustratedTools())
        CHECK(ids.contains(id));
    for (const QString& id : none)
        CHECK(ids.contains(id));
    CHECK(help::toolIllustration(QStringLiteral("eyedropper")) == nullptr);
    CHECK(help::toolIllustration(QStringLiteral("noSuchTool")) == nullptr);
    // Made once and kept.
    CHECK(help::toolIllustration(QStringLiteral("line")) == help::toolIllustration(QStringLiteral("line")));
}

TEST_CASE("making the pictures leaves the open document, its undo and the settings alone", "[help][illustration]") {
    REQUIRE(bwtest::settingsInScratch(scratchDir()));
    // Two of the scripts set options their tools keep in the settings (the
    // spray's density, the texture stamp's kind): start from the defaults,
    // so a run that wrote them would show.
    for (const char* group : {"spray", "texture"})
        appSettings()->remove(QString::fromLatin1(group));
    MainWindow w;
    Document* doc = w.document();
    CellEdit e;
    e.changes.push_back({2, 1, doc->layerCanvas().at(2, 1), Cell{U'X', Color::indexed(Yellow), Color::indexed(Blue)}});
    REQUIRE(doc->edit(e));
    e.changes = {{3, 1, doc->layerCanvas().at(3, 1), Cell{U'Y', Color::indexed(Red), Color::indexed(Black)}}};
    REQUIRE(doc->edit(e));
    doc->undoStack()->undo();
    w.tools()->setActive(QStringLiteral("clone"));
    const Pen pen{Color::indexed(Brown), Color::indexed(Green)};
    w.canvasView()->setPen(pen);
    const BrushSettings brush = w.canvasView()->brushSettings();
    const Canvas canvas = doc->canvas();
    const int count = doc->undoStack()->count(), index = doc->undoStack()->index();
    const QString settingsFile = appSettings()->fileName();
    const QByteArray settings = settingsBytes();

    for (const QString& id : help::illustratedTools()) {
        INFO("tool " << id.toStdString());
        CHECK(help::makeToolIllustration(id).has_value());
    }

    CHECK(doc->canvas() == canvas);
    CHECK(doc->undoStack()->count() == count);
    CHECK(doc->undoStack()->index() == index);
    CHECK(doc->undoStack()->isClean() == false);
    CHECK(w.tools()->active()->id() == QStringLiteral("clone"));
    CHECK(!static_cast<CloneTool*>(w.tools()->active())->mark());
    CHECK(w.canvasView()->pen().fg == pen.fg);
    CHECK(w.canvasView()->pen().bg == pen.bg);
    CHECK(w.canvasView()->brushSettings() == brush);
    CHECK(appSettings()->fileName() == settingsFile);
    CHECK(settingsBytes() == settings);
}

TEST_CASE("the pictures don't take the pen or glyph set kept from the last run", "[help][illustration]") {
    REQUIRE(bwtest::settingsInScratch(scratchDir()));
    std::map<QString, Canvas> fresh;
    for (const QString& id : help::illustratedTools())
        if (const auto p = help::makeToolIllustration(id))
            fresh.emplace(id, p->cells);
    appSettings()->setValue(QStringLiteral("pen/foreground"), int(Red));
    appSettings()->setValue(QStringLiteral("pen/background"), int(LightBlue));
    appSettings()->setValue(QStringLiteral("glyphSet/index"), 0);
    for (const QString& id : help::illustratedTools()) {
        INFO("tool " << id.toStdString());
        const auto p = help::makeToolIllustration(id);
        REQUIRE(p);
        CHECK(p->cells == fresh.at(id));
    }
}

TEST_CASE("the pictures show what their tools do", "[help][illustration]") {
    auto cells = [](const char* id) {
        const help::Illustration* p = help::toolIllustration(QString::fromLatin1(id));
        REQUIRE(p);
        return p->cells;
    };

    SECTION("the line climbs from the bottom left to the top right") {
        const Canvas c = cells("line");
        int last = c.width(), top = -1;
        for (int y = 0; y < c.height(); ++y) {
            INFO("row " << y);
            int left = -1;
            for (int x = 0; x < c.width() && left < 0; ++x)
                if (shows(c.at(x, y), LightCyan))
                    left = x;
            REQUIRE(left >= 0);
            if (y == 0)
                top = left;
            CHECK(left <= last);
            last = left;
        }
        CHECK(top - last >= 10);
    }
    SECTION("the fill floods the inside of the outline and nothing outside it") {
        const Canvas c = cells("fill");
        CHECK(shows(c.at(7, 3), Cyan));
        CHECK(shows(c.at(4, 2), Cyan));
        CHECK(c.at(0, 0) == Cell{});
        CHECK(c.at(15, 5) == Cell{});
    }
    SECTION("the shade brush builds up ░▒▓█") {
        const QString row = rowText(cells("shade"), 3);
        const qsizetype a = row.indexOf(u'░'), b = row.indexOf(u'▒'), c = row.indexOf(u'▓'), d = row.indexOf(u'█');
        CHECK(a >= 0);
        CHECK(a < b);
        CHECK(b < c);
        CHECK(c < d);
    }
    SECTION("the clone stamp copies the art") {
        const Canvas c = cells("clone");
        for (int y = 1; y <= 3; ++y)
            for (int x = 1; x <= 5; ++x) {
                INFO("cell " << x << "," << y);
                CHECK(c.at(x + 8, y) == c.at(x, y));
            }
        CHECK(shows(c.at(3, 2), Yellow));
    }
    SECTION("the selection lifts the art and sets it down further on") {
        const Canvas c = cells("select");
        bool artLeft = false;
        for (int y = 1; y <= 3; ++y)
            for (int x = 1; x <= 5; ++x)
                artLeft = artLeft || shows(c.at(x, y), Yellow);
        CHECK(!artLeft);
        CHECK(shows(c.at(11, 3), Yellow));
    }
    SECTION("the keyboard types") {
        CHECK(rowText(cells("keyboard"), 2).contains(QStringLiteral("hello, ansi")));
    }
    SECTION("the colorize brush recolours the right and keeps the glyphs") {
        const Canvas c = cells("colorize");
        CHECK(c.at(1, 0).fg == Color::indexed(Grey));
        CHECK(c.at(14, 0).fg == Color::indexed(LightMagenta));
        CHECK(rowText(c, 0) == QStringLiteral("░▒▓█ colour ▓▒░░"));
    }
}

TEST_CASE("the Tool Options help panel shows the active tool's picture", "[help][illustration]") {
    REQUIRE(bwtest::settingsInScratch(scratchDir()));
    appSettings()->remove(QStringLiteral("toolOptions/showHelp"));
    MainWindow w;
    ToolOptions* o = w.toolOptions();
    QLabel* picture = o->illustrationLabel();
    REQUIRE(picture);
    CHECK(picture->objectName() == QStringLiteral("toolIllustration"));
    for (const Tool* t : w.tools()->tools()) {
        INFO("tool " << t->id().toStdString());
        w.tools()->setActive(t->id());
        const help::Illustration* p = help::toolIllustration(t->id());
        CHECK(picture->pixmap().isNull() == !p);
        CHECK(picture->isVisibleTo(o) == bool(p));
        if (p)
            CHECK(picture->pixmap().toImage().convertToFormat(p->image.format()) == p->image);
    }
    w.tools()->setActive(QStringLiteral("line"));
    o->helpToggle()->setChecked(false);
    CHECK(!picture->isVisibleTo(o));
    w.tools()->setActive(QStringLiteral("fill"));
    CHECK(!picture->isVisibleTo(o));
    o->helpToggle()->setChecked(true);
    CHECK(picture->isVisibleTo(o));
    w.tools()->setActive(QStringLiteral("eyedropper"));
    CHECK(!picture->isVisibleTo(o));
    appSettings()->remove(QStringLiteral("toolOptions/showHelp"));
}

TEST_CASE("Help > Manual shows each tool's picture under its heading", "[help][illustration]") {
    MainWindow w;
    const QString md = manualMarkdown(*w.tools(), &w);
    for (const Tool* t : w.tools()->tools()) {
        INFO("tool " << t->id().toStdString());
        const QString image = QStringLiteral("](%1)").arg(illustrationUrl(t->id()).toString());
        CHECK(md.contains(image) == bool(help::toolIllustration(t->id())));
    }
    const qsizetype heading = md.indexOf(QStringLiteral("## Line")), image = md.indexOf(QStringLiteral("](illustration:line)"));
    CHECK(heading >= 0);
    CHECK(heading < image);
    CHECK(md.indexOf(QStringLiteral("\n## "), heading + 1) > image);

    ManualDialog* d = showManual(*w.tools(), &w);
    QTextDocument* doc = d->text()->document();
    QStringList shown;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (auto f = b.begin(); !f.atEnd(); ++f)
            if (f.fragment().charFormat().isImageFormat())
                shown << f.fragment().charFormat().toImageFormat().name();
    CHECK(shown.contains(illustrationUrl(QStringLiteral("line")).toString()));
    CHECK(!shown.contains(illustrationUrl(QStringLiteral("eyedropper")).toString()));
    const QImage line = doc->resource(QTextDocument::ImageResource, illustrationUrl(QStringLiteral("line"))).value<QImage>();
    CHECK(line == help::toolIllustration(QStringLiteral("line"))->image);
    d->close();
}
