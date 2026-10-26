// The workspace (PLAN §11): the Toolbox, Tool Options and glyph bar docks,
// the pen's swap and reset, the Window menu and the saved layout.

#include "blockwright/glyph_sets.h"

#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "glyph_bar.h"
#include "lint_dock.h"
#include "main_window.h"
#include "palette_widget.h"
#include "scratch_settings.h"
#include "select_tool.h"
#include "tool.h"
#include "tool_options.h"
#include "toolbox.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QHelpEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

QAction* toolAction(MainWindow& w, const QString& id) { return act(w, id + QStringLiteral("Tool")); }

QString keyText(const QKeySequence& k) { return k.toString(QKeySequence::NativeText); }

void press(QWidget* w, QPoint at, Qt::MouseButton button = Qt::LeftButton) {
    QMouseEvent e(QEvent::MouseButtonPress, QPointF(at), w->mapToGlobal(QPointF(at)), button, button, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

// Ask the current style for the split button's arrow or icon hit area.
QRect toolboxButtonRect(QToolButton* button, QStyle::SubControl control) {
    QStyleOptionToolButton option;
    option.initFrom(button);
    option.features = QStyleOptionToolButton::HasMenu | QStyleOptionToolButton::MenuButtonPopup;
    option.iconSize = button->iconSize();
    return button->style()->subControlRect(QStyle::CC_ToolButton, &option, control, button);
}

// The Window menu, by its title.
QMenu* windowMenu(MainWindow& w) {
    for (QAction* a : w.menuBar()->actions())
        if (a->menu() && a->text().remove(QLatin1Char('&')) == QStringLiteral("Window"))
            return a->menu();
    return nullptr;
}

// The scratch directory the run's settings file is in.
QString scratchDir() { return QFileInfo(bwtest::scratchSettingsFile()).absolutePath(); }

// Forgets the layout the windows here saved, from the scratch settings only.
struct ClearLayout {
    ClearLayout() { REQUIRE(bwtest::settingsInScratch(scratchDir())); }
    ~ClearLayout() {
        if (bwtest::settingsInScratch(scratchDir()))
            bw::appSettings()->clear();
    }
};

} // namespace

TEST_CASE("the toolbox has a button for every tool, with the Tools menu's actions and keys") {
    MainWindow w;
    Toolbox* box = w.toolbox();
    REQUIRE(box);
    CHECK(w.toolboxDock()->widget() == box);
    CHECK(w.toolboxDock()->objectName() == QStringLiteral("toolboxDock"));
    CHECK(w.dockWidgetArea(w.toolboxDock()) == Qt::LeftDockWidgetArea);
    CHECK(box->buttons().size() == Toolbox::groups().size());
    for (Tool* t : w.tools()->tools()) {
        INFO(t->id().toStdString());
        QAction* a = toolAction(w, t->id());
        QToolButton* b = box->buttonFor(t->id());
        REQUIRE(b);
        CHECK((b->defaultAction() == a || b->menu()->actions().contains(a)));
        CHECK_FALSE(a->icon().isNull());
        // the tooltip names the tool and its shortcut
        CHECK(a->toolTip().startsWith(QString(t->name()).remove(QLatin1Char('&'))));
        REQUIRE_FALSE(a->shortcut().isEmpty());
        CHECK(a->toolTip().contains(keyText(a->shortcut())));
        CHECK(b->focusPolicy() == Qt::NoFocus);
    }
    // a single tool's button has no menu; a group's has its tools, in order
    CHECK(box->buttonFor(QStringLiteral("keyboard"))->menu() == nullptr);
    QToolButton* shapes = box->buttonFor(QStringLiteral("line"));
    CHECK(shapes == box->buttonFor(QStringLiteral("rectangle")));
    CHECK(shapes == box->buttonFor(QStringLiteral("ellipse")));
    REQUIRE(shapes->menu());
    CHECK(shapes->menu()->actions() == QList<QAction*>{toolAction(w, QStringLiteral("line")),
                                                       toolAction(w, QStringLiteral("rectangle")),
                                                       toolAction(w, QStringLiteral("ellipse"))});
    CHECK(shapes->popupMode() == QToolButton::MenuButtonPopup);
    CHECK(toolAction(w, QStringLiteral("ellipse"))->toolTip().contains(QStringLiteral("right-click")));
    CHECK_FALSE(toolAction(w, QStringLiteral("eyedropper"))->toolTip().contains(QStringLiteral("right-click")));
}

TEST_CASE("a toolbox button picks its tool, and a group's button shows the last one picked") {
    MainWindow w;
    Toolbox* box = w.toolbox();
    QToolButton* brushes = box->buttonFor(QStringLiteral("rampShade"));
    CHECK(brushes->defaultAction() == toolAction(w, QStringLiteral("rampShade")));

    box->buttonFor(QStringLiteral("eyedropper"))->click();
    CHECK(w.tools()->active()->id() == QStringLiteral("eyedropper"));
    CHECK(box->buttonFor(QStringLiteral("eyedropper"))->isChecked());

    // picked from the menu (or a shortcut): the group's button takes it
    toolAction(w, QStringLiteral("soft"))->trigger();
    CHECK(brushes->defaultAction() == toolAction(w, QStringLiteral("soft")));
    CHECK(brushes->isChecked());
    CHECK(brushes->menu());
    CHECK_FALSE(box->buttonFor(QStringLiteral("eyedropper"))->isChecked());
    // another tool, then a click on the group's button: the last one again
    box->buttonFor(QStringLiteral("fill"))->click();
    CHECK_FALSE(brushes->isChecked());
    brushes->click();
    CHECK(w.tools()->active()->id() == QStringLiteral("soft"));
}

TEST_CASE("clicking each toolbox group's arrow opens its flyout") {
    const auto id = QString::fromLatin1(GENERATE("halfBlock", "rampShade", "outline", "line", "fill"));
    INFO(id.toStdString());
    MainWindow w;
    w.show();
    QApplication::processEvents();
    auto* button = w.toolbox()->buttonFor(id);
    REQUIRE(button);
    auto* menu = button->menu();
    REQUIRE(menu);
    const QRect arrow = toolboxButtonRect(button, QStyle::SC_ToolButtonMenu);
    REQUIRE_FALSE(arrow.isEmpty());
    REQUIRE(button->rect().contains(arrow));
    const QString activeBefore = w.tools()->active()->id();
    bool shown = false;
    // QToolButton opens QMenu in a nested event loop. This timer closes it
    // whether the click blocks inside sendEvent or returns asynchronously.
    QTimer closeMenu;
    closeMenu.setInterval(0);
    QObject::connect(&closeMenu, &QTimer::timeout, menu, [&] {
        if (menu->isVisible()) {
            shown = true;
            menu->close();
        }
    });
    closeMenu.start();
    press(button, arrow.center());
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(arrow.center()),
                        button->mapToGlobal(QPointF(arrow.center())), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(button, &release);
    QApplication::processEvents();
    closeMenu.stop();
    CHECK(shown);
    CHECK_FALSE(menu->isVisible());
    CHECK(w.tools()->active()->id() == activeBefore);
}

TEST_CASE("clicking each toolbox group's icon picks its current tool without opening the flyout") {
    const auto id = QString::fromLatin1(GENERATE("halfBlock", "rampShade", "outline", "line", "fill"));
    INFO(id.toStdString());
    MainWindow w;
    w.show();
    QApplication::processEvents();
    auto* button = w.toolbox()->buttonFor(id);
    REQUIRE(button);
    auto* menu = button->menu();
    REQUIRE(menu);
    // Pick another member first, then leave the group. The icon must select
    // that remembered member, rather than reverting to the group's first.
    QAction* current = menu->actions().last();
    current->trigger();
    REQUIRE(button->defaultAction() == current);
    toolAction(w, QStringLiteral("keyboard"))->trigger();
    const QRect icon = toolboxButtonRect(button, QStyle::SC_ToolButton);
    REQUIRE_FALSE(icon.isEmpty());
    REQUIRE_FALSE(toolboxButtonRect(button, QStyle::SC_ToolButtonMenu).contains(icon.center()));
    bool opened = false;
    QObject::connect(menu, &QMenu::aboutToShow, menu, [&] { opened = true; });
    QTimer closeMenu;
    QObject::connect(&closeMenu, &QTimer::timeout, menu, &QMenu::close);
    closeMenu.start(0); // also bounds an accidental popup's nested event loop
    press(button, icon.center());
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(icon.center()),
                        button->mapToGlobal(QPointF(icon.center())), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(button, &release);
    QApplication::processEvents();
    closeMenu.stop();
    CHECK_FALSE(opened);
    CHECK_FALSE(menu->isVisible());
    CHECK(button->defaultAction() == current);
    CHECK(toolAction(w, w.tools()->active()->id()) == current);
    CHECK(button->isChecked());
}

TEST_CASE("the toolbox's two columns fit the default dock and a 240 pixel column") {
    MainWindow w;
    w.show();
    QApplication::processEvents();
    const auto checkFits = [](Toolbox* box) {
        const auto buttons = box->buttons();
        for (int i = 0; i < buttons.size(); ++i) {
            INFO(buttons[i]->objectName().toStdString());
            CHECK(box->rect().contains(buttons[i]->geometry()));
            CHECK(buttons[i]->width() >= buttons[i]->sizeHint().width());
            if (i % 2 == 1) {
                CHECK(buttons[i]->y() == buttons[i - 1]->y());
                CHECK(buttons[i]->x() > buttons[i - 1]->geometry().right());
            } else if (i >= 2) {
                CHECK(buttons[i]->x() == buttons[i - 2]->x());
                CHECK(buttons[i]->y() > buttons[i - 2]->geometry().bottom());
            }
        }
    };
    checkFits(w.toolbox());
    QHash<QString, QAction*> actions;
    for (Tool* tool : w.tools()->tools())
        actions.insert(tool->id(), toolAction(w, tool->id()));
    Toolbox narrow(w.tools(), actions);
    CHECK(narrow.minimumSizeHint().width() <= 240);
    CHECK(narrow.sizeHint().width() <= 240);
    narrow.resize(240, narrow.sizeHint().height());
    narrow.show();
    QApplication::processEvents();
    REQUIRE(narrow.width() == 240);
    checkFits(&narrow);
}

TEST_CASE("a right click on a group's toolbox button opens the group") {
    MainWindow w;
    w.show();
    QToolButton* shapes = w.toolbox()->buttonFor(QStringLiteral("line"));
    QMenu* opened = nullptr;
    QTimer::singleShot(0, [&opened] {
        opened = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (opened)
            opened->close();
    });
    emit shapes->customContextMenuRequested(shapes->rect().center());
    CHECK(opened == shapes->menu());
    CHECK(shapes->contextMenuPolicy() == Qt::CustomContextMenu);
}

TEST_CASE("every tool has a Tool Options entry") {
    MainWindow w;
    for (Tool* t : w.tools()->tools()) {
        INFO(t->id().toStdString());
        bool known = false;
        ToolOptions::optionsFor(t->id(), &known);
        CHECK(known);
    }
    bool known = true;
    CHECK(ToolOptions::optionsFor(QStringLiteral("noSuchTool"), &known).testFlag(ToolOptions::Option::Size));
    CHECK_FALSE(known);
}

TEST_CASE("Tool Options shows only the active tool's settings") {
    MainWindow w;
    ToolOptions* o = w.toolOptions();
    REQUIRE(o);
    CHECK(w.toolOptionsDock()->objectName() == QStringLiteral("toolOptionsDock"));
    CHECK(w.dockWidgetArea(w.toolOptionsDock()) == Qt::LeftDockWidgetArea);
    auto* title = o->findChild<QLabel*>(QStringLiteral("toolOptionsTitle"));
    auto* none = o->findChild<QLabel*>(QStringLiteral("toolOptionsNone"));
    REQUIRE(title);
    REQUIRE(none);
    const auto shows = [o](QWidget* control) { return control->isVisibleTo(o); };
    for (Tool* t : w.tools()->tools()) {
        INFO(t->id().toStdString());
        w.tools()->setActive(t);
        const ToolOptions::Options want = ToolOptions::optionsFor(t->id());
        CHECK(o->shown() == want);
        CHECK(title->text() == QString(t->name()).remove(QLatin1Char('&')));
        CHECK(shows(none) == !want);
        CHECK(shows(o->sizeBox()) == want.testFlag(ToolOptions::Option::Size));
        CHECK(shows(o->tipBox()) == want.testFlag(ToolOptions::Option::Tip));
        CHECK(shows(o->strengthBox()) == want.testFlag(ToolOptions::Option::Strength));
        CHECK(shows(o->pressureBox()) == want.testFlag(ToolOptions::Option::Pressure));
        CHECK(shows(o->rampBox()) == want.testFlag(ToolOptions::Option::Ramp));
        CHECK(shows(o->alphaLockBox()) == want.testFlag(ToolOptions::Option::AlphaLock));
        CHECK(shows(o->lightBox()) == want.testFlag(ToolOptions::Option::Light));
        CHECK(shows(o->formBox()) == want.testFlag(ToolOptions::Option::Form));
        CHECK(shows(o->rimBox()) == want.testFlag(ToolOptions::Option::Form));
        CHECK(shows(o->hardnessBox()) == want.testFlag(ToolOptions::Option::Hardness));
        CHECK(shows(o->outlineSelectionBox()) == want.testFlag(ToolOptions::Option::OutlineSelection));
        CHECK(shows(o->symmetryBox()) == want.testFlag(ToolOptions::Option::Symmetry));
        CHECK(shows(o->foldsBox()) == want.testFlag(ToolOptions::Option::Symmetry));
        CHECK(shows(o->centreBox()) == want.testFlag(ToolOptions::Option::Symmetry));
        CHECK(shows(o->centreXBox()) == want.testFlag(ToolOptions::Option::Symmetry));
        CHECK(shows(o->pencil4Box()) == want.testFlag(ToolOptions::Option::Pencil));
        CHECK(shows(o->glyphLabel()) == want.testFlag(ToolOptions::Option::Glyph));
        CHECK(shows(o->filledBox()) == want.testFlag(ToolOptions::Option::Filled));
        CHECK(shows(o->pasteModeBox()) == want.testFlag(ToolOptions::Option::PasteMode));
        CHECK(shows(o->sampleMergedBox()) == want.testFlag(ToolOptions::Option::SampleMerged));
        CHECK(shows(o->gradientShapeBox()) == want.testFlag(ToolOptions::Option::Gradient));
        CHECK(shows(o->gradientFromBox()) == want.testFlag(ToolOptions::Option::Gradient));
        CHECK(shows(o->gradientToBox()) == want.testFlag(ToolOptions::Option::Gradient));
        CHECK(shows(o->gradientBandingBox()) == want.testFlag(ToolOptions::Option::Gradient));
        CHECK(shows(o->cloneAlignedBox()) == want.testFlag(ToolOptions::Option::CloneAligned));
        CHECK(shows(o->sprayKindBox()) == want.testFlag(ToolOptions::Option::Spray));
        CHECK(shows(o->sprayDensityBox()) == want.testFlag(ToolOptions::Option::Spray));
        CHECK(shows(o->spraySeedBox()) == want.testFlag(ToolOptions::Option::Spray));
        CHECK(shows(o->rimLintBox()) == want.testFlag(ToolOptions::Option::RimLint));
    }
    // spot checks against the tools themselves
    using O = ToolOptions::Option;
    CHECK(ToolOptions::optionsFor(QStringLiteral("keyboard")) == ToolOptions::Options());
    CHECK(ToolOptions::optionsFor(QStringLiteral("eyedropper")) == ToolOptions::Options(O::SampleMerged));
    CHECK(ToolOptions::optionsFor(QStringLiteral("fill")).testFlag(O::SampleMerged));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("halfBlock")).testFlag(O::SampleMerged));
    CHECK(ToolOptions::optionsFor(QStringLiteral("select")) == ToolOptions::Options(O::PasteMode));
    CHECK(ToolOptions::optionsFor(QStringLiteral("rampShade")).testFlag(O::Strength));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("shade")).testFlag(O::Strength));
    CHECK(ToolOptions::optionsFor(QStringLiteral("rectangle")).testFlag(O::Filled));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("line")).testFlag(O::Filled));
    CHECK(ToolOptions::optionsFor(QStringLiteral("halfBlock")).testFlag(O::Pencil));
    CHECK(ToolOptions::optionsFor(QStringLiteral("outline")).testFlag(O::OutlineSelection));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("rimLight")).testFlag(O::OutlineSelection));
    CHECK(ToolOptions::optionsFor(QStringLiteral("rimLight")).testFlag(O::RimLint));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("form")).testFlag(O::RimLint));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("unknown")).testFlag(O::RimLint));
    CHECK(ToolOptions::optionsFor(QStringLiteral("gradient")) == (O::AlphaLock | O::Ramp | O::Gradient));
    CHECK(ToolOptions::optionsFor(QStringLiteral("clone")).testFlag(O::CloneAligned));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("clone")).testFlag(O::Ramp));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("clone")).testFlag(O::Strength));
    CHECK(ToolOptions::optionsFor(QStringLiteral("spray")) == (ToolOptions::optionsFor(QStringLiteral("clone")) &
                                                               ~ToolOptions::Options(O::CloneAligned) | O::Ramp |
                                                               O::Spray));
    CHECK_FALSE(ToolOptions::optionsFor(QStringLiteral("texture")).testFlag(O::Spray));
}

TEST_CASE("every Tool Options control has a tooltip, with its shortcut where it has one") {
    MainWindow w;
    ToolOptions* o = w.toolOptions();
    QList<QWidget*> controls;
    for (QWidget* c : o->findChildren<QSpinBox*>())
        controls << c;
    for (QWidget* c : o->findChildren<QComboBox*>())
        controls << c;
    for (QWidget* c : o->findChildren<QCheckBox*>())
        controls << c;
    CHECK(controls.size() >= 23);
    for (QWidget* c : controls) {
        INFO(c->objectName().toStdString());
        CHECK_FALSE(c->toolTip().isEmpty());
    }
    CHECK(o->sizeBox()->toolTip().contains(keyText(act(w, QStringLiteral("brushSmaller"))->shortcut())));
    CHECK(o->sizeBox()->toolTip().contains(keyText(act(w, QStringLiteral("brushLarger"))->shortcut())));
    CHECK(o->symmetryBox()->toolTip().contains(keyText(act(w, QStringLiteral("mirrorMode"))->shortcut())));
    CHECK(o->filledBox()->toolTip().contains(keyText(act(w, QStringLiteral("filledShapes"))->shortcut())));
}

TEST_CASE("Tool Options' Filled follows Filled Shapes both ways") {
    MainWindow w;
    QAction* filled = act(w, QStringLiteral("filledShapes"));
    auto* rect = qobject_cast<ShapeTool*>(w.tools()->find(QStringLiteral("rectangle")));
    REQUIRE(rect);
    QCheckBox* box = w.toolOptions()->filledBox();
    CHECK_FALSE(box->isChecked());
    box->setChecked(true);
    CHECK(filled->isChecked());
    CHECK(rect->filled());
    filled->trigger();
    CHECK_FALSE(box->isChecked());
    CHECK_FALSE(rect->filled());
}

TEST_CASE("Tool Options' paste mode follows the select tool both ways") {
    MainWindow w;
    QComboBox* box = w.toolOptions()->pasteModeBox();
    CHECK(box->currentData().toInt() == int(PasteMode::Normal));
    box->setCurrentIndex(box->findData(int(PasteMode::Transparent)));
    emit box->activated(box->currentIndex());
    CHECK(w.selectTool()->pasteMode() == PasteMode::Transparent);
    CHECK(act(w, QStringLiteral("pasteTransparent"))->isChecked());
    act(w, QStringLiteral("pasteUnder"))->trigger();
    CHECK(box->currentData().toInt() == int(PasteMode::Under));
}

TEST_CASE("Tool Options shows the glyph brush's glyph") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    toolAction(w, QStringLiteral("glyph"))->trigger();
    auto* glyph = qobject_cast<GlyphTool*>(w.tools()->active());
    REQUIRE(glyph);
    const auto shown = [glyph] {
        const char32_t g = glyph->glyph();
        return QString::fromUcs4(&g, 1);
    };
    CHECK(w.toolOptions()->glyphLabel()->text() == shown());
    QKeyEvent f4(QEvent::KeyPress, Qt::Key_F4, Qt::NoModifier);
    QApplication::sendEvent(v, &f4);
    CHECK(glyph->glyph() == glyphSetGlyph(v->glyphSet(), 3));
    CHECK(w.toolOptions()->glyphLabel()->text() == shown());
}

TEST_CASE("the glyph bar types its glyphs, picks the glyph brush's, and follows the set") {
    MainWindow w;
    GlyphBar* bar = w.glyphBar();
    REQUIRE(bar);
    CanvasView* v = w.canvasView();
    CHECK(w.glyphBarDock()->objectName() == QStringLiteral("glyphBarDock"));
    CHECK(w.dockWidgetArea(w.glyphBarDock()) == Qt::BottomDockWidgetArea);
    for (int k = 0; k < kGlyphSetKeys; ++k) {
        REQUIRE(bar->cell(k));
        CHECK(bar->cell(k)->toolTip().startsWith(QStringLiteral("F%1:").arg(k + 1)));
        CHECK_FALSE(bar->cell(k)->icon().isNull());
    }
    QComboBox* sets = bar->setBox();
    CHECK(sets->count() == kGlyphSetCount);
    CHECK(sets->currentData().toInt() == v->glyphSet());

    // keyboard mode: a click types the glyph at the cursor, like its F-key
    REQUIRE(w.tools()->active()->id() == QStringLiteral("keyboard"));
    bar->cell(1)->click();
    CHECK(w.document()->canvas().at(0, 0).ch == glyphSetGlyph(v->glyphSet(), 1));
    CHECK(v->cellCursor().x == 1);

    // another set from the list, and from the view
    emit sets->activated(sets->findData(4));
    CHECK(v->glyphSet() == 4);
    v->setGlyphSet(6);
    CHECK(sets->currentData().toInt() == 6);
    const char32_t ch = glyphSetGlyph(6, 2);
    CHECK(bar->cell(2)->toolTip().contains(QString::fromUcs4(&ch, 1)));

    // the glyph brush: a click picks the glyph and types nothing
    toolAction(w, QStringLiteral("glyph"))->trigger();
    const Canvas before = w.document()->canvas();
    bar->cell(2)->click();
    CHECK(qobject_cast<GlyphTool*>(w.tools()->active())->glyph() == ch);
    CHECK(w.document()->canvas() == before);
}

TEST_CASE("the pen's swap arrow and reset corner") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    PaletteWidget* p = w.paletteWidget();
    // the parts of the pen don't overlap the swatches or each other's corners
    for (const QRect& r : {p->foregroundRect(), p->backgroundRect(), p->swapRect(), p->resetRect()}) {
        CHECK_FALSE(r.isEmpty());
        CHECK(p->penRect().contains(r));
        CHECK(p->indexAt(r.center()) == -1);
    }
    CHECK_FALSE(p->swapRect().intersects(p->foregroundRect()));
    CHECK_FALSE(p->swapRect().intersects(p->backgroundRect()));
    CHECK_FALSE(p->resetRect().intersects(p->foregroundRect()));
    CHECK_FALSE(p->resetRect().intersects(p->backgroundRect()));

    v->setForeground(Yellow);
    v->setBackground(Blue);
    press(p, p->swapRect().center());
    CHECK(v->pen().fg == Color::indexed(Blue));
    CHECK(v->pen().bg == Color::indexed(Yellow));
    CHECK(p->pen().fg == Color::indexed(Blue));
    act(w, QStringLiteral("swapColours"))->trigger();
    CHECK(v->pen().fg == Color::indexed(Yellow));
    press(p, p->resetRect().center());
    CHECK(v->pen().fg == Color::indexed(Grey));
    CHECK(v->pen().bg == Color::indexed(Black));
    // a right click on them does nothing
    v->setForeground(Red);
    press(p, p->swapRect().center(), Qt::RightButton);
    CHECK(v->pen().fg == Color::indexed(Red));

    // the foreground draws over the background, top left
    const QImage img = p->grab().toImage();
    CHECK(img.pixelColor(p->foregroundRect().center()) == QColor(0xaa, 0x00, 0x00));
    CHECK(img.pixelColor(p->backgroundRect().bottomRight() - QPoint(3, 3)) == QColor(0, 0, 0));

    // the reset's tooltip names its shortcut
    w.show();
    QHelpEvent tip(QEvent::ToolTip, p->resetRect().center(), p->mapToGlobal(p->resetRect().center()));
    QApplication::sendEvent(p, &tip);
    CHECK(QToolTip::text().contains(keyText(act(w, QStringLiteral("defaultColours"))->shortcut())));
    QToolTip::hideText();
}

TEST_CASE("the Window menu lists every dock and hides and shows it") {
    MainWindow w;
    w.show();
    QMenu* menu = windowMenu(w);
    REQUIRE(menu);
    const QList<QDockWidget*> docks = w.docks();
    CHECK(docks.size() == 9);
    CHECK(w.findChildren<QDockWidget*>().size() == docks.size());
    for (QDockWidget* d : docks) {
        INFO(d->objectName().toStdString());
        QAction* a = act(w, QStringLiteral("show_") + d->objectName());
        CHECK(menu->actions().contains(a));
        CHECK(a->isCheckable());
        CHECK(a->isChecked());
        // the preview, the palette and the Library are always shown; the
        // Layers dock can close like the toolbox
        const bool closable = d->features().testFlag(QDockWidget::DockWidgetClosable);
        CHECK(closable == (d != w.previewDock() && d != w.paletteDock() && d != w.libraryDock()));
        CHECK(a->isEnabled() == closable);
        if (!closable)
            continue;
        a->trigger();
        CHECK_FALSE(d->isVisible());
        CHECK_FALSE(a->isChecked());
        a->trigger();
        CHECK(d->isVisible());
    }
    // closing a dock unchecks its item
    w.glyphBarDock()->close();
    CHECK_FALSE(act(w, QStringLiteral("show_glyphBarDock"))->isChecked());
    CHECK(menu->actions().contains(act(w, QStringLiteral("resetLayout"))));
    CHECK(act(w, QStringLiteral("resetLayout"))->shortcut().isEmpty());
}

TEST_CASE("Reset Layout puts every dock back, shown and docked") {
    MainWindow w;
    w.show();
    w.removeDockWidget(w.toolboxDock());
    w.addDockWidget(Qt::RightDockWidgetArea, w.toolboxDock());
    w.toolboxDock()->show();
    w.addDockWidget(Qt::TopDockWidgetArea, w.libraryDock());
    w.toolOptionsDock()->setFloating(true);
    w.glyphBarDock()->hide();
    w.layersDock()->setFloating(true);
    w.paletteDock()->hide();
    w.tabifyDockWidget(w.previewDock(), w.paletteDock());
    REQUIRE(w.dockWidgetArea(w.toolboxDock()) == Qt::RightDockWidgetArea);

    act(w, QStringLiteral("resetLayout"))->trigger();
    const std::pair<QDockWidget*, Qt::DockWidgetArea> want[] = {
        {w.toolboxDock(), Qt::LeftDockWidgetArea},  {w.toolOptionsDock(), Qt::LeftDockWidgetArea},
        {w.previewDock(), Qt::RightDockWidgetArea}, {w.paletteDock(), Qt::RightDockWidgetArea},
        {w.layersDock(), Qt::RightDockWidgetArea},  {w.libraryDock(), Qt::RightDockWidgetArea},
        {w.lintDock(), Qt::RightDockWidgetArea},    {w.ircViewDock(), Qt::RightDockWidgetArea},
        {w.glyphBarDock(), Qt::BottomDockWidgetArea},
    };
    for (const auto& [d, area] : want) {
        INFO(d->objectName().toStdString());
        CHECK(w.dockWidgetArea(d) == area);
        CHECK_FALSE(d->isHidden());
        CHECK_FALSE(d->isFloating());
        CHECK(act(w, QStringLiteral("show_") + d->objectName())->isChecked());
        // Lint and the IRC View share a place, as tabs, Lint in front
        if (d == w.lintDock())
            CHECK(w.tabifiedDockWidgets(d) == QList<QDockWidget*>{w.ircViewDock()});
        else if (d == w.ircViewDock())
            CHECK(w.tabifiedDockWidgets(d) == QList<QDockWidget*>{w.lintDock()});
        else
            CHECK(w.tabifiedDockWidgets(d).isEmpty());
    }
    QApplication::processEvents();
    CHECK(w.lintWidget()->shown());
    CHECK_FALSE(w.ircViewWidget()->shown());
    // the toolbox is above its options
    CHECK(w.toolboxDock()->geometry().bottom() < w.toolOptionsDock()->geometry().top());
}

TEST_CASE("the right dock column can grow and shrink", "[dock-width]") {
    ClearLayout clear;
    MainWindow w;
    w.show();
    QApplication::processEvents();
    for (int width : {480, 300}) {
        w.resizeDocks({w.libraryDock()}, {width}, Qt::Horizontal);
        QApplication::processEvents();
        for (QDockWidget* d : {w.previewDock(), w.paletteDock(), w.layersDock(), w.libraryDock(), w.lintDock()}) {
            INFO(d->objectName().toStdString());
            CHECK(d->width() == width);
        }
        w.ircViewDock()->raise();
        QApplication::processEvents();
        CHECK(w.ircViewDock()->width() == width);
        w.lintDock()->raise();
    }
}

TEST_CASE("the left dock column can grow and shrink", "[dock-width]") {
    ClearLayout clear;
    MainWindow w;
    w.show();
    QApplication::processEvents();
    for (int width : {320, 200}) {
        w.resizeDocks({w.toolboxDock()}, {width}, Qt::Horizontal);
        QApplication::processEvents();
        CHECK(w.toolboxDock()->width() == width);
        CHECK(w.toolOptionsDock()->width() == width);
    }
}

TEST_CASE("Reset Layout restores the default dock column widths", "[dock-width]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == 256);
    CHECK(w.libraryDock()->width() == 360);
    w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {320, 480}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->width() == 320);
    REQUIRE(w.libraryDock()->width() == 480);
    act(w, QStringLiteral("resetLayout"))->trigger();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == 256);
    CHECK(w.toolOptionsDock()->width() == 256);
    CHECK(w.previewDock()->width() == 360);
    CHECK(w.paletteDock()->width() == 360);
    CHECK(w.layersDock()->width() == 360);
    CHECK(w.libraryDock()->width() == 360);
    CHECK(w.lintDock()->width() == 360);
}

TEST_CASE("default dock widths preserve room for the canvas", "[dock-width][dock-floor]") {
    ClearLayout clear;
    const QSize size = GENERATE(QSize(700, 500), QSize(1280, 720), QSize(1366, 768), QSize(1920, 1080));
    const bool reset = GENERATE(false, true);
    CAPTURE(size.width(), size.height(), reset);
    MainWindow w;
    if (reset) {
        // Reset must use the current size, rather than the size at first show.
        w.resize(1920, 1080);
        w.show();
        QApplication::processEvents();
        w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {320, 480}, Qt::Horizontal);
        QApplication::processEvents();
        w.resize(size);
        act(w, QStringLiteral("resetLayout"))->trigger();
    } else {
        w.resize(size);
        w.show();
    }
    QApplication::processEvents();
    auto* canvasScroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(canvasScroll);
    const int viewport = canvasScroll->viewport()->width();
    CAPTURE(viewport, w.toolboxDock()->width(), w.libraryDock()->width());
    if (viewport < 640) {
        // At 700px there isn't room even at the native dock minimums.
        CHECK(w.toolboxDock()->width() == w.toolboxDock()->minimumSizeHint().width());
        CHECK(w.libraryDock()->width() == w.paletteDock()->minimumSizeHint().width());
    } else {
        CHECK(viewport >= 640);
    }
    if (size.width() >= 1366) {
        CHECK(viewport >= 640);
        CHECK(w.toolboxDock()->width() == 256);
        CHECK(w.libraryDock()->width() == 360);
    } else if (size.width() == 1280) {
        // The right column alone can surrender enough space for the floor.
        CHECK(viewport >= 640);
        CHECK(w.toolboxDock()->width() == 256);
        CHECK(w.libraryDock()->width() < 360);
    }
}

TEST_CASE("the first-run window width fits the available screen", "[dock-width][dock-screen]") {
    ClearLayout clear;
    MainWindow w;
    w.show();
    QApplication::processEvents();
    const QSize available = w.screen()->availableGeometry().size();
    CAPTURE(w.width(), available.width());
    CHECK(w.width() <= available.width());
    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    if (scroll->viewport()->width() < 640) {
        CHECK(w.toolboxDock()->width() == w.toolboxDock()->minimumSizeHint().width());
        CHECK(w.libraryDock()->width() == w.paletteDock()->minimumSizeHint().width());
    }
}

TEST_CASE("first-run dock widths follow window sizing until a column is resized", "[dock-width][dock-settle]") {
    ClearLayout clear;
    MainWindow w;
    // Exercise both main()'s construct/show path and a larger starting window
    // followed by the WM fitting it to a 1280px work area after first show.
    const bool largeStart = GENERATE(false, true);
    CAPTURE(largeStart);
    if (largeStart)
        w.resize(1400, 900);
    w.show();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    REQUIRE(scroll);
    for (QSize size : {QSize(1280, 720), QSize(1366, 768), QSize(1280, 720)}) {
        CAPTURE(size.width(), size.height());
        w.resize(size);
        QApplication::processEvents();
        CHECK(scroll->viewport()->width() >= 640);
        CHECK(w.toolboxDock()->width() == 256);
        CHECK(w.libraryDock()->width() >= w.paletteDock()->minimumSizeHint().width());
    }
    // Manual widths take precedence, even if the canvas then falls below
    // the default floor. Returning to an earlier size must keep them too.
    const QSize manual = GENERATE(QSize(300, 400), QSize(230, 280));
    CAPTURE(manual.width(), manual.height());
    w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {manual.width(), manual.height()}, Qt::Horizontal);
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->width() == manual.width());
    REQUIRE(w.libraryDock()->width() == manual.height());
    for (QSize size : {QSize(1200, 720), QSize(1366, 768), QSize(1280, 720)}) {
        w.resize(size);
        QApplication::processEvents();
        CHECK(w.toolboxDock()->width() == manual.width());
        CHECK(w.libraryDock()->width() == manual.height());
    }
}

TEST_CASE("first-run dock widths recover after a one-step window squeeze", "[dock-width][dock-squeeze]") {
    ClearLayout clear;
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    REQUIRE(w.toolboxDock()->width() == 256);
    REQUIRE(w.libraryDock()->width() == 360);

    struct ResizeCounter : QObject {
        int count = 0;
        bool eventFilter(QObject*, QEvent* event) override {
            if (event->type() == QEvent::Resize)
                ++count;
            return false;
        }
    } counter;
    w.installEventFilter(&counter);
    for (QSize size : {QSize(600, 720), QSize(1920, 1080)}) {
        CAPTURE(size.width(), size.height());
        counter.count = 0;
        w.resize(size);
        QApplication::processEvents();
        CHECK(w.width() == size.width());
        CHECK(counter.count == 1);
    }
    CHECK(w.toolboxDock()->width() == 256);
    CHECK(w.libraryDock()->width() == 360);
    toolAction(w, QStringLiteral("text"))->trigger();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.toolOptionsDock()->widget());
    REQUIRE(scroll);
    CHECK(scroll->horizontalScrollBar()->maximum() == 0);
    CHECK_FALSE(scroll->horizontalScrollBar()->isVisible());
}

TEST_CASE("every tool's options fit the default left column without horizontal scrolling", "[dock-width][tool-options-width]") {
    ClearLayout clear;
    const QSize size = GENERATE(QSize(1280, 720), QSize(1920, 1080));
    CAPTURE(size.width(), size.height());
    MainWindow w;
    w.resize(size);
    w.show();
    QApplication::processEvents();
    auto* scroll = qobject_cast<QScrollArea*>(w.toolOptionsDock()->widget());
    REQUIRE(scroll);
    // A short options panel also covers the vertical scrollbar taking space.
    w.resizeDocks({w.toolOptionsDock()}, {200}, Qt::Vertical);
    for (Tool* tool : w.tools()->tools()) {
        INFO(tool->id().toStdString());
        toolAction(w, tool->id())->trigger();
        QApplication::processEvents();
        CHECK(scroll->horizontalScrollBar()->maximum() == 0);
        CHECK_FALSE(scroll->horizontalScrollBar()->isVisible());
    }
}

TEST_CASE("resized dock column widths survive closing the window", "[dock-width]") {
    ClearLayout clear;
    {
        MainWindow w;
        // Fit the offscreen screen so restoreGeometry keeps the same size.
        w.resize(760, 740);
        w.show();
        QApplication::processEvents();
        w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {200, 300}, Qt::Horizontal);
        QApplication::processEvents();
        REQUIRE(w.toolboxDock()->width() == 200);
        REQUIRE(w.libraryDock()->width() == 300);
        REQUIRE(w.close());
    }
    MainWindow w;
    w.show();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == 200);
    CHECK(w.toolOptionsDock()->width() == 200);
    CHECK(w.previewDock()->width() == 300);
    CHECK(w.paletteDock()->width() == 300);
    CHECK(w.layersDock()->width() == 300);
    CHECK(w.libraryDock()->width() == 300);
    CHECK(w.lintDock()->width() == 300);
    // A restored layout must never opt into the first-run sizing policy.
    for (QSize size : {QSize(1366, 768), QSize(1280, 720)}) {
        w.resize(size);
        QApplication::processEvents();
        CHECK(w.toolboxDock()->width() == 200);
        CHECK(w.libraryDock()->width() == 300);
    }
}

TEST_CASE("a legacy narrow dock layout gives way to the default widths", "[dock-width]") {
    ClearLayout clear;
    {
        MainWindow w;
        w.resize(760, 740);
        w.show();
        QApplication::processEvents();
        w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {160, 246}, Qt::Horizontal);
        w.glyphBarDock()->hide();
        QApplication::processEvents();
        REQUIRE(w.toolboxDock()->width() == 160);
        REQUIRE(w.libraryDock()->width() == 246);
        // Version 3 is the layout before the column-width defaults. Its
        // narrow widths must not override the new first-run layout.
        const QByteArray legacy = w.saveState(3);
        REQUIRE(w.close());
        appSettings()->setValue(QStringLiteral("window/state"), legacy);
    }
    MainWindow w;
    // Give the migrated layout room for the full defaults.
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == 256);
    CHECK(w.toolOptionsDock()->width() == 256);
    CHECK(w.previewDock()->width() == 360);
    CHECK(w.paletteDock()->width() == 360);
    CHECK(w.layersDock()->width() == 360);
    CHECK(w.libraryDock()->width() == 360);
    CHECK(w.lintDock()->width() == 360);
    CHECK(w.glyphBarDock()->isVisible());
    w.resizeDocks({w.toolboxDock(), w.libraryDock()}, {200, 300}, Qt::Horizontal);
    QApplication::processEvents();
    CHECK(w.toolboxDock()->width() == 200);
    CHECK(w.libraryDock()->width() == 300);
}

TEST_CASE("the app's settings are an INI file in the test's scratch directory") {
    // Before any window here is built or any setting cleared. QSettings::setPath()
    // left macOS and Windows on the real settings; the factory doesn't.
    const auto s = bw::appSettings();
    CHECK(s->format() == QSettings::IniFormat);
    CHECK(s->fileName() == bwtest::scratchSettingsFile());
    CHECK(QFileInfo(s->fileName()).absolutePath() == scratchDir());
    CHECK(scratchDir() != QDir::homePath());
    CHECK(bwtest::settingsInScratch(scratchDir()));
}

TEST_CASE("Qt's own settings are in the test's scratch directory too") {
    // A QFileDialog reads and writes QtProject's native settings, outside the
    // factory. Where QSettings::setPath() can move them, the test main put
    // them in the scratch directory; elsewhere no test may build a QFileDialog.
    if (!bwtest::kQtSettingsRedirectable)
        SKIP("Qt's native settings can't be moved off the user's on this platform");
    const QSettings qt(QSettings::UserScope, QStringLiteral("QtProject"));
    CHECK(bwtest::inDir(qt.fileName(), scratchDir()));
    CHECK(bwtest::qtSettingsInScratch(scratchDir()));
}

TEST_CASE("the dock layout survives closing the window") {
    ClearLayout clear;
    {
        MainWindow w;
        w.show();
        w.addDockWidget(Qt::RightDockWidgetArea, w.toolboxDock());
        w.glyphBarDock()->hide();
        REQUIRE(w.close());
    }
    {
        // the window kept it in the scratch INI file
        QSettings ini(bwtest::scratchSettingsFile(), QSettings::IniFormat);
        CHECK(ini.contains(QStringLiteral("window/state")));
        CHECK(ini.contains(QStringLiteral("window/geometry")));
    }
    {
        MainWindow w;
        CHECK(w.dockWidgetArea(w.toolboxDock()) == Qt::RightDockWidgetArea);
        CHECK_FALSE(w.glyphBarDock()->isVisibleTo(&w));
        CHECK_FALSE(act(w, QStringLiteral("show_glyphBarDock"))->isChecked());
        CHECK(w.toolOptionsDock()->isVisibleTo(&w));
        w.show();
        act(w, QStringLiteral("resetLayout"))->trigger();
        CHECK(w.dockWidgetArea(w.toolboxDock()) == Qt::LeftDockWidgetArea);
        CHECK(w.glyphBarDock()->isVisible());
        w.close();
    }
    {
        // the reset layout is what the next window starts with
        MainWindow w;
        CHECK(w.dockWidgetArea(w.toolboxDock()) == Qt::LeftDockWidgetArea);
        CHECK(w.glyphBarDock()->isVisibleTo(&w));
    }
}

TEST_CASE("a saved layout that hid a dock that can't close still shows it") {
    ClearLayout clear;
    {
        MainWindow w;
        w.show();
        w.previewDock()->hide();
        w.paletteDock()->hide();
        REQUIRE(w.close());
    }
    MainWindow w;
    CHECK(w.previewDock()->isVisibleTo(&w));
    CHECK(w.paletteDock()->isVisibleTo(&w));
}
