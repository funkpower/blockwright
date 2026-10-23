// The detail tools in the main window (PLAN §10, "Detail"): the rim-light
// brush, the pencil's 8-connected mode and the symmetry modes, from the
// Tools and Edit menus and the Brush dock, with the mouse delivered as the
// platform delivers it.

#include "brush_dock.h"
#include "canvas_view.h"
#include "document.h"
#include "draw_tools.h"
#include "main_window.h"
#include "tool.h"

#include "system_input.h"

#include "blockwright/brush.h"
#include "blockwright/halfblock.h"
#include "blockwright/palette.h"
#include "blockwright/ramp.h"
#include "blockwright/shapes.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QUndoStack>

#include <map>
#include <set>
#include <utility>

using namespace bw;

namespace {

using Point = std::pair<int, int>;

QAction* act(MainWindow& w, const QString& name) {
    auto* a = w.findChild<QAction*>(name);
    REQUIRE(a);
    return a;
}

void use(MainWindow& w, const char* id) {
    act(w, QString::fromLatin1(id) + QStringLiteral("Tool"))->trigger();
    REQUIRE(w.tools()->active()->id() == QString::fromLatin1(id));
}

BrushDock* dock(MainWindow& w) {
    auto* d = w.findChild<BrushDock*>();
    REQUIRE(d);
    return d;
}

const Cell& at(MainWindow& w, int x, int y) { return w.document()->canvas().at(x, y); }

DragTool* dragTool(MainWindow& w) {
    auto* t = qobject_cast<DragTool*>(w.tools()->active());
    REQUIRE(t);
    return t;
}

// The view pixel in the middle of half-block pixel (x, py).
QPointF pixel(CanvasView* v, int x, int py) {
    const QRect r = v->cellRect(QPoint(x, py >> 1));
    return QPointF(r.center().x(), r.top() + r.height() * ((py & 1) ? 0.75 : 0.25));
}

// A left-button drag through half-block pixels, through the window system.
// Returns the edit the preview showed just before the release.
CellEdit drag(MainWindow& w, std::initializer_list<QPoint> points) {
    CanvasView* v = w.canvasView();
    const QPoint* p = points.begin();
    bwtest::systemMouse(v, QEvent::MouseButtonPress, pixel(v, p->x(), p->y()), Qt::LeftButton, Qt::LeftButton);
    for (++p; p != points.end(); ++p)
        bwtest::systemMouse(v, QEvent::MouseMove, pixel(v, p->x(), p->y()), Qt::NoButton, Qt::LeftButton);
    const CellEdit shown = dragTool(w)->pending();
    const QPoint last = *(points.end() - 1);
    bwtest::systemMouse(v, QEvent::MouseButtonRelease, pixel(v, last.x(), last.y()), Qt::LeftButton, Qt::NoButton);
    return shown;
}

// The cells an edit wrote, and what it wrote there.
std::map<Point, Cell> written(const CellEdit& e) {
    std::map<Point, Cell> out;
    for (const CellChange& c : e.changes)
        out[{c.x, c.y}] = c.after;
    return out;
}

// Checks the canvas holds what the preview showed, as one undo step.
void landed(MainWindow& w, const CellEdit& shown, int stepsBefore) {
    REQUIRE_FALSE(shown.empty());
    for (const CellChange& c : shown.changes)
        CHECK(at(w, c.x, c.y) == c.after);
    CHECK(w.document()->undoStack()->index() == stepsBefore + 1);
}

Ramp steel() { return *findRamp("steel"); }  // k K w W

// A steel shape at rows 1-4, columns 2-6, darker on the left: rungs 8 to 12
// (w to W), as one undo step.
void paintBlob(MainWindow& w) {
    CellEdit e;
    for (int y = 1; y <= 4; ++y)
        for (int x = 2; x <= 6; ++x)
            e.changes.push_back({x, y, at(w, x, y), rungCell(steel(), 8 + (x - 2))});
    REQUIRE(w.canvasView()->applyEdit(e));
}

std::set<Point> cells(const CellEdit& e) {
    std::set<Point> out;
    for (const CellChange& c : e.changes)
        out.insert({c.x, c.y});
    return out;
}

} // namespace

TEST_CASE("the rim-light brush lights the edges facing the dock's light by one rung") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    v->setRamp(steel());
    v->setBrushSettings(BrushSettings{12, Tip::Square, 1.0, PressureTo::Nothing});
    paintBlob(w);
    QComboBox* light = dock(w)->lightBox();
    REQUIRE(light->count() == 8);
    CHECK(light->currentData().toInt() == int(Compass::NW));
    light->setCurrentIndex(light->findData(int(Compass::N)));
    CHECK(v->brushSettings().lightDirection == Compass::N);
    // the other settings stay as they were
    CHECK(v->brushSettings().size == 12);
    const int steps = w.document()->undoStack()->index();
    use(w, "rimLight");
    const CellEdit shown = drag(w, {{4, 4}});
    landed(w, shown, steps);
    std::map<Point, int> lit;
    for (const auto& [p, cell] : written(shown)) {
        REQUIRE(rungOf(steel(), cell));
        lit[p] = *rungOf(steel(), cell) - (8 + (p.first - 2));
    }
    // the top row but its last cell, which is W already
    CHECK(lit == std::map<Point, int>{{{2, 1}, 1}, {{3, 1}, 1}, {{4, 1}, 1}, {{5, 1}, 1}});

    // from the west: the left column, and the cells beside its ends, whose
    // normals on screen turn about 55 degrees from the west (as the core's)
    light->setCurrentIndex(light->findData(int(Compass::W)));
    w.document()->undoStack()->undo();
    const CellEdit west = drag(w, {{4, 4}});
    CHECK(cells(west) == std::set<Point>{{2, 1}, {2, 2}, {2, 3}, {2, 4}, {3, 1}, {3, 4}});
}

TEST_CASE("the pencil is 4-connected by default and 8-connected from the dock") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    QCheckBox* four = dock(w)->pencil4Box();
    CHECK(four->isChecked());
    CHECK(v->pencilConnectivity() == Connectivity::Four);
    use(w, "halfBlock");
    v->setForeground(Red);
    const auto lit = [&] {
        std::set<Point> px;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 6; ++x) {
                const HalfPair h = halfBlocks(at(w, x, y)).value_or(HalfPair{});
                if (h.top == Red)
                    px.insert({x, 2 * y});
                if (h.bottom == Red)
                    px.insert({x, 2 * y + 1});
            }
        return px;
    };
    // one move from one corner to the other: the line joins them
    drag(w, {{0, 0}, {3, 3}});
    const std::set<Point> stepped = lit();
    CHECK(stepped.size() == 7);
    CHECK(stepped.count({0, 0}));
    CHECK(stepped.count({3, 3}));

    w.document()->undoStack()->undo();
    four->setChecked(false);
    CHECK(v->pencilConnectivity() == Connectivity::Eight);
    const int steps = w.document()->undoStack()->index();
    const CellEdit shown = drag(w, {{0, 0}, {3, 3}});
    landed(w, shown, steps);
    CHECK(lit() == std::set<Point>{{0, 0}, {1, 1}, {2, 2}, {3, 3}});
}

TEST_CASE("each symmetry mode from the Edit menu repeats a brush stroke as its exact image") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    const int W = w.document()->canvas().width(), H = w.document()->canvas().height();
    v->setBrushSettings(BrushSettings{1, Tip::Square, 1.0, PressureTo::Nothing});
    use(w, "glyph");
    auto* glyph = qobject_cast<GlyphTool*>(w.tools()->active());
    // an L of cells, (3,2), (4,2), (4,3), in `ch`
    const auto stroke = [&](char32_t ch) {
        glyph->setGlyph(ch);
        const int steps = w.document()->undoStack()->index();
        const CellEdit shown = drag(w, {{3, 4}, {4, 4}, {4, 6}});
        landed(w, shown, steps);
        std::map<Point, char32_t> out;
        for (const auto& [p, cell] : written(shown))
            out[p] = cell.ch;
        w.document()->undoStack()->undo();
        return out;
    };
    // the L in `ch` at its own cells and `images` of it
    const auto L = [](char32_t ch, std::map<Point, char32_t> images) {
        images.insert({{{3, 2}, ch}, {{4, 2}, ch}, {{4, 3}, ch}});
        return images;
    };
    const auto mirrorX = [&](char32_t ch) {
        return std::map<Point, char32_t>{{{W - 4, 2}, ch}, {{W - 5, 2}, ch}, {{W - 5, 3}, ch}};
    };
    const auto mirrorY = [&](char32_t ch) {
        return std::map<Point, char32_t>{{{3, H - 3}, ch}, {{4, H - 3}, ch}, {{4, H - 4}, ch}};
    };

    SECTION("mirror X") {
        act(w, QStringLiteral("symmetryMirrorX"))->trigger();
        CHECK(act(w, QStringLiteral("mirrorMode"))->isChecked());
        CHECK(stroke(U'▌') == L(U'▌', mirrorX(U'▐')));
    }
    SECTION("mirror Y") {
        act(w, QStringLiteral("symmetryMirrorY"))->trigger();
        CHECK(act(w, QStringLiteral("mirrorMode"))->isChecked());
        CHECK(act(w, QStringLiteral("symmetryMirrorY"))->isChecked());
        CHECK(dock(w)->symmetryBox()->currentData().toInt() == int(SymmetryMode::MirrorY));
        CHECK(stroke(U'▀') == L(U'▀', mirrorY(U'▄')));
    }
    SECTION("mirror X and Y") {
        act(w, QStringLiteral("symmetryMirrorXY"))->trigger();
        std::map<Point, char32_t> all = L(U'▌', mirrorX(U'▐'));
        all.merge(mirrorY(U'▌'));
        all.merge(std::map<Point, char32_t>{{{W - 4, H - 3}, U'▐'}, {{W - 5, H - 3}, U'▐'}, {{W - 5, H - 4}, U'▐'}});
        CHECK(stroke(U'▌') == all);
    }
    SECTION("radial, two folds about a set cell") {
        BrushDock* d = dock(w);
        d->centreXBox()->setValue(10);
        d->centreYBox()->setValue(10);
        d->symmetryBox()->setCurrentIndex(d->symmetryBox()->findData(int(SymmetryMode::Radial)));
        CHECK(act(w, QStringLiteral("symmetryRadial"))->isChecked());
        CHECK(d->foldsBox()->isEnabled());
        d->foldsBox()->setValue(2);
        d->centreBox()->setChecked(true);
        REQUIRE(v->symmetry() == Symmetry{SymmetryMode::Radial, 2, Point{10, 10}});
        // the half turn is exact, its glyph flipped both ways
        CHECK(stroke(U'▀') == L(U'▀', {{{17, 18}, U'▄'}, {{16, 18}, U'▄'}, {{16, 17}, U'▄'}}));
    }
    SECTION("radial, four folds: quarter turns on screen, glyphs turned with them") {
        BrushDock* d = dock(w);
        d->symmetryBox()->setCurrentIndex(d->symmetryBox()->findData(int(SymmetryMode::Radial)));
        d->foldsBox()->setValue(4);
        d->centreXBox()->setValue(6);
        d->centreYBox()->setValue(3);
        d->centreBox()->setChecked(true);
        REQUIRE(v->symmetry() == Symmetry{SymmetryMode::Radial, 4, Point{6, 3}});
        glyph->setGlyph(U'▀');
        const int steps = w.document()->undoStack()->index();
        const CellEdit shown = drag(w, {{8, 6}});
        landed(w, shown, steps);
        std::map<Point, char32_t> out;
        for (const auto& [p, cell] : written(shown))
            out[p] = cell.ch;
        // two cells across is one cell down: cells are twice as tall as
        // wide; a quarter turn turns the glyph as Rotate does (▀ to ▐)
        CHECK(out == std::map<Point, char32_t>{{{8, 3}, U'▀'}, {{6, 4}, U'▐'}, {{4, 3}, U'▄'}, {{6, 2}, U'▌'}});
    }
    SECTION("off from the dock: the stroke alone, and mirror mode off") {
        act(w, QStringLiteral("symmetryMirrorY"))->trigger();
        dock(w)->symmetryBox()->setCurrentIndex(0);
        CHECK_FALSE(v->mirror());
        CHECK_FALSE(act(w, QStringLiteral("mirrorMode"))->isChecked());
        CHECK(stroke(U'▀') == L(U'▀', {}));
        // mirror mode back on keeps the chosen mode
        act(w, QStringLiteral("mirrorMode"))->trigger();
        CHECK(v->symmetry().mode == SymmetryMode::MirrorY);
        CHECK(dock(w)->symmetryBox()->currentData().toInt() == int(SymmetryMode::MirrorY));
        CHECK(stroke(U'▀') == L(U'▀', mirrorY(U'▄')));
    }
}

TEST_CASE("the half-block pencil takes every symmetry mode") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    CanvasView* v = w.canvasView();
    const int W = w.document()->canvas().width(), H = w.document()->canvas().height();
    use(w, "halfBlock");
    v->setForeground(Red);
    const auto pixels = [&](SymmetryMode mode) {
        act(w, mode == SymmetryMode::MirrorX    ? QStringLiteral("symmetryMirrorX")
               : mode == SymmetryMode::MirrorY  ? QStringLiteral("symmetryMirrorY")
               : mode == SymmetryMode::MirrorXY ? QStringLiteral("symmetryMirrorXY")
                                                : QStringLiteral("symmetryRadial"))
            ->trigger();
        const int steps = w.document()->undoStack()->index();
        const CellEdit shown = drag(w, {{1, 0}, {2, 0}});
        landed(w, shown, steps);
        std::set<Point> px;
        for (const auto& [p, cell] : written(shown)) {
            const HalfPair h = halfBlocks(cell).value_or(HalfPair{});
            if (h.top == Red)
                px.insert({p.first, 2 * p.second});
            if (h.bottom == Red)
                px.insert({p.first, 2 * p.second + 1});
        }
        w.document()->undoStack()->undo();
        return px;
    };
    const int R = 2 * H - 1;  // the last pixel row
    CHECK(pixels(SymmetryMode::MirrorX) == std::set<Point>{{1, 0}, {2, 0}, {W - 2, 0}, {W - 3, 0}});
    CHECK(pixels(SymmetryMode::MirrorY) == std::set<Point>{{1, 0}, {2, 0}, {1, R}, {2, R}});
    CHECK(pixels(SymmetryMode::MirrorXY) == std::set<Point>{{1, 0}, {2, 0}, {W - 2, 0}, {W - 3, 0}, {1, R}, {2, R},
                                                            {W - 2, R}, {W - 3, R}});
    // radial, the canvas centre, two folds: the half turn
    dock(w)->foldsBox()->setValue(2);
    CHECK(pixels(SymmetryMode::Radial) == std::set<Point>{{1, 0}, {2, 0}, {W - 2, R}, {W - 3, R}});
}

TEST_CASE("a shape dragged past the canvas edge keeps the radial images that land on it") {
    MainWindow w;
    REQUIRE(bwtest::showExposed(&w));
    AnsiDocument doc;
    doc.canvas = Canvas(80, 25);
    w.document()->replace(std::move(doc), QString());
    CanvasView* v = w.canvasView();
    const Canvas blank = w.document()->canvas();
    v->setForeground(LightRed);
    BrushDock* d = dock(w);
    d->symmetryBox()->setCurrentIndex(d->symmetryBox()->findData(int(SymmetryMode::Radial)));
    d->foldsBox()->setValue(4);
    REQUIRE(v->symmetry() == Symmetry{SymmetryMode::Radial, 4});
    StrokeOptions o;
    o.symmetry = v->symmetry();
    // a line drawn entirely above the canvas: its quarter turns land on
    // the canvas's sides
    Pixels above;
    for (int x = 20; x <= 60; ++x)
        above.push_back({x, -5});
    const CellEdit want = plotPixels(blank, above, LightRed, o);
    REQUIRE_FALSE(want.empty());
    use(w, "line");
    Tool* t = w.tools()->active();
    const auto event = [](int x, int py, Qt::MouseButton b, Qt::MouseButtons held) {
        return ToolEvent{QPoint(x, py >> 1), py, b, held, Qt::NoModifier, 1.0};
    };
    REQUIRE(t->mousePress(event(20, -5, Qt::LeftButton, Qt::LeftButton)));
    t->mouseMove(event(60, -5, Qt::NoButton, Qt::LeftButton));
    CHECK(dragTool(w)->pending().changes == want.changes);
    t->mouseRelease(event(60, -5, Qt::LeftButton, Qt::NoButton));
    Canvas expect = blank;
    want.apply(expect);
    CHECK(w.document()->canvas() == expect);
}

TEST_CASE("the dock's radial centre stays on the canvas") {
    MainWindow w;
    BrushDock* d = dock(w);
    const Canvas& c = w.document()->canvas();
    CHECK(d->centreXBox()->maximum() == c.width() - 1);
    CHECK(d->centreYBox()->maximum() == c.height() - 1);
    d->symmetryBox()->setCurrentIndex(d->symmetryBox()->findData(int(SymmetryMode::Radial)));
    d->centreXBox()->setValue(30);
    d->centreYBox()->setValue(20);
    d->centreBox()->setChecked(true);
    REQUIRE(w.canvasView()->symmetry().centre == Point{30, 20});
    // a smaller canvas pulls the centre in to its edge
    AnsiDocument doc;
    doc.canvas = Canvas(12, 5);
    w.document()->replace(std::move(doc), QString());
    CHECK(d->centreXBox()->maximum() == 11);
    CHECK(d->centreYBox()->maximum() == 4);
    CHECK(w.canvasView()->symmetry().centre == Point{11, 4});
    // and a bigger one lets it out again
    AnsiDocument big;
    big.canvas = Canvas(200, 90);
    w.document()->replace(std::move(big), QString());
    CHECK(d->centreXBox()->maximum() == 199);
    CHECK(d->centreYBox()->maximum() == 89);
    d->centreXBox()->setValue(150);
    CHECK(w.canvasView()->symmetry().centre == Point{150, 4});
    // Codex round 2 #4: with symmetry off, a smaller canvas still pulls the
    // saved centre in, so turning symmetry back on turns about it
    d->centreXBox()->setValue(30);
    d->centreYBox()->setValue(20);
    REQUIRE(w.canvasView()->symmetry().centre == Point{30, 20});
    w.canvasView()->setMirror(false);
    AnsiDocument small;
    small.canvas = Canvas(12, 5);
    w.document()->replace(std::move(small), QString());
    CHECK(w.canvasView()->symmetryChoice().centre == Point{11, 4});
    w.canvasView()->setMirror(true);
    CHECK(w.canvasView()->symmetry().centre == Point{11, 4});
    CHECK(d->centreXBox()->value() == 11);
    CHECK(d->centreYBox()->value() == 4);
}
