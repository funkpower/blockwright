// The shading brushes in the main window (PLAN §4, phase 2b): the form,
// soft, despeckle and smudge tools in the Tools menu and toolbar, their settings in
// Tool Options, and a stroke of each through the window system.

#include "tool_options.h"
#include "toolbox.h"
#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "tool.h"

#include "system_input.h"

#include "blockwright/brush.h"
#include "blockwright/brushes/form.h"
#include "blockwright/layer.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QMenu>
#include <QToolButton>
#include <QUndoStack>

#include <set>
#include <string>
#include <utility>

using namespace bw;

namespace {

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

const Cell& at(MainWindow& w, int x, int y) { return w.document()->canvas().at(x, y); }

int rung(MainWindow& w, int x, int y) {
    const auto g = rungOf(*findRamp("steel"), at(w, x, y));
    REQUIRE(g);
    return *g;
}

QPointF centre(CanvasView* v, int x, int y) { return QRectF(v->cellRect(QPoint(x, y))).center(); }

// A left-button drag through the window system, cell to cell along a row.
void systemDrag(CanvasView* v, int x0, int x1, int y, Qt::MouseButton button = Qt::LeftButton) {
    bwtest::systemMouse(v, QEvent::MouseButtonPress, centre(v, x0, y), button, button);
    for (int x = x0 + 1; x <= x1; ++x)
        bwtest::systemMouse(v, QEvent::MouseMove, centre(v, x, y), Qt::NoButton, button);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, centre(v, x1, y), button, Qt::NoButton);
}

std::set<std::pair<int, int>> changed(MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    std::set<std::pair<int, int>> s;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (!(c.at(x, y) == Cell{}))
                s.insert({x, y});
    return s;
}

} // namespace

TEST_CASE("the shading brushes are in the Tools menu and the toolbox, with their keys") {
    MainWindow w;
    // On a toolbox button, or in its group's menu.
    const auto inToolbox = [&w](QAction* a, const QString& id) {
        QToolButton* b = w.toolbox()->buttonFor(id);
        return b && (b->defaultAction() == a || (b->menu() && b->menu()->actions().contains(a)));
    };
    for (const auto& [id, key] : {std::pair{"form", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T)},
                                  std::pair{"soft", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N)},
                                  std::pair{"despeckle", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)},
                                  std::pair{"smudge", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M)}}) {
        INFO(id);
        QAction* a = act(w, QString::fromLatin1(id) + QStringLiteral("Tool"));
        CHECK(a->shortcut() == key);
        CHECK(inToolbox(a, QString::fromLatin1(id)));
        use(w, id);
        CHECK(a->isChecked());
    }
    // every tool is in the toolbox
    for (Tool* t : w.tools()->tools())
        CHECK(inToolbox(act(w, t->id() + QStringLiteral("Tool")), t->id()));
}

TEST_CASE("Tool Options sets the form, the light, rim light, glint and hardness") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    auto* dock = w.findChild<ToolOptions*>();
    REQUIRE(dock);
    CHECK(v->formOptions() == FormOptions{});
    CHECK(dock->lightBox()->currentData().toInt() == int(Compass::NW));
    CHECK(dock->lightBox()->count() == 8);
    dock->formBox()->setCurrentIndex(dock->formBox()->findData(int(FormShape::Cylinder)));
    dock->lightBox()->setCurrentIndex(dock->lightBox()->findData(int(Compass::E)));
    dock->rimBox()->setChecked(true);
    dock->specularBox()->setChecked(false);
    dock->hardnessBox()->setValue(50);
    CHECK(v->formOptions() == FormOptions{FormShape::Cylinder, true, false});
    // the form brush is lit from the one light the rim-light brush uses
    CHECK(v->brushSettings().lightDirection == Compass::E);
    CHECK(v->hardness() == 0.5);
    // and follows the view
    v->setFormOptions(FormOptions{FormShape::Plane, false, true});
    BrushSettings bs = v->brushSettings();
    bs.lightDirection = Compass::S;
    v->setBrushSettings(bs);
    v->setHardness(2.0);
    CHECK(dock->formBox()->currentData().toInt() == int(FormShape::Plane));
    CHECK(dock->lightBox()->currentData().toInt() == int(Compass::S));
    CHECK_FALSE(dock->rimBox()->isChecked());
    CHECK(dock->specularBox()->isChecked());
    CHECK(dock->hardnessBox()->value() == 100);
    // the status bar names the form tool's settings
    use(w, "form");
    CHECK(w.tools()->active()->status().contains(QStringLiteral("plane")));
}

TEST_CASE("form brush: a mouse stroke shades the selection as a dome, one undo step") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "form");
    v->setBrushSettings(BrushSettings{3, Tip::Square, 1.0, PressureTo::Nothing});
    const CellRect box = CellRect::fromCorners(10, 2, 33, 13);
    v->setSelection(Selection(box));
    const Canvas& c = w.document()->canvas();
    Mask shape(c.width(), c.height());
    for (int y = box.y; y < box.y + box.height; ++y)
        for (int x = box.x; x < box.x + box.width; ++x)
            shape.set(x, y);
    const auto tones = formRungs(*findRamp("steel"), shape);

    // the preview shows while the drag goes on, and lands on release
    bwtest::systemMouse(v, QEvent::MouseButtonPress, centre(v, 12, 7), Qt::LeftButton, Qt::LeftButton);
    for (int x = 13; x <= 31; ++x)
        bwtest::systemMouse(v, QEvent::MouseMove, centre(v, x, 7), Qt::NoButton, Qt::LeftButton);
    auto* tool = qobject_cast<DragTool*>(w.tools()->active());
    REQUIRE(tool);
    CHECK_FALSE(tool->pending().changes.empty());
    CHECK(changed(w).empty());
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, centre(v, 31, 7), Qt::LeftButton, Qt::NoButton);
    CHECK(w.document()->undoStack()->count() == 1);
    const auto cells = changed(w);
    REQUIRE_FALSE(cells.empty());
    for (const auto& [x, y] : cells) {
        INFO(x << "," << y);
        CHECK(box.contains(x, y));
        CHECK(rung(w, x, y) == tones[static_cast<size_t>(y) * c.width() + x]);
    }
    // lit from the top left
    CHECK(rung(w, 15, 6) > rung(w, 29, 8));
    w.document()->undoStack()->undo();
    CHECK(changed(w).empty());
}

TEST_CASE("form brush in mirror mode shades both halves") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "form");
    v->setBrushSettings(BrushSettings{5, Tip::Round, 1.0, PressureTo::Nothing});
    act(w, QStringLiteral("mirrorMode"))->trigger();
    REQUIRE(v->mirror());
    systemDrag(v, 6, 14, 6);
    const int width = w.document()->canvas().width();
    const auto cells = changed(w);
    REQUIRE_FALSE(cells.empty());
    for (const auto& [x, y] : cells) {
        INFO(x << "," << y);
        CHECK(cells.count({width - 1 - x, y}));
    }
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("soft brush: a stylus stroke lightens the middle most, one undo step") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "soft");
    v->setBrushSettings(BrushSettings{15, Tip::Round, 1.0, PressureTo::Strength});
    // through the window system, the pen pressing harder along the row
    bwtest::systemTablet(v, centre(v, 20, 10), Qt::LeftButton, 0.6);
    for (int x = 21; x <= 30; ++x)
        bwtest::systemTablet(v, centre(v, x, 10), Qt::LeftButton, 0.6 + 0.04 * (x - 20));
    bwtest::systemTablet(v, centre(v, 30, 10), Qt::NoButton, 0.0);
    CHECK(w.document()->undoStack()->count() == 1);
    CHECK(rung(w, 30, 10) == 4);  // the middle, at full pressure: a colour
    CHECK(rung(w, 30, 10) > rung(w, 35, 10));
    CHECK(rung(w, 37, 10) == 1);  // the tip's edge
    CHECK(rung(w, 38, 10) == 0);  // past it
    // the right button darkens it back down
    systemDrag(v, 30, 30, 10, Qt::RightButton);
    CHECK(rung(w, 30, 10) < 4);
    CHECK(w.document()->undoStack()->count() == 2);
}

TEST_CASE("despeckle brush: a mouse stroke clears a speck") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    // a lone grey cell on black, with the ramp brush
    use(w, "rampShade");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    systemDrag(v, 20, 20, 8);
    systemDrag(v, 40, 40, 8);
    REQUIRE(rung(w, 20, 8) == 4);
    use(w, "despeckle");
    v->setBrushSettings(BrushSettings{3, Tip::Square, 1.0, PressureTo::Nothing});
    systemDrag(v, 18, 22, 8);
    CHECK(rung(w, 20, 8) == 0);
    CHECK(rung(w, 40, 8) == 4);  // outside the stroke
    CHECK(w.document()->undoStack()->count() == 3);
}

namespace {

// A band of 8s meeting 6s on row 8, x 10 to 21, as one undo step.
void band(MainWindow& w) {
    const Ramp r = *findRamp("steel");
    CellEdit e;
    for (int x = 10; x < 22; ++x)
        e.changes.push_back({x, 8, Cell{}, rungCell(r, x < 16 ? 8 : 6)});
    REQUIRE(w.document()->edit(e));
}

std::string bandRungs(MainWindow& w) {
    std::string s;
    for (int x = 10; x < 22; ++x)
        s += char('0' + rung(w, x, 8));
    return s;
}

} // namespace

TEST_CASE("smudge brush: a mouse stroke drags one plane over the next, previewed as it lands, one undo step") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(*findRamp("steel"));
    band(w);
    use(w, "smudge");
    using O = ToolOptions::Option;
    CHECK(ToolOptions::optionsFor(QStringLiteral("smudge")).testFlag(O::Strength));
    CHECK(ToolOptions::optionsFor(QStringLiteral("smudge")).testFlag(O::Ramp));
    CHECK(w.tools()->active()->status() == QStringLiteral("Ramp steel"));
    w.toolOptions()->strengthBox()->setValue(50);
    v->setBrushSettings(BrushSettings{1, Tip::Square, 0.5, PressureTo::Nothing});
    SECTION("the left button") {
        bwtest::systemMouse(v, QEvent::MouseButtonPress, centre(v, 11, 8), Qt::LeftButton, Qt::LeftButton);
        for (int x = 12; x <= 20; ++x)
            bwtest::systemMouse(v, QEvent::MouseMove, centre(v, x, 8), Qt::NoButton, Qt::LeftButton);
        auto* t = qobject_cast<DragTool*>(w.tools()->active());
        REQUIRE(t);
        const CellEdit shown = t->pending();
        CHECK_FALSE(shown.empty());
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, centre(v, 20, 8), Qt::LeftButton, Qt::NoButton);
        for (const CellChange& c : shown.changes)
            CHECK(at(w, c.x, c.y) == c.after);
        CHECK(bandRungs(w) == "888888776666");
        CHECK(w.document()->undoStack()->count() == 2);
        w.document()->undoStack()->undo();
        CHECK(bandRungs(w) == "888888666666");
    }
    SECTION("the right button smudges too") {
        systemDrag(v, 11, 20, 8, Qt::RightButton);
        CHECK(bandRungs(w) == "888888776666");
    }
    SECTION("the selection stops it") {
        v->setSelection(Selection(CellRect::fromCorners(10, 8, 16, 8)));
        systemDrag(v, 11, 20, 8);
        CHECK(bandRungs(w) == "888888766666");
    }
}

TEST_CASE("smudge brush: it smudges the active layer, through the alpha lock", "[layers]") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    Document* doc = w.document();
    v->setRamp(*findRamp("steel"));
    band(w);
    // a cells layer over the band, clear but for its own band at 17
    doc->addLayer(LayerKind::Cells);
    const Ramp r = *findRamp("steel");
    CellEdit e;
    for (int x = 10; x < 22; ++x)
        if (x != 17)
            e.changes.push_back({x, 8, transparentCell(), rungCell(r, x < 16 ? 8 : 6)});
    REQUIRE(doc->edit(e));
    const Canvas below = doc->layers().layers().front().cells;
    use(w, "smudge");
    w.toolOptions()->alphaLockBox()->setChecked(true);
    v->setBrushSettings(BrushSettings{1, Tip::Square, 0.5, PressureTo::Nothing});
    systemDrag(v, 11, 20, 8);
    // laid on the layer, 7 at 16; 17 is clear and stays so, and the carry
    // starts again past it
    CHECK(rungOf(r, doc->layerCanvas().at(16, 8)) == 7);
    CHECK(doc->layerCanvas().at(17, 8) == transparentCell());
    CHECK(rungOf(r, doc->layerCanvas().at(18, 8)) == 6);
    CHECK(doc->layers().layers().front().cells == below);
}
