// The brushes' common behaviour in the main window (PLAN §4, phase 2b.1):
// Tool Options' size, tip, strength, pressure mapping, ramp and alpha
// lock, the ramp shade brush, and a stylus's pressure through QTabletEvent.

#include "tool_options.h"
#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "tool.h"

#include "system_input.h"

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/layer.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabletEvent>
#include <QUndoStack>

#include <algorithm>
#include <random>
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

// The view pixel in the middle of half-block pixel (x, py).
QPointF pixel(CanvasView* v, int x, int py) {
    const QRect r = v->cellRect(QPoint(x, py >> 1));
    return QPointF(r.center().x(), r.top() + r.height() * ((py & 1) ? 0.75 : 0.25));
}

// `device` is the mouse by default; a stylus device makes the mouse event
// Qt synthesizes from a tablet event no widget took.
void mouse(CanvasView* v, QEvent::Type type, int x, int py, Qt::MouseButton button = Qt::LeftButton,
           const QPointingDevice* device = QPointingDevice::primaryPointingDevice()) {
    const QPointF at = pixel(v, x, py);
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent e(type, at, v->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : button, held,
                  Qt::NoModifier, device);
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

// A pen on a tablet: position and pressure.
const QPointingDevice* stylus() {
    static const QPointingDevice dev(QStringLiteral("test stylus"), 4242, QInputDevice::DeviceType::Stylus,
                                     QPointingDevice::PointerType::Pen,
                                     QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1,
                                     3);
    return &dev;
}

// Sends a tablet event at half-block pixel (x, py) with the buttons `held`
// after it (a barrel button among them); returns whether the view accepted
// it (so Qt makes no mouse event of it).
bool tabletHeld(CanvasView* v, QEvent::Type type, int x, int py, double pressure, Qt::MouseButton button,
                Qt::MouseButtons held) {
    const QPointF at = pixel(v, x, py);
    QTabletEvent e(type, stylus(), at, v->mapToGlobal(at), pressure, 0, 0, 0, 0, 0, Qt::NoModifier,
                   type == QEvent::TabletMove ? Qt::NoButton : button, held);
    e.setAccepted(false);
    QApplication::sendEvent(v, &e);
    return e.isAccepted();
}

bool tablet(CanvasView* v, QEvent::Type type, int x, int py, double pressure,
            Qt::MouseButton button = Qt::LeftButton) {
    return tabletHeld(v, type, x, py, pressure, button,
                      type == QEvent::TabletRelease ? Qt::NoButton : Qt::MouseButtons(button));
}

// A mouse button event with the buttons `held` after it given outright.
void mouseHeld(CanvasView* v, QEvent::Type type, int x, int py, Qt::MouseButton button, Qt::MouseButtons held) {
    const QPointF at = pixel(v, x, py);
    QMouseEvent e(type, at, v->mapToGlobal(at), button, held, Qt::NoModifier);
    QApplication::sendEvent(v, &e);
}


void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

const Cell& at(MainWindow& w, int x, int y) { return w.document()->canvas().at(x, y); }

DragTool* dragTool(MainWindow& w) {
    auto* t = qobject_cast<DragTool*>(w.tools()->active());
    REQUIRE(t);
    return t;
}

// The cells an edit changes.
std::set<std::pair<int, int>> cellsOf(const CellEdit& e) {
    std::set<std::pair<int, int>> s;
    for (const CellChange& c : e.changes)
        s.insert({c.x, c.y});
    return s;
}

// The cells that differ from the mirror image of their mirror cell.
std::set<std::pair<int, int>> asymmetric(MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    CellEdit left;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < (c.width() + 1) / 2; ++x)
            left.changes.push_back({x, y, c.at(x, y), c.at(x, y)});
    return cellsOf(mirrorEdit(c, left));
}

// The cells that differ from a blank canvas.
std::set<std::pair<int, int>> changed(MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    std::set<std::pair<int, int>> s;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (!(c.at(x, y) == Cell{}))
                s.insert({x, y});
    return s;
}

std::set<std::pair<int, int>> tipAt(int x, int y, int size, Tip tip, MainWindow& w) {
    const Canvas& c = w.document()->canvas();
    const auto cells = footprint(x, y, size, tip, c.width(), c.height());
    return {cells.begin(), cells.end()};
}

Ramp ramp(const char* name) {
    const auto r = findRamp(name);
    REQUIRE(r);
    return *r;
}

} // namespace

TEST_CASE("Tool Options is in the window and shows the view's settings") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QDockWidget* dock = w.toolOptionsDock();
    REQUIRE(dock);
    CHECK(dock->objectName() == QStringLiteral("toolOptionsDock"));
    CHECK(dock->isAncestorOf(w.toolOptions()));
    ToolOptions* d = w.toolOptions();
    // defaults: size 1, square, a quarter strength (a rung a pass), pressure
    // to size, steel, no lock
    CHECK(v->brushSettings().size == 1);
    CHECK(v->brushSettings().tip == Tip::Square);
    CHECK(v->brushSettings().strength == 0.25);
    CHECK(v->brushSettings().pressure == PressureTo::Size);
    CHECK(v->ramp().name == "steel");
    CHECK_FALSE(v->lockTransparent());
    CHECK(d->sizeBox()->value() == 1);
    CHECK(d->strengthBox()->value() == 25);
    CHECK(d->rampBox()->currentText() == QStringLiteral("steel"));
    CHECK(d->rampBox()->count() == int(namedRamps().size()));

    d->sizeBox()->setValue(4);
    d->tipBox()->setCurrentIndex(d->tipBox()->findData(int(Tip::Round)));
    d->strengthBox()->setValue(50);
    d->pressureBox()->setCurrentIndex(d->pressureBox()->findData(int(PressureTo::Strength)));
    d->rampBox()->setCurrentIndex(d->rampBox()->findText(QStringLiteral("fire")));
    d->alphaLockBox()->setChecked(true);
    CHECK(v->brushSettings() == BrushSettings{4, Tip::Round, 0.5, PressureTo::Strength});
    CHECK(v->ramp().name == "fire");
    CHECK(v->lockTransparent());

    // and follows the view
    BrushSettings s = v->brushSettings();
    s.size = 9;
    v->setBrushSettings(s);
    v->setRamp(ramp("ice"));
    v->setLockTransparent(false);
    CHECK(d->sizeBox()->value() == 9);
    CHECK(d->rampBox()->currentText() == QStringLiteral("ice"));
    CHECK_FALSE(d->alphaLockBox()->isChecked());
    CHECK(d->tipBox()->currentData().toInt() == int(Tip::Round));
}

TEST_CASE("the view clamps the brush size and strength and refuses a bad ramp") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    v->setBrushSettings(BrushSettings{0, Tip::Square, -1.0, PressureTo::Nothing});
    CHECK(v->brushSettings().size == 1);
    CHECK(v->brushSettings().strength == 0.0);
    v->setBrushSettings(BrushSettings{kMaxBrushSize + 5, Tip::Square, 3.0, PressureTo::Nothing});
    CHECK(v->brushSettings().size == kMaxBrushSize);
    CHECK(v->brushSettings().strength == 1.0);
    v->setRamp(Ramp{"one", {1}});
    CHECK(v->ramp().name == "steel");
}

TEST_CASE("Ctrl+[ and Ctrl+] step the brush size, and the dock follows") {
    MainWindow w;
    QAction* smaller = act(w, QStringLiteral("brushSmaller"));
    QAction* larger = act(w, QStringLiteral("brushLarger"));
    CHECK(smaller->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
    CHECK(larger->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
    larger->trigger();
    larger->trigger();
    CHECK(w.canvasView()->brushSettings().size == 3);
    CHECK(w.toolOptions()->sizeBox()->value() == 3);
    for (int i = 0; i < 5; ++i)
        smaller->trigger();
    CHECK(w.canvasView()->brushSettings().size == 1);
}

TEST_CASE("the brushes paint with the dock's size and tip") {
    // (the half-block brush is a one-pixel pencil and keeps to it)
    for (const char* id : {"shade", "rampShade", "glyph", "colorize"}) {
        for (const Tip tip : {Tip::Square, Tip::Round}) {
            INFO(id << " tip " << int(tip));
            MainWindow w;
            CanvasView* v = w.canvasView();
            use(w, id);
            v->setForeground(LightGreen);
            v->setBackground(Blue);
            v->setBrushSettings(BrushSettings{5, tip, 1.0, PressureTo::Nothing});
            drag(v, {{10, 10}});
            CHECK(changed(w) == tipAt(10, 5, 5, tip, w));
            CHECK(w.document()->undoStack()->count() == 1);
        }
    }
}

TEST_CASE("ramp shade brush: left lightens along the dock's ramp, right darkens, one undo step a stroke") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    QUndoStack* undo = w.document()->undoStack();
    auto* label = w.findChild<QLabel*>(QStringLiteral("toolStatusLabel"));
    REQUIRE(label);
    use(w, "rampShade");
    CHECK(act(w, QStringLiteral("rampShadeTool"))->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_T));
    CHECK(label->text() == QStringLiteral("Ramp steel"));
    const Ramp steel = ramp("steel");

    // a quarter strength: a rung a pass, blank (black) is rung 0
    drag(v, {{0, 0}, {6, 0}});
    for (int x = 0; x <= 6; ++x)
        CHECK(rungOf(steel, at(w, x, 0)) == 1);
    CHECK(undo->count() == 1);
    drag(v, {{0, 0}, {2, 0}});
    CHECK(rungOf(steel, at(w, 1, 0)) == 2);
    CHECK(rungOf(steel, at(w, 4, 0)) == 1);
    CHECK(undo->count() == 2);
    drag(v, {{1, 0}, {4, 0}}, Qt::RightButton);
    CHECK(rungOf(steel, at(w, 1, 0)) == 1);
    CHECK(rungOf(steel, at(w, 4, 0)) == 0);
    CHECK(undo->count() == 3);
    undo->undo();
    CHECK(rungOf(steel, at(w, 1, 0)) == 2);
    CHECK(rungOf(steel, at(w, 4, 0)) == 1);

    // full strength: a whole colour a pass
    w.toolOptions()->strengthBox()->setValue(100);
    drag(v, {{10, 0}});
    CHECK(rungOf(steel, at(w, 10, 0)) == 4);
    CHECK(at(w, 10, 0) == rungCell(steel, 4));

    // another ramp from the dock: off-ramp cells (steel's grey) are left
    // alone, the status says which
    w.toolOptions()->rampBox()->setCurrentIndex(w.toolOptions()->rampBox()->findText(QStringLiteral("fire")));
    CHECK(label->text() == QStringLiteral("Ramp fire"));
    const Cell grey = at(w, 10, 0);
    drag(v, {{10, 0}, {12, 0}});
    CHECK(at(w, 10, 0) == grey);
    CHECK(at(w, 12, 0) == rungCell(ramp("fire"), 4));
}

TEST_CASE("changing the brush mid-drag redoes the preview, and release lands it") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rampShade");
    mouse(v, QEvent::MouseButtonPress, 10, 10);
    mouse(v, QEvent::MouseMove, 14, 10);
    CHECK(dragTool(w)->pending().changes.size() == 5);
    w.toolOptions()->sizeBox()->setValue(3);
    CHECK(cellsOf(dragTool(w)->pending()).size() == 3 * 7);
    w.toolOptions()->rampBox()->setCurrentIndex(w.toolOptions()->rampBox()->findText(QStringLiteral("fire")));
    for (const CellChange& c : dragTool(w)->pending().changes)
        CHECK(c.after == rungCell(ramp("fire"), 1));
    const CellEdit shown = dragTool(w)->pending();
    mouse(v, QEvent::MouseButtonRelease, 14, 10);
    for (const CellChange& c : shown.changes)
        CHECK(at(w, c.x, c.y) == c.after);
    CHECK(changed(w) == cellsOf(shown));
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("the alpha lock leaves transparent cells alone") {
    for (const char* id : {"halfBlock", "shade", "rampShade", "glyph", "colorize"}) {
        INFO(id);
        MainWindow w;
        CanvasView* v = w.canvasView();
        Document* doc = w.document();
        // a cells layer over the background, opaque at 0 and 2, clear at 1
        doc->addLayer(LayerKind::Cells);
        const Cell clear = transparentCell();
        REQUIRE(doc->edit(CellEdit{{{0, 0, clear, Cell{}}, {2, 0, clear, Cell{}}}}));
        use(w, id);
        v->setForeground(LightGreen);
        w.toolOptions()->alphaLockBox()->setChecked(true);
        CHECK(doc->active()->props.alphaLocked);
        drag(v, {{0, 0}, {2, 0}});
        CHECK(doc->layerCanvas().at(1, 0) == clear);
        CHECK_FALSE(doc->layerCanvas().at(0, 0) == Cell{});
        CHECK_FALSE(doc->layerCanvas().at(2, 0) == Cell{});
        w.toolOptions()->alphaLockBox()->setChecked(false);
        drag(v, {{1, 0}});
        // colorize recolours what is there, and a hole has nothing to recolour
        CHECK((doc->layerCanvas().at(1, 0) == clear) == (std::string(id) == "colorize"));
    }
}

TEST_CASE("a stylus paints through the tool, its pressure scaling the size") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{5, Tip::Square, 1.0, PressureTo::Size});
    // 5 × 0.6 is 3 cells
    CHECK(tablet(v, QEvent::TabletPress, 10, 10, 0.6));
    CHECK(cellsOf(dragTool(w)->pending()) == tipAt(10, 5, 3, Tip::Square, w));
    // pressing harder where it is grows the dab
    CHECK(tablet(v, QEvent::TabletMove, 10, 10, 1.0));
    CHECK(cellsOf(dragTool(w)->pending()) == tipAt(10, 5, 5, Tip::Square, w));
    // a release reads no pressure (0): it lands what the stroke showed
    const CellEdit shown = dragTool(w)->pending();
    CHECK(tablet(v, QEvent::TabletRelease, 10, 10, 0.0));
    CHECK(changed(w) == cellsOf(shown));
    CHECK(w.document()->undoStack()->count() == 1);

    // a mouse presses fully
    drag(v, {{30, 10}});
    CHECK(changed(w).count({32, 5}));
    CHECK(changed(w).count({28, 5}));
}

TEST_CASE("a stylus's pressure can scale the ramp brush's strength instead") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rampShade");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Strength});
    const Ramp steel = ramp("steel");
    // full strength is four rungs; half the pressure two, a light touch one
    CHECK(tablet(v, QEvent::TabletPress, 0, 0, 0.5));
    CHECK(tablet(v, QEvent::TabletRelease, 0, 0, 0.0));
    CHECK(rungOf(steel, at(w, 0, 0)) == 2);
    tablet(v, QEvent::TabletPress, 2, 0, 0.05);
    tablet(v, QEvent::TabletRelease, 2, 0, 0.0);
    CHECK(rungOf(steel, at(w, 2, 0)) == 1);
    tablet(v, QEvent::TabletPress, 4, 0, 1.0);
    tablet(v, QEvent::TabletRelease, 4, 0, 0.0);
    CHECK(rungOf(steel, at(w, 4, 0)) == 4);
    CHECK(w.document()->undoStack()->count() == 3);
}

TEST_CASE("a stylus event the tool doesn't take is left for the mouse") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    w.tools()->setActive(QStringLiteral("keyboard"));
    CHECK_FALSE(tablet(v, QEvent::TabletPress, 4, 4, 0.5));
    CHECK_FALSE(tablet(v, QEvent::TabletRelease, 4, 4, 0.0));
    CHECK(w.document()->undoStack()->count() == 0);
}

TEST_CASE("mirror mode with an even brush paints the exact mirror") {
    for (const char* id : {"shade", "rampShade", "glyph"}) {
        for (const Tip tip : {Tip::Square, Tip::Round}) {
            INFO(id << " tip " << int(tip));
            MainWindow w;
            CanvasView* v = w.canvasView();
            use(w, id);
            v->setForeground(LightGreen);
            v->setBrushSettings(BrushSettings{4, tip, 0.25, PressureTo::Nothing});
            act(w, QStringLiteral("mirrorMode"))->trigger();
            REQUIRE(v->mirror());
            drag(v, {{6, 10}});
            const int width = w.document()->canvas().width();
            const auto cells = changed(w);
            for (const auto& [x, y] : cells) {
                INFO("cell " << x << "," << y);
                CHECK(cells.count({width - 1 - x, y}));
            }
            CHECK(cells.size() == 2 * tipAt(6, 5, 4, tip, w).size());
        }
    }
}

TEST_CASE("a release lands with the stroke's last pressure, not the release's") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{5, Tip::Square, 1.0, PressureTo::Size});
    // The release moves to another cell and reads 0: the line out to it is
    // at the stroke's pressure (1.0, five rows), not at size 1.
    CHECK(tablet(v, QEvent::TabletPress, 10, 10, 1.0));
    CHECK(tablet(v, QEvent::TabletRelease, 20, 10, 0.0));
    CHECK(changed(w).size() == 5u * 15u);
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("pressing harder on the touch-down cell repaints it at the stronger strength") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rampShade");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Strength});
    const Ramp steel = ramp("steel");
    CHECK(tablet(v, QEvent::TabletPress, 0, 0, 0.05));
    CHECK(rungOf(steel, dragTool(w)->pending().changes.at(0).after) == 1);
    CHECK(tablet(v, QEvent::TabletMove, 0, 0, 0.5));
    CHECK(rungOf(steel, dragTool(w)->pending().changes.at(0).after) == 2);
    CHECK(tablet(v, QEvent::TabletMove, 0, 0, 1.0));
    // easing off leaves it at the peak
    CHECK(tablet(v, QEvent::TabletMove, 0, 0, 0.3));
    CHECK(tablet(v, QEvent::TabletRelease, 0, 0, 0.0));
    CHECK(rungOf(steel, at(w, 0, 0)) == 4);
    CHECK(w.document()->undoStack()->count() == 1);
    // A light touch-down, then a line out at full pressure: one pass, the
    // start cell as deep as the rest.
    CHECK(tablet(v, QEvent::TabletPress, 0, 2, 0.05));
    CHECK(tablet(v, QEvent::TabletMove, 0, 2, 1.0));
    CHECK(tablet(v, QEvent::TabletMove, 3, 2, 1.0));
    CHECK(tablet(v, QEvent::TabletRelease, 3, 2, 0.0));
    for (int x = 0; x <= 3; ++x)
        CHECK(rungOf(steel, at(w, x, 1)) == 4);
}

TEST_CASE("with iCE off the ramp brush writes no shade that needs a bright background") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    act(w, QStringLiteral("iceColours"))->trigger();  // iCE off, as a file without the flag opens
    REQUIRE_FALSE(w.document()->ansi().ice);
    use(w, "rampShade");
    const Ramp fire = ramp("fire");  // Black Red LightRed Yellow White
    v->setRamp(fire);
    BrushSettings s{1, Tip::Square, 1.0, PressureTo::Nothing};
    v->setBrushSettings(s);
    drag(v, {{0, 0}});
    drag(v, {{0, 0}});
    REQUIRE(rungOf(fire, at(w, 0, 0)) == 8);  // LightRed
    // A rung more is a LightRed/Yellow shade: two brights, a bright
    // background either way round, which iCE off would show blinking on Red.
    s.strength = 0.25;
    v->setBrushSettings(s);
    drag(v, {{0, 0}});
    CHECK(at(w, 0, 0).bg.paletteIndex() < 8);
    CHECK(rungOf(fire, iceCell(at(w, 0, 0), false)) == 8);
    // Every ramp, every strength, up then down: each cell written shows as a
    // rung of the ramp with iCE off.
    for (const Ramp& r : namedRamps()) {
        v->setRamp(r);
        for (const double strength : {0.25, 0.5, 0.75, 1.0}) {
            INFO(r.name << " strength " << strength);
            s.strength = strength;
            v->setBrushSettings(s);
            for (const Qt::MouseButton b : {Qt::LeftButton, Qt::RightButton})
                for (int pass = 0; pass < 2 * r.topRung(); ++pass) {
                    drag(v, {{2, 0}}, b);
                    const Cell& c = at(w, 2, 0);
                    CHECK(c.bg.paletteIndex() < 8);
                    CHECK(rungOf(r, iceCell(c, false)));
                }
        }
    }
}

TEST_CASE("the device that starts a drag owns it") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    const std::set<std::pair<int, int>> two = {{2, 2}, {3, 2}};

    SECTION("a stylus during a mouse drag neither extends nor lands it") {
        mouse(v, QEvent::MouseButtonPress, 2, 4);
        // taken (so Qt makes no mouse event of it), and ignored
        CHECK(tablet(v, QEvent::TabletPress, 20, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletMove, 21, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 21, 4, 0.0));
        CHECK(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 0);
        mouse(v, QEvent::MouseMove, 3, 4);
        mouse(v, QEvent::MouseButtonRelease, 3, 4);
        CHECK(changed(w) == two);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a mouse during a stylus drag neither extends nor lands it") {
        CHECK(tablet(v, QEvent::TabletPress, 2, 4, 1.0));
        mouse(v, QEvent::MouseButtonPress, 20, 4);
        mouse(v, QEvent::MouseMove, 21, 4);
        mouse(v, QEvent::MouseButtonRelease, 21, 4);
        CHECK(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 0);
        CHECK(tablet(v, QEvent::TabletMove, 3, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 3, 4, 0.0));
        CHECK(changed(w) == two);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a stylus drag ignores mouse events made from the stylus too") {
        // A platform that makes mouse events of every stylus event: they
        // don't start the drag again or add to it at full pressure.
        CHECK(tablet(v, QEvent::TabletPress, 2, 4, 1.0));
        mouse(v, QEvent::MouseButtonPress, 2, 4, Qt::LeftButton, stylus());
        mouse(v, QEvent::MouseMove, 6, 4, Qt::LeftButton, stylus());
        mouse(v, QEvent::MouseButtonRelease, 6, 4, Qt::LeftButton, stylus());
        CHECK(w.document()->undoStack()->count() == 0);
        CHECK(tablet(v, QEvent::TabletMove, 3, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 3, 4, 0.0));
        CHECK(changed(w) == two);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a drag Qt's mouse events from the stylus start is theirs, once") {
        // The tool took the mouse event Qt made of a stylus press: the
        // stylus's own events stay unaccepted, so Qt goes on making mouse
        // events of them, and those drive the drag.
        mouse(v, QEvent::MouseButtonPress, 2, 4, Qt::LeftButton, stylus());
        CHECK_FALSE(tablet(v, QEvent::TabletMove, 3, 4, 1.0));
        mouse(v, QEvent::MouseMove, 3, 4, Qt::LeftButton, stylus());
        // the real mouse is another device
        mouse(v, QEvent::MouseButtonPress, 20, 4);
        mouse(v, QEvent::MouseMove, 21, 4);
        mouse(v, QEvent::MouseButtonRelease, 21, 4);
        CHECK_FALSE(tablet(v, QEvent::TabletRelease, 3, 4, 0.0));
        CHECK(w.document()->undoStack()->count() == 0);
        mouse(v, QEvent::MouseButtonRelease, 3, 4, Qt::LeftButton, stylus());
        CHECK(changed(w) == two);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    // After any of them, either device starts the next drag.
    CHECK(tablet(v, QEvent::TabletPress, 10, 4, 1.0));
    CHECK(tablet(v, QEvent::TabletRelease, 10, 4, 0.0));
    drag(v, {{12, 4}});
    CHECK(changed(w).count({10, 2}));
    CHECK(changed(w).count({12, 2}));
    CHECK(w.document()->undoStack()->count() == 3);
}

TEST_CASE("a drag's owner lets go when the drag ends, whatever buttons are still held") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    SECTION("the stylus lifted with its barrel button held") {
        CHECK(tabletHeld(v, QEvent::TabletPress, 2, 4, 1.0, Qt::LeftButton, Qt::LeftButton | Qt::RightButton));
        CHECK(tabletHeld(v, QEvent::TabletRelease, 2, 4, 0.0, Qt::LeftButton, Qt::RightButton));
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 1);
        drag(v, {{10, 4}});
        CHECK(changed(w).count({10, 2}));
    }
    SECTION("the mouse's left button let go with the right held") {
        mouseHeld(v, QEvent::MouseButtonPress, 2, 4, Qt::LeftButton, Qt::LeftButton);
        mouseHeld(v, QEvent::MouseButtonPress, 2, 4, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        mouseHeld(v, QEvent::MouseButtonRelease, 2, 4, Qt::LeftButton, Qt::RightButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 1);
        CHECK(tablet(v, QEvent::TabletPress, 10, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 10, 4, 0.0));
        CHECK(changed(w).count({10, 2}));
    }
    CHECK(changed(w).count({2, 2}));
    CHECK(w.document()->undoStack()->count() == 2);
}

TEST_CASE("a popup that opens during a mouse drag ends it; a stylus stroke goes on") {
    // Through Qt's own delivery: a real QMenu takes the mouse's events, and
    // once it has had one Qt sends the drag's release to the canvas no more.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    const auto sys = [&](QEvent::Type type, int x, int py, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, pixel(v, x, py), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                            held);
    };
    SECTION("a popup that takes the release") {
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        sys(QEvent::MouseMove, 3, 4, Qt::LeftButton);
        bwtest::popupOver(v, true);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
        // the stylus starts the next drag
        CHECK(tablet(v, QEvent::TabletPress, 20, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletMove, 21, 4, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 21, 4, 0.0));
        CHECK(changed(w) == std::set<std::pair<int, int>>{{20, 2}, {21, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
        // and a click paints its own cell, joined to nothing
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{20, 2}, {21, 2}, {30, 2}});
        CHECK(w.document()->undoStack()->count() == 2);
    }
    SECTION("a popup that closes with the button still held") {
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        bwtest::popupOver(v, false);
        CHECK_FALSE(dragTool(w)->dragging());
        sys(QEvent::MouseMove, 3, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 3, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{30, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a popup that takes a move and closes before the release") {
        // Codex round 5: the release reaches the window but not the canvas.
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        QMenu m(&w);
        m.addAction(QStringLiteral("x"));
        m.popup(w.mapToGlobal(QPoint(400, 300)));
        QApplication::processEvents();
        sys(QEvent::MouseMove, 3, 4, Qt::LeftButton);
        m.close();
        QApplication::processEvents();
        sys(QEvent::MouseButtonRelease, 3, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{30, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a popup a right press outside closes, before either release") {
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        QMenu m(&w);
        m.addAction(QStringLiteral("x"));
        m.popup(w.mapToGlobal(QPoint(400, 300)));
        QApplication::processEvents();
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 3, 4), Qt::RightButton,
                            Qt::LeftButton | Qt::RightButton);
        QApplication::processEvents();
        REQUIRE_FALSE(m.isVisible());
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 3, 4), Qt::RightButton, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 3, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{30, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a popup that takes the release outside it, dismissed by a click") {
        sys(QEvent::MouseButtonPress, 4, 4, Qt::LeftButton);
        sys(QEvent::MouseMove, 6, 4, Qt::LeftButton);
        QMenu m(&w);
        m.addAction(QStringLiteral("x"));
        m.popup(w.mapToGlobal(QPoint(400, 300)));
        QApplication::processEvents();
        sys(QEvent::MouseButtonRelease, 6, 4, Qt::NoButton);  // over the canvas: Qt routes it to the popup
        REQUIRE(m.isVisible());                                // QMenu stays open
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);  // closes the popup; not replayed to the canvas
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(tablet(v, QEvent::TabletPress, 20, 8, 1.0));
        CHECK(tablet(v, QEvent::TabletMove, 21, 8, 1.0));
        CHECK(tablet(v, QEvent::TabletRelease, 21, 8, 0.0));
        CHECK(w.document()->undoStack()->count() == 1);
        sys(QEvent::MouseButtonPress, 40, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 40, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{20, 4}, {21, 4}, {40, 2}});
    }
    SECTION("a popup that takes the release while another button is held") {
        // The right button still down doesn't make up for the left's release.
        sys(QEvent::MouseButtonPress, 2, 4, Qt::LeftButton);
        sys(QEvent::MouseMove, 3, 4, Qt::LeftButton);
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 3, 4), Qt::RightButton,
                            Qt::LeftButton | Qt::RightButton);
        bwtest::popupOver(v, true, Qt::LeftButton, Qt::RightButton);
        CHECK_FALSE(dragTool(w)->dragging());
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 3, 4), Qt::RightButton, Qt::NoButton);
        CHECK(changed(w).empty());
        sys(QEvent::MouseButtonPress, 30, 4, Qt::LeftButton);
        sys(QEvent::MouseButtonRelease, 30, 4, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{30, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a pen's mouse stroke a popup opens over") {
        // The pen's synthesized mouse events reach the popup as the mouse's
        // do: the drag ends at once, as a mouse drag does.
        const auto pen = [&](QEvent::Type type, int x, Qt::MouseButtons held) {
            bwtest::systemMouse(v, type, pixel(v, x, 4), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                held, bwtest::systemStylus());
        };
        pen(QEvent::MouseButtonPress, 2, Qt::LeftButton);
        pen(QEvent::MouseMove, 3, Qt::LeftButton);
        bwtest::popupOver(v, false);
        CHECK_FALSE(dragTool(w)->dragging());
        pen(QEvent::MouseMove, 4, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
    SECTION("a pen's mouse stroke ignores the mouse's click") {
        // The mouse's click is another device's: the pen's drag goes on, the
        // click paints nothing, and the pen's own release lands it (#45).
        const auto pen = [&](QEvent::Type type, int x, Qt::MouseButtons held) {
            bwtest::systemMouse(v, type, pixel(v, x, 4), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                                held, bwtest::systemStylus());
        };
        pen(QEvent::MouseButtonPress, 2, Qt::LeftButton);
        pen(QEvent::MouseMove, 3, Qt::LeftButton);
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 10, 4), Qt::RightButton, Qt::RightButton);
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 10, 4), Qt::RightButton, Qt::NoButton);
        CHECK(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
        pen(QEvent::MouseMove, 4, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 4, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
        CHECK(QWidget::mouseGrabber() == nullptr);
    }
    SECTION("a stylus stroke a popup opens and closes over") {
        // Qt sends the pen's events to the canvas, popup or not: the pen's
        // drag goes on and its own release ends it.
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
            m.close();
            QApplication::processEvents();
        }
        CHECK(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::NoButton, 0.0);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a stylus press right after a release under a popup") {
        // #46: the release goes through the popup, so Qt keeps the pen down
        // and sends the next press, with no hover between, as a move.
        // A left stroke paints cells 2 and 3; a right one rubs them out.
        const auto penLiftedUnderPopup = [&](Qt::MouseButton button = Qt::LeftButton, double pressure = 1.0) {
            const int strokes = w.document()->undoStack()->count();
            bwtest::systemTablet(v, pixel(v, 2, 4), button, pressure);
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), button, pressure);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            m.close();
            QApplication::processEvents();
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(w.document()->undoStack()->count() == strokes + 1);
            if (button == Qt::LeftButton)
                CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}});
            else
                CHECK(changed(w).empty());
        };
        SECTION("starts a stroke") {
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}});
            CHECK(w.document()->undoStack()->count() == 2);
        }
        // Qt's held record makes a press of another button a press or a
        // release of the one let go (the lowest bit that changed): the view's
        // own record says which button it is.
        const auto settled = [&](std::set<std::pair<int, int>> cells, int strokes) {
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == cells);
            CHECK(w.document()->undoStack()->count() == strokes);
            // nothing is left pending for a hover to go on with
            bwtest::systemTablet(v, pixel(v, 30, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == cells);
            CHECK(w.document()->undoStack()->count() == strokes);
        };
        SECTION("with the right button after a left stroke") {
            // Codex round 1: the right stroke rubs out the left one's cells
            // and ends on its own release.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 2, 4), Qt::RightButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            settled({}, 2);
        }
        SECTION("with the left button after a right stroke") {
            bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            penLiftedUnderPopup(Qt::RightButton);
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::NoButton, 0.0);
            settled({{10, 2}, {11, 2}}, 3);
        }
        SECTION("with the middle button, which no brush takes") {
            // Nor does the mouse event Qt makes of it come as the left
            // button's press.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::MiddleButton, 1.0);
            CHECK_FALSE(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::MiddleButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}}, 1);
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}, {40, 2}}, 2);
        }
        SECTION("pressed while another button is held") {
            // The right button's stroke: the tip pressed during it does
            // nothing, the right button's release ends it, and the tip held
            // after that paints nothing.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 2, 4), Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            CHECK(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}});
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w).empty());
            CHECK(w.document()->undoStack()->count() == 2);
            bwtest::systemTablet(v, pixel(v, 13, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 13, 4), Qt::NoButton, 0.0);
            settled({}, 2);
        }
        SECTION("two buttons pressed at once") {
            // The lower one is the stroke's; the other's release leaves it
            // going.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::LeftButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 12, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 12, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}, {10, 2}, {11, 2}, {12, 2}}, 2);
        }
        SECTION("a different button pressed straight after a release") {
            // No hover between the left stroke's release and the right press.
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::NoButton, 0.0);
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}});
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::RightButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}}, 3);
        }
        // That press set no tablet grab, so a lift off the canvas goes to
        // another widget; the stroke lands where the canvas last saw the pen
        // once the pen is back.
        const QPoint offInWindow = w.statusBar()->geometry().center();
        REQUIRE(w.childAt(offInWindow) != v);
        const QPointF off = v->mapFrom(&w, QPointF(offInWindow));
        const auto strokeLiftedOffCanvas = [&] {
            penLiftedUnderPopup();
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, off, Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, off, Qt::NoButton, 0.0);
        };
        SECTION("lifted off the canvas, its stroke lands when the pen hovers back") {
            strokeLiftedOffCanvas();
            bwtest::systemTablet(v, pixel(v, 30, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}});
            CHECK(w.document()->undoStack()->count() == 2);
            // and the next press is an ordinary one
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}, {40, 2}});
            CHECK(w.document()->undoStack()->count() == 3);
        }
        SECTION("lifted off the canvas, its stroke lands when the pen presses on it again") {
            // No hover over the canvas between: the press lands the old
            // stroke and starts its own, joined to nothing.
            strokeLiftedOffCanvas();
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::LeftButton, 1.0);
            CHECK(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}});
            CHECK(w.document()->undoStack()->count() == 2);
            bwtest::systemTablet(v, pixel(v, 40, 4), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}, {40, 2}});
            CHECK(w.document()->undoStack()->count() == 3);
        }
        SECTION("lifted off the canvas, its stroke lands when another button presses on it") {
            strokeLiftedOffCanvas();
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::RightButton, 1.0);
            CHECK(dragTool(w)->dragging());
            CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}});
            CHECK(w.document()->undoStack()->count() == 2);
            bwtest::systemTablet(v, pixel(v, 11, 4), Qt::NoButton, 0.0);
            settled({{2, 2}, {3, 2}, {10, 2}}, 3);
        }
        SECTION("a press Qt makes a release of reads its own pressure") {
            // Codex round 2: Qt still holds the right button, so it sends
            // the left press as a release; the press reads its pressure and
            // its release keeps it.
            bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            // 5 × 0.05 is 1 cell: the right stroke rubs out the left one
            v->setBrushSettings(BrushSettings{5, Tip::Square, 1.0, PressureTo::Size});
            penLiftedUnderPopup(Qt::RightButton, 0.05);
            bwtest::systemTablet(v, pixel(v, 20, 20), Qt::LeftButton, 1.0);
            CHECK(cellsOf(dragTool(w)->pending()) == tipAt(20, 10, 5, Tip::Square, w));
            bwtest::systemTablet(v, pixel(v, 20, 20), Qt::NoButton, 0.0);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(changed(w) == tipAt(20, 10, 5, Tip::Square, w));
            CHECK(w.document()->undoStack()->count() == 3);
        }
        SECTION("a click in keyboard mode with another button moves the cell cursor") {
            // Codex round 2: Qt sends the right press as the left button's;
            // keyboard mode doesn't take it, so it moves the cell cursor as
            // a click does.
            penLiftedUnderPopup();
            w.tools()->setActive(QStringLiteral("keyboard"));
            REQUIRE(v->cellCursor() != Cursor{30, 4});
            bwtest::systemTablet(v, pixel(v, 30, 8), Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 30, 8), Qt::NoButton, 0.0);
            CHECK(v->cellCursor() == Cursor{30, 4});
            CHECK(w.document()->undoStack()->count() == 1);
        }
        // Qt's record of the pen is up again for the tests after this one.
        bwtest::systemTablet(v, pixel(v, 50, 4), Qt::NoButton, 0.0);
    }
    SECTION("a right stroke lifted off the canvas ends where the view last saw it, not at the next press") {
        // Codex round 3: Qt shows only the left button changing on the left
        // press, so nothing says the right one went up there.
        use(w, "halfBlock");
        v->setPen(Pen{Color::indexed(Grey), Color::indexed(Red)});
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::RightButton, 1.0);
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::NoButton, 0.0);
            m.close();
            QApplication::processEvents();
        }
        REQUIRE(w.document()->undoStack()->count() == 1);
        const QPoint offInWindow = w.statusBar()->geometry().center();
        REQUIRE(w.childAt(offInWindow) != v);
        const QPointF off = v->mapFrom(&w, QPointF(offInWindow));
        bwtest::systemTablet(v, pixel(v, 10, 4), Qt::RightButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 11, 4), Qt::RightButton, 1.0);
        bwtest::systemTablet(v, off, Qt::RightButton, 1.0);
        bwtest::systemTablet(v, off, Qt::NoButton, 0.0);
        bwtest::systemTablet(v, pixel(v, 40, 4), Qt::LeftButton, 1.0);
        CHECK(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 2);
        bwtest::systemTablet(v, pixel(v, 40, 4), Qt::NoButton, 0.0);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {10, 2}, {11, 2}, {40, 2}});
        CHECK(w.document()->undoStack()->count() == 3);
    }
    SECTION("a stroke pressed under a popup after a release the view declined there stays whole") {
        // Codex round 3: the release the view declines comes again through
        // the window, and Qt records it; the press after it, which the view
        // takes through the popup, Qt doesn't, and sends as a press again.
        use(w, "halfBlock");
        v->setPen(Pen{Color::indexed(Grey), Color::indexed(Red)});
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(v, &esc);
        REQUIRE_FALSE(dragTool(w)->dragging());
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::MiddleButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 10, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
            CHECK(dragTool(w)->dragging());
            bwtest::systemTablet(v, pixel(v, 20, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 21, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
            CHECK(dragTool(w)->dragging());
            CHECK(w.document()->undoStack()->count() == 0);
            m.close();
            QApplication::processEvents();
        }
        bwtest::systemTablet(v, pixel(v, 22, 4), Qt::RightButton | Qt::MiddleButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 22, 4), Qt::MiddleButton, 1.0);
        CHECK_FALSE(dragTool(w)->dragging());
        std::set<std::pair<int, int>> row;
        for (int x = 10; x <= 22; ++x)
            row.insert({x, 2});
        CHECK(changed(w) == row);
        CHECK(w.document()->undoStack()->count() == 1);
        bwtest::systemTablet(v, pixel(v, 22, 4), Qt::NoButton, 0.0);
        CHECK(changed(w) == row);
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a button the tool declines pressed under a popup leaves its drag going") {
        // The select tool declines the right press through the popup; Qt
        // passes it again through the window and the view, which has acted on
        // it, sees it again: a release of a button its drag doesn't use and
        // the same press, both declined again.
        use(w, "select");
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 5, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            bwtest::systemTablet(v, pixel(v, 8, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            m.close();
            QApplication::processEvents();
        }
        bwtest::systemTablet(v, pixel(v, 9, 6), Qt::LeftButton | Qt::RightButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 9, 6), Qt::RightButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 9, 6), Qt::NoButton, 0.0);
        CHECK(v->selection().bounds() == CellRect::fromCorners(2, 2, 9, 3));
        CHECK(changed(w).empty());
    }
    SECTION("a stylus button pressed under a popup leaves the stroke whole") {
        // Codex round 2: the canvas takes the right press through the popup,
        // so Qt doesn't record it and sends the next event as that press
        // again; the left stroke goes on to its own release.
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        {
            QMenu m(v);
            m.addAction(QStringLiteral("x"));
            m.popup(v->mapToGlobal(QPoint(10, 10)));
            QApplication::processEvents();
            bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton | Qt::RightButton, 1.0);
            m.close();
            QApplication::processEvents();
        }
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton | Qt::RightButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 5, 4), Qt::LeftButton | Qt::RightButton, 1.0);
        CHECK(dragTool(w)->dragging());
        CHECK(w.document()->undoStack()->count() == 0);
        bwtest::systemTablet(v, pixel(v, 5, 4), Qt::RightButton, 1.0);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}, {5, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
        bwtest::systemTablet(v, pixel(v, 5, 4), Qt::NoButton, 0.0);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}, {5, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a stylus stroke Esc drops doesn't start again while the pen stays down") {
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(v, &esc);
        CHECK_FALSE(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton, 1.0);
        CHECK_FALSE(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::NoButton, 0.0);
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
}

TEST_CASE("a menu opened from the keyboard ends a mouse drag; a stylus stroke goes on (#47)") {
    // Alt+F takes the focus from the canvas twice, for the popup and then for
    // the menu bar; a click on the menu bar takes it only for the popup.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    QMenu* file = w.menuBar()->actions().at(0)->menu();
    REQUIRE(file);
    REQUIRE(file->title() == QStringLiteral("&File"));
    const auto fileMenuByKeyboard = [&] {
        bwtest::systemKey(v, Qt::Key_F, Qt::AltModifier, QStringLiteral("f"));
        CHECK(file->isVisible());
        CHECK_FALSE(v->hasFocus());
        bwtest::systemKey(file, Qt::Key_Escape);
        CHECK_FALSE(file->isVisible());
        bwtest::systemKey(w.menuBar(), Qt::Key_Escape);
    };

    SECTION("a stylus stroke") {
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        CHECK(dragTool(w)->dragging());
        fileMenuByKeyboard();
        CHECK(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 3, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::NoButton, 0.0);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a stylus stroke the menu bar alone takes the focus from") {
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        QFocusEvent bar(QEvent::FocusOut, Qt::MenuBarFocusReason);
        QApplication::sendEvent(v, &bar);
        CHECK(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::LeftButton, 1.0);
        bwtest::systemTablet(v, pixel(v, 4, 4), Qt::NoButton, 0.0);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}});
        CHECK(w.document()->undoStack()->count() == 1);
    }
    SECTION("a mouse drag") {
        const auto mouseAt = [&](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons held) {
            bwtest::systemMouse(v, type, pixel(v, x, 4), button, held);
        };
        mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
        mouseAt(QEvent::MouseMove, 3, Qt::NoButton, Qt::LeftButton);
        CHECK(dragTool(w)->dragging());
        fileMenuByKeyboard();
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(QWidget::mouseGrabber() == nullptr);
        mouseAt(QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
        mouseAt(QEvent::MouseButtonRelease, 4, Qt::LeftButton, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
    SECTION("a pen's mouse stroke") {
        const auto pen = [&](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons held) {
            bwtest::systemMouse(v, type, pixel(v, x, 4), button, held, bwtest::systemStylus());
        };
        pen(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
        CHECK(dragTool(w)->dragging());
        fileMenuByKeyboard();
        CHECK_FALSE(dragTool(w)->dragging());
        pen(QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 4, Qt::LeftButton, Qt::NoButton);
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
}

TEST_CASE("a window switch ends a stylus stroke a menu opened from the keyboard kept (#47)") {
    // The menu bar has the focus, not the canvas, so the canvas sees no
    // focus-out when the window goes inactive.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    QMenu* file = w.menuBar()->actions().at(0)->menu();
    const auto switchAway = [&](QWidget& other) {
        other.resize(100, 100);
        REQUIRE(bwtest::showExposed(&other));
        QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::SynchronousDelivery>(
            other.windowHandle(), Qt::ActiveWindowFocusReason);
        QApplication::processEvents();
        REQUIRE(QApplication::activeWindow() == &other);
    };
    bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
    bwtest::systemKey(v, Qt::Key_F, Qt::AltModifier, QStringLiteral("f"));
    REQUIRE(file->isVisible());
    SECTION("popup still open") {
        QWidget other;
        switchAway(other);
        CHECK_FALSE(dragTool(w)->dragging());
    }
    SECTION("popup closed, menu bar holds the focus") {
        bwtest::systemKey(file, Qt::Key_Escape);
        REQUIRE(QApplication::focusWidget() == w.menuBar());
        {
            QWidget other;
            switchAway(other);
            CHECK_FALSE(dragTool(w)->dragging());
        }
        QWindowSystemInterface::handleFocusWindowChanged<QWindowSystemInterface::SynchronousDelivery>(
            w.windowHandle(), Qt::ActiveWindowFocusReason);
        QApplication::processEvents();
        bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 10, 10), Qt::LeftButton, Qt::LeftButton);
        bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 10, 10), Qt::LeftButton, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{10, 5}});
        bwtest::systemTablet(v, pixel(v, 6, 4), Qt::NoButton, 0.0);  // the pen hovers back
        CHECK(changed(w) == std::set<std::pair<int, int>>{{10, 5}});
    }
    SECTION("popup closed, the focus moves on in the window") {
        bwtest::systemKey(file, Qt::Key_Escape);
        REQUIRE(QApplication::focusWidget() == w.menuBar());
        QWidget* other = nullptr;
        for (QWidget* c : w.findChildren<QWidget*>())
            if (c != v && c != w.menuBar() && c->isVisible() && c->isEnabled() && (c->focusPolicy() & Qt::ClickFocus)) {
                other = c;
                break;
            }
        REQUIRE(other);
        other->setFocus(Qt::MouseFocusReason);
        REQUIRE(QApplication::focusWidget() == other);
        CHECK_FALSE(dragTool(w)->dragging());
        bwtest::systemTablet(v, pixel(v, 6, 4), Qt::NoButton, 0.0);  // the pen hovers back
        CHECK(changed(w).empty());
        CHECK(w.document()->undoStack()->count() == 0);
    }
}

TEST_CASE("the undo key during a stylus stroke undoes nothing while the menu bar has the focus (#47)") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    QMenu* file = w.menuBar()->actions().at(0)->menu();
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, 10, 10), Qt::LeftButton, Qt::LeftButton);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, 10, 10), Qt::LeftButton, Qt::NoButton);
    REQUIRE(w.document()->undoStack()->index() == 1);
    bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
    bwtest::systemKey(v, Qt::Key_F, Qt::AltModifier, QStringLiteral("f"));
    bwtest::systemKey(file, Qt::Key_Escape);
    REQUIRE(QApplication::focusWidget() == w.menuBar());
    REQUIRE(dragTool(w)->dragging());
    bwtest::systemKey(w.menuBar(), Qt::Key_Z, Qt::ControlModifier);
    CHECK_FALSE(dragTool(w)->dragging());
    CHECK(w.document()->undoStack()->index() == 1);
    CHECK(changed(w) == std::set<std::pair<int, int>>{{10, 5}});
}

TEST_CASE("another device's click mid-drag leaves the owner's drag to finish (#45)") {
    // Through Qt's own delivery. A release from another device ends Qt's
    // record of which widget the owner pressed, so without a grab Qt drops
    // the owner's later moves and its release.
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    const auto mouseAt = [&](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, pixel(v, x, 4), button, held);
    };
    const auto pen = [&](QEvent::Type type, int x, Qt::MouseButton button, Qt::MouseButtons held) {
        bwtest::systemMouse(v, type, pixel(v, x, 4), button, held, bwtest::systemStylus());
    };
    const std::set<std::pair<int, int>> stroke = {{2, 2}, {3, 2}, {4, 2}};

    SECTION("a pen's click during a mouse drag") {
        mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
        mouseAt(QEvent::MouseMove, 3, Qt::NoButton, Qt::LeftButton);
        CHECK(QWidget::mouseGrabber() == v);
        pen(QEvent::MouseButtonPress, 10, Qt::LeftButton, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 10, Qt::LeftButton, Qt::NoButton);
        CHECK(dragTool(w)->dragging());
        CHECK(changed(w).empty());
        mouseAt(QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
        mouseAt(QEvent::MouseButtonRelease, 4, Qt::LeftButton, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == stroke);
        CHECK(w.document()->undoStack()->count() == 1);
        CHECK(QWidget::mouseGrabber() == nullptr);
        // the next click is a stroke of its own, joined to nothing
        mouseAt(QEvent::MouseButtonPress, 30, Qt::LeftButton, Qt::LeftButton);
        mouseAt(QEvent::MouseButtonRelease, 30, Qt::LeftButton, Qt::NoButton);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}, {4, 2}, {30, 2}});
        CHECK(w.document()->undoStack()->count() == 2);
    }
    SECTION("the mouse's click on another widget during a pen's mouse stroke") {
        // The click is the canvas's to ignore: it doesn't reach the widget
        // under it, which would otherwise keep the pen's moves too.
        QStatusBar* bar = w.statusBar();
        REQUIRE(bar->isVisible());
        int barPresses = 0;
        struct Counter : QObject {
            int* n;
            explicit Counter(int* n) : n(n) {}
            bool eventFilter(QObject*, QEvent* e) override {
                if (e->type() == QEvent::MouseButtonPress)
                    ++*n;
                return false;
            }
        } counter(&barPresses);
        bar->installEventFilter(&counter);
        pen(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
        pen(QEvent::MouseMove, 3, Qt::NoButton, Qt::LeftButton);
        bwtest::systemMouse(bar, QEvent::MouseButtonPress, QPointF(5, 5), Qt::LeftButton, Qt::LeftButton);
        bwtest::systemMouse(bar, QEvent::MouseButtonRelease, QPointF(5, 5), Qt::LeftButton, Qt::NoButton);
        CHECK(barPresses == 0);
        CHECK(dragTool(w)->dragging());
        pen(QEvent::MouseMove, 4, Qt::NoButton, Qt::LeftButton);
        pen(QEvent::MouseButtonRelease, 4, Qt::LeftButton, Qt::NoButton);
        CHECK_FALSE(dragTool(w)->dragging());
        CHECK(changed(w) == stroke);
        CHECK(w.document()->undoStack()->count() == 1);
        CHECK(QWidget::mouseGrabber() == nullptr);
        // with the drag over, a click reaches the widget under it again
        bwtest::systemMouse(bar, QEvent::MouseButtonPress, QPointF(5, 5), Qt::LeftButton, Qt::LeftButton);
        bwtest::systemMouse(bar, QEvent::MouseButtonRelease, QPointF(5, 5), Qt::LeftButton, Qt::NoButton);
        CHECK(barPresses == 1);
        bar->removeEventFilter(&counter);
    }
    SECTION("the grab ends with the drag, however it ends") {
        SECTION("Esc, the button still held") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE(QWidget::mouseGrabber() == v);
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(v, &esc);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(QWidget::mouseGrabber() == nullptr);
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("the undo key") {
            pen(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE(QWidget::mouseGrabber() == v);
            QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
            QApplication::sendEvent(v, &undo);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(QWidget::mouseGrabber() == nullptr);
            pen(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("a popup") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE(QWidget::mouseGrabber() == v);
            bwtest::popupOver(v, true);
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(QWidget::mouseGrabber() == nullptr);
        }
        SECTION("a tool switch") {
            pen(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE(QWidget::mouseGrabber() == v);
            use(w, "line");
            CHECK(QWidget::mouseGrabber() == nullptr);
            pen(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("a new canvas") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE(QWidget::mouseGrabber() == v);
            w.document()->clear();
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(QWidget::mouseGrabber() == nullptr);
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("Select All, which starts the selection tool") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE(QWidget::mouseGrabber() == v);
            act(w, QStringLiteral("selectAll"))->trigger();
            CHECK(QWidget::mouseGrabber() == nullptr);
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("Deselect leaves the brush's drag going, and the grab") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            act(w, QStringLiteral("deselect"))->trigger();
            CHECK(dragTool(w)->dragging());
            CHECK(QWidget::mouseGrabber() == v);
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        SECTION("the focus going elsewhere") {
            mouseAt(QEvent::MouseButtonPress, 2, Qt::LeftButton, Qt::LeftButton);
            REQUIRE(QWidget::mouseGrabber() == v);
            v->clearFocus();
            CHECK_FALSE(dragTool(w)->dragging());
            CHECK(QWidget::mouseGrabber() == nullptr);
            mouseAt(QEvent::MouseButtonRelease, 2, Qt::LeftButton, Qt::NoButton);
        }
        CHECK(w.document()->undoStack()->count() <= 1);
    }
    SECTION("a stylus stroke driven by tablet events takes no grab") {
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::LeftButton, 1.0);
        CHECK(dragTool(w)->dragging());
        CHECK(QWidget::mouseGrabber() == nullptr);
        bwtest::systemTablet(v, pixel(v, 2, 4), Qt::NoButton, 0.0);
        CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}});
    }
    CHECK(QWidget::mouseGrabber() == nullptr);
}

TEST_CASE("a canvas that isn't shown takes no mouse grab") {
    // QWidget::grabMouse() wants a visible widget. The drag works all the
    // same.
    MainWindow w;
    CanvasView* v = w.canvasView();
    REQUIRE_FALSE(v->isVisible());
    use(w, "glyph");
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    mouse(v, QEvent::MouseButtonPress, 2, 4);
    CHECK(dragTool(w)->dragging());
    CHECK(QWidget::mouseGrabber() == nullptr);
    mouse(v, QEvent::MouseMove, 3, 4);
    mouse(v, QEvent::MouseButtonRelease, 3, 4);
    CHECK_FALSE(dragTool(w)->dragging());
    CHECK(changed(w) == std::set<std::pair<int, int>>{{2, 2}, {3, 2}});
    CHECK(QWidget::mouseGrabber() == nullptr);
}

TEST_CASE("mirror mode with Pressure->Strength: a cell both strokes reach takes the stronger dab") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    use(w, "rampShade");
    act(w, QStringLiteral("mirrorMode"))->trigger();
    REQUIRE(w.document()->canvas().width() == 80);
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Strength});
    const Ramp steel = ramp("steel");
    // columns 39 and 40 mirror each other: the stroke covers 39 lightly and
    // 40 fully, its mirror 40 lightly and 39 fully
    tablet(v, QEvent::TabletPress, 39, 0, 0.25);
    tablet(v, QEvent::TabletMove, 40, 0, 1.0);
    tablet(v, QEvent::TabletRelease, 40, 0, 0.0);
    CHECK(rungOf(steel, at(w, 39, 0)) == 4);
    CHECK(rungOf(steel, at(w, 40, 0)) == 4);
    CHECK(w.document()->undoStack()->count() == 1);
}

TEST_CASE("mirror mode: random stylus strokes leave a mirror-symmetric canvas") {
    for (const char* id : {"rampShade", "shade"}) {
        MainWindow w;
        CanvasView* v = w.canvasView();
        use(w, id);
        act(w, QStringLiteral("mirrorMode"))->trigger();
        REQUIRE(v->mirror());
        std::mt19937 rng(20261022);
        const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        const auto pressure = [&] { return std::uniform_real_distribution<double>(0.02, 1.0)(rng); };
        for (int stroke = 0; stroke < 60; ++stroke) {
            v->setBrushSettings(BrushSettings{pick(1, 5), pick(0, 1) ? Tip::Round : Tip::Square, pick(10, 100) / 100.0,
                                              pick(0, 1) ? PressureTo::Strength : PressureTo::Size});
            const Qt::MouseButton button = pick(0, 3) ? Qt::LeftButton : Qt::RightButton;
            int x = pick(30, 49), py = pick(0, 19);
            tablet(v, QEvent::TabletPress, x, py, pressure(), button);
            for (int n = pick(0, 4); n > 0; --n) {
                x = std::clamp(x + pick(-6, 6), 0, 79);
                py = std::clamp(py + pick(-4, 4), 0, 39);
                tablet(v, QEvent::TabletMove, x, py, pressure(), button);
            }
            tablet(v, QEvent::TabletRelease, x, py, 0.0, button);
            INFO(id << " stroke " << stroke);
            REQUIRE(asymmetric(w).empty());
        }
    }
}

TEST_CASE("a brush keeps the pen it was pressed with for the whole stroke") {
    MainWindow w;
    CanvasView* v = w.canvasView();
    for (const char* id : {"shade", "glyph", "colorize"}) {
        INFO(id);
        use(w, id);
        v->setForeground(LightGreen);
        mouse(v, QEvent::MouseButtonPress, 0, 0);
        mouse(v, QEvent::MouseMove, 2, 0);
        v->setForeground(Yellow);
        // a brush change redoes the stroke, in the pen it started with
        BrushSettings s = v->brushSettings();
        s.tip = s.tip == Tip::Square ? Tip::Round : Tip::Square;
        v->setBrushSettings(s);
        act(w, QStringLiteral("mirrorMode"))->trigger();
        act(w, QStringLiteral("mirrorMode"))->trigger();
        mouse(v, QEvent::MouseMove, 3, 0);
        mouse(v, QEvent::MouseButtonRelease, 3, 0);
        for (int x = 0; x <= 3; ++x)
            CHECK(at(w, x, 0).fg.paletteIndex() == LightGreen);
        // the next stroke takes the new pen
        drag(v, {{0, 2}});
        CHECK(at(w, 0, 1).fg.paletteIndex() == Yellow);
    }
}
