// The ramp gradient (PLAN §10, "Shading"): linear and radial, cell-banded or
// Bayer-dithered, along a ramp's ladder, with no ladder-lint finding on what
// it fills.

#include "blockwright/ansi.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/brushes/guard.h"
#include "blockwright/lint.h"
#include "blockwright/palette.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// lint::indexesBuilt() exists only with the test hooks, which blockwright_core
// carries whenever the tests are built.
#ifndef BW_TEST_HOOKS
#error "test_gradient.cpp needs BW_TEST_HOOKS (set on blockwright_core when BUILD_TESTING is ON)"
#endif

using namespace bw;

namespace {

Ramp steel() { return *findRamp("steel"); }  // k K w W

Canvas filled(int w, int h, const Cell& c) {
    Canvas canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.at(x, y) = c;
    return canvas;
}

Canvas applied(Canvas canvas, const CellEdit& edit) {
    edit.apply(canvas);
    return canvas;
}

std::string describe(const lint::LadderReport& r) {
    std::ostringstream out;
    for (const auto& f : r.findings)
        out << f.message << "\n";
    return out.str();
}

} // namespace

TEST_CASE("gradient: a linear one across a row walks every rung, shades two cells wide", "[gradient]") {
    const Ramp ramp = steel();
    Gradient g;
    g.x0 = 0, g.x1 = 59, g.fromRung = 0, g.toRung = ramp.topRung();
    const std::vector<int> rungs = gradientRungs(ramp, g, 60, 4);
    REQUIRE(rungs.size() == 240);
    for (int y = 0; y < 4; ++y) {
        std::vector<int> row(rungs.begin() + y * 60, rungs.begin() + (y + 1) * 60);
        CHECK(row == std::vector<int>(rungs.begin(), rungs.begin() + 60));  // every row alike
    }
    std::vector<int> levels;  // the row as runs
    std::vector<int> widths;
    for (int x = 0; x < 60; ++x) {
        if (levels.empty() || levels.back() != rungs[size_t(x)]) {
            levels.push_back(rungs[size_t(x)]);
            widths.push_back(0);
        }
        ++widths.back();
    }
    std::vector<int> all;
    for (int r = 0; r <= ramp.topRung(); ++r)
        all.push_back(r);
    CHECK(levels == all);
    for (size_t i = 0; i < levels.size(); ++i) {
        INFO("rung " << levels[i]);
        if (levels[i] % kRungsPerStep)
            CHECK(widths[i] == 2);
        else
            CHECK(widths[i] > 2);  // a plateau
    }
}

TEST_CASE("gradient: radial is round on screen, a row counting as two columns", "[gradient]") {
    const Ramp ramp = steel();
    Gradient g;
    g.shape = GradientShape::Radial;
    g.x0 = 30, g.y0 = 15, g.x1 = 50, g.y1 = 15, g.fromRung = ramp.topRung(), g.toRung = 0;
    const std::vector<int> rungs = gradientRungs(ramp, g, 61, 31);
    REQUIRE(!rungs.empty());
    const auto at = [&](int x, int y) { return rungs[size_t(y) * 61 + x]; };
    CHECK(at(30, 15) == ramp.topRung());
    for (int k = 0; k <= 15; ++k) {
        INFO("k " << k);
        CHECK(at(30 + 2 * k, 15) == at(30, 15 + k));
        CHECK(at(30 - 2 * k, 15) == at(30, 15 - k));
    }
    CHECK(at(0, 0) == 0);  // past the ladder's end, a plane
}

TEST_CASE("gradient: a span too short for the ladder runs past the end and skips nothing", "[gradient]") {
    const Ramp ramp = steel();  // 13 rungs, 26 columns at the band
    Gradient g;
    g.x0 = 0, g.x1 = 5, g.fromRung = 0, g.toRung = ramp.topRung();
    const std::vector<int> rungs = gradientRungs(ramp, g, 40, 1);
    for (int x = 1; x < 40; ++x)
        CHECK(std::abs(rungs[size_t(x)] - rungs[size_t(x - 1)]) <= 1);
    CHECK(rungs.back() == ramp.topRung());
}

TEST_CASE("gradient: the same cell at both ends paints nothing", "[gradient]") {
    const Canvas canvas = filled(10, 5, Cell{});
    Gradient g;
    g.x0 = g.x1 = 3, g.y0 = g.y1 = 2, g.toRung = 12;
    CHECK(rampGradient(canvas, steel(), g).empty());
}

TEST_CASE("gradient: the selection and the alpha lock bound it", "[gradient]") {
    const Ramp ramp = steel();
    Canvas canvas = filled(30, 10, Cell{});
    canvas.at(12, 4).transparent = true;
    Gradient g;
    g.x0 = 0, g.x1 = 29, g.toRung = ramp.topRung();
    StrokeOptions options;
    options.selection = selectionMask(Selection(CellRect{10, 2, 8, 5}), 30, 10);
    options.lockTransparent = true;
    const CellEdit edit = rampGradient(canvas, ramp, g, options);
    REQUIRE(!edit.empty());
    for (const CellChange& c : edit.changes) {
        INFO(c.x << "," << c.y);
        CHECK(c.x >= 10);
        CHECK(c.x < 18);
        CHECK(c.y >= 2);
        CHECK(c.y < 7);
        CHECK(!(c.x == 12 && c.y == 4));
        CHECK(c.before != c.after);
    }
    // Cells already at their rung are left out: a second pass changes nothing.
    CHECK(rampGradient(applied(canvas, edit), ramp, g, options).empty());
}

TEST_CASE("gradient: with iCE off it writes no bright background and stops short of one", "[gradient]") {
    const Ramp fire = *findRamp("fire");
    Gradient g;
    g.x0 = 0, g.x1 = 79, g.toRung = fire.topRung();
    const Canvas canvas = filled(80, 3, Cell{});
    StrokeOptions options;
    options.ice = false;
    const Canvas out = applied(canvas, rampGradient(canvas, fire, g, options));
    std::set<int> seen;
    for (int x = 0; x < 80; ++x) {
        const Cell& c = out.at(x, 1);
        CHECK(c.bg.paletteIndex() < 8);
        seen.insert(*rungOf(fire, c));
    }
    int top = 0;  // the last rung writable without iCE, unbroken from 0
    while (top < fire.topRung() && rungCell(fire, top + 1).bg.paletteIndex() < 8)
        ++top;
    REQUIRE(top < fire.topRung());  // fire has two bright neighbours
    CHECK(*seen.rbegin() == top);
    CHECK(int(seen.size()) == top + 1);
    // With iCE on it goes to the top.
    const Canvas lit = applied(canvas, rampGradient(canvas, fire, g));
    CHECK(rungOf(fire, lit.at(79, 1)) == fire.topRung());
}

TEST_CASE("gradient: a fill in a selection steps into the field around it", "[gradient]") {
    // A 1-3 fill over the left 30 columns of a field at steel's top rung
    // ran its 3 plane 14 cells into the field and jumped 3 to 12 there.
    const Ramp ramp = steel();
    const Canvas field = filled(40, 10, rungCell(ramp, ramp.topRung()));
    Gradient g;
    g.x0 = 0, g.x1 = 29, g.fromRung = 1, g.toRung = 3;
    StrokeOptions options;
    options.selection = selectionMask(Selection(CellRect{0, 0, 30, 10}), 40, 10);
    const Canvas out = applied(field, rampGradient(field, ramp, g, options));
    const lint::LadderReport r = lint::checkLadder(out, ramp);
    INFO(describe(r));
    CHECK(r.clean());
    std::set<int> row;
    for (int x = 0; x < 30; ++x)
        row.insert(*rungOf(ramp, out.at(x, 4)));
    CHECK(rungOf(ramp, out.at(0, 4)) == 1);  // it starts where it was asked to
    CHECK(row.count(2) == 1);
    CHECK(row.count(3) == 1);
    for (int x = 30; x < 40; ++x)
        CHECK(rungOf(ramp, out.at(x, 4)) == ramp.topRung());  // and paints nothing outside
}

TEST_CASE("gradient: a radial centre just off the canvas leaves no speck at the edge", "[gradient]") {
    // The first ring, a band 2 columns out, clipped to the one cell beside
    // the centre: darker than its three neighbours.
    const Ramp ramp = steel();
    const Canvas blank = filled(32, 12, Cell{});
    for (auto [x0, x1] : {std::pair{-1, 25}, std::pair{32, 6}}) {
        Gradient g;
        g.shape = GradientShape::Radial;
        g.x0 = x0, g.y0 = 5, g.x1 = x1, g.y1 = 5, g.fromRung = 1, g.toRung = 11;
        const Canvas out = applied(blank, rampGradient(blank, ramp, g));
        const lint::LadderReport r = lint::checkLadder(out, ramp);
        INFO("centre " << x0 << ",5\n" << describe(r));
        CHECK(r.clean());
    }
}

TEST_CASE("gradient: three rungs across a solid aren't all one glyph", "[gradient]") {
    // Steel's 3 is K▓ on black and its 5 K▓ on w: each shade written the one
    // way that keeps its background dark, both ▓ (7 and 9 both ░).
    const Ramp ramp = steel();
    const Canvas blank = filled(20, 4, Cell{});
    for (bool ice : {true, false})
        for (auto [from, to] : {std::pair{3, 5}, std::pair{5, 3}, std::pair{7, 9}, std::pair{9, 7}}) {
            Gradient g;
            g.x0 = 0, g.x1 = 19, g.fromRung = from, g.toRung = to;
            StrokeOptions options;
            options.ice = ice;
            const Canvas out = applied(blank, rampGradient(blank, ramp, g, options));
            const lint::LadderReport r = lint::checkLadder(out, ramp);
            INFO((ice ? "ice " : "no-ice ") << from << "-" << to << "\n" << describe(r));
            CHECK(r.clean());
            std::set<int> row;
            for (int x = 0; x < 20; ++x)
                row.insert(*rungOf(ramp, out.at(x, 0)));
            CHECK(rungOf(ramp, out.at(0, 0)) == from);
            CHECK(row.count((from + to) / 2) == 1);
            CHECK(row.count(to) == 1);
        }
}

TEST_CASE("gradient: ends at the limits of int don't overflow", "[gradient]") {
    const Ramp ramp = steel();
    const int lo = std::numeric_limits<int>::min(), hi = std::numeric_limits<int>::max();
    const auto at = [&](int x0, int y0, int x1, int y1) {
        Gradient g;
        g.x0 = x0, g.y0 = y0, g.x1 = x1, g.y1 = y1, g.toRung = ramp.topRung();
        const std::vector<int> rungs = gradientRungs(ramp, g, 1, 1);
        REQUIRE(rungs.size() == 1);
        return rungs[0];
    };
    // The one cell half way along: the middle of the ladder (a row's ends
    // fall a column further along, onto rung 6's end or 5's).
    CHECK(at(lo, 0, hi, 0) == 6);
    CHECK(at(hi, 0, lo, 0) == 6);
    CHECK(at(0, lo, 0, hi) == 7);
    CHECK(at(0, hi, 0, lo) == 6);
    // 2^31 columns past an end one cell from the start: the top rung.
    CHECK(at(lo, 0, lo + 1, 0) == ramp.topRung());
    CHECK(at(0, lo, 0, lo + 1) == ramp.topRung());
}

namespace {

Gradient aimed(GradientShape shape, int x0, int y0, int deg, double len, int from, int to) {
    Gradient g;
    g.shape = shape;
    g.x0 = x0, g.y0 = y0;
    const double a = deg * std::numbers::pi / 180.0;
    g.x1 = x0 + int(std::lround(len * std::cos(a)));
    g.y1 = y0 + int(std::lround(len * std::sin(a) / 2.0));
    g.fromRung = from, g.toRung = to;
    return g;
}

// The ranges the sweeps try: the whole ramp both ways, inside it, one within
// a pair, and three rungs across a solid both ways at each end.
std::vector<std::pair<int, int>> sweepRanges(const Ramp& ramp) {
    const int top = ramp.topRung();
    return {{0, top}, {top, 0}, {1, top - 1}, {6, 2}, {1, 3}, {3, 5}, {5, 3}, {top - 5, top - 3}, {top - 3, top - 5}};
}

// What the gradient shows on a canvas padded far enough to hold all of it:
// its whole ladder.
std::set<int> wholeLadder(const Ramp& ramp, Gradient g, int w, int h, bool ice) {
    const int pad = 240;  // columns; a row counts two
    g.x0 += pad, g.x1 += pad, g.y0 += pad / 2, g.y1 += pad / 2;
    const std::vector<int> rungs = gradientRungs(ramp, g, w + 2 * pad, h + pad, ice);
    return {rungs.begin(), rungs.end()};
}

// The one finding a fill may leave: one glyph, where what it paints shows
// fewer than three rungs or not all of its ladder (the drag runs on past
// the canvas or the selection), so it can't show two glyphs.
bool forgiven(const lint::LadderFinding& f, const std::set<int>& shown, const Ramp& ramp, const Gradient& g, int w,
              int h, bool ice) {
    if (f.kind != lint::LadderFinding::Kind::OneGlyph)
        return false;
    if (shown.size() < 3)
        return true;
    const std::set<int> whole = wholeLadder(ramp, g, w, h, ice);
    return !std::includes(shown.begin(), shown.end(), whole.begin(), whole.end());
}

// How many cells in from the box's edge a wide rung's run comes, from the
// x, y to nx, ny it reports, reading only its cells in the box (the ones the
// fill paints): 0 when one of them is on the edge, none when it has none.
// Only the sides with the field beyond them count; a side on the canvas's
// edge has nothing to climb to.
std::optional<int> runFromEdge(const lint::LadderFinding& f, const CellRect& box, int w, int h) {
    std::optional<int> best;
    const int sx = (f.nx > f.x) - (f.nx < f.x), sy = (f.ny > f.y) - (f.ny < f.y);
    for (int x = f.x, y = f.y;; x += sx, y += sy) {
        if (x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height) {
            int in = std::numeric_limits<int>::max();
            if (box.x > 0)
                in = std::min(in, x - box.x);
            if (box.x + box.width < w)
                in = std::min(in, box.x + box.width - 1 - x);
            if (box.y > 0)
                in = std::min(in, y - box.y);
            if (box.y + box.height < h)
                in = std::min(in, box.y + box.height - 1 - y);
            best = std::min(best.value_or(in), in);
        }
        if (x == f.nx && y == f.ny)
            break;
    }
    return best;
}

// Whether a wide rung's run is the L: a band of the fill joining the climb's
// run of the same rung. The climb from a field of rung `e` moves a rung a
// cell in from the edge, so its cells of rung r sit fewer than |r - e| cells
// in; a run that comes no nearer the edge than that is a wide rung of the
// fill's own, not the L, and one with no cell in the box is the field's.
bool onClimb(const lint::LadderFinding& f, const Canvas& out, const Ramp& ramp, const CellRect& box, int e, int w,
             int h) {
    const std::optional<int> r = rungOf(ramp, out.at(f.x, f.y));
    const std::optional<int> in = runFromEdge(f, box, w, h);
    return r && in && *in < std::abs(*r - e);
}

std::string named(const Ramp& ramp, const Gradient& g, bool ice) {
    std::ostringstream out;
    out << ramp.name << (ice ? " ice" : " no-ice") << (g.shape == GradientShape::Linear ? " linear " : " radial ")
        << g.x0 << "," << g.y0 << " to " << g.x1 << "," << g.y1 << " rungs " << g.fromRung << "-" << g.toRung;
    return out.str();
}

// A fill of a blank canvas: every finding but a forgiven one fails.
void sweepBlank(const Canvas& blank, const Ramp& ramp, const Gradient& g, bool ice, int& painted) {
    StrokeOptions options;
    options.ice = ice;
    const CellEdit edit = rampGradient(blank, ramp, g, options);
    if (edit.empty())
        return;
    ++painted;
    const Canvas out = applied(blank, edit);
    const lint::LadderReport r = lint::checkLadder(out, ramp);
    if (r.clean())
        return;
    INFO(named(ramp, g, ice) << "\n" << describe(r));
    std::set<int> shown;
    for (int y = 0; y < out.height(); ++y)
        for (int x = 0; x < out.width(); ++x)
            shown.insert(*rungOf(ramp, out.at(x, y)));
    for (const auto& f : r.findings)
        CHECK(forgiven(f, shown, ramp, g, out.width(), out.height(), ice));
}

} // namespace

TEST_CASE("gradient: what it fills is ladder-lint clean at every angle, both shapes, every ramp", "[gradient]") {
    const Canvas blank = filled(64, 32, Cell{});
    int painted = 0;
    for (const Ramp& ramp : namedRamps())
        for (bool ice : {true, false})
            for (GradientShape shape : {GradientShape::Linear, GradientShape::Radial})
                for (int deg = 0; deg < 360; deg += 15)
                    for (double len : {6.0, 13.0, 20.0, 27.0, 33.0, 45.0, 52.0, 58.0, 71.0, 90.0})
                        for (auto [from, to] : sweepRanges(ramp))
                            sweepBlank(blank, ramp, aimed(shape, 31, 15, deg, len, from, to), ice, painted);
    CHECK(painted > 5000);
}

TEST_CASE("gradient: a start on the canvas's edge or off it is lint clean too", "[gradient]") {
    const int w = 32, h = 16;
    const Canvas blank = filled(w, h, Cell{});
    const std::pair<int, int> starts[] = {{-1, 7}, {0, 7},  {w - 1, 7}, {w, 7},  {-2, 7},  {w + 1, 7}, {15, -1},
                                          {15, 0}, {15, h - 1}, {15, h}, {-1, -1}, {0, 0}, {w, h},    {-1, 0}};
    int painted = 0;
    for (const Ramp& ramp : namedRamps())
        for (bool ice : {true, false})
            for (GradientShape shape : {GradientShape::Linear, GradientShape::Radial})
                for (auto [x0, y0] : starts)
                    for (int deg = 0; deg < 360; deg += 45)
                        for (double len : {6.0, 20.0, 26.0, 45.0})
                            for (auto [from, to] : sweepRanges(ramp))
                                sweepBlank(blank, ramp, aimed(shape, x0, y0, deg, len, from, to), ice, painted);
    CHECK(painted > 5000);
}

// Where a fill meets the field round a selection it climbs to the field's
// rung, square to the edge or not; what it leaves it reports. The one thing
// it may leave is a wide rung where a band or ring of its own joins the climb
// at a slant to the edge, and that only now and then.
TEST_CASE("gradient: a fill in a selection meets the field round it clean, but for a reported L", "[gradient]") {
    const int w = 40, h = 20;
    int painted = 0, joined = 0;
    for (const Ramp& ramp : namedRamps()) {
        const int top = ramp.topRung();
        for (int e : {kRungsPerStep, top, 2, top - 1}) {  // solids and shades, dark and bright
            const Canvas field = filled(w, h, rungCell(ramp, e));
            StrokeLint lint(field, ramp);
            for (bool ice : {true, false})
                for (const CellRect& box : {CellRect{0, 0, 26, h}, CellRect{6, 4, 24, 11}}) {
                    StrokeOptions options;
                    options.ice = ice;
                    options.selection = selectionMask(Selection(box), w, h);
                    const int cx = box.x + box.width / 2, cy = box.y + box.height / 2;
                    for (GradientShape shape : {GradientShape::Linear, GradientShape::Radial})
                        for (int deg = 0; deg < 360; deg += 45)
                            for (double len : {10.0, 30.0})
                                for (auto [from, to] : {std::pair{0, top}, std::pair{top, 0}, std::pair{1, 3},
                                                        std::pair{3, 1}, std::pair{6, 2}}) {
                                    const Gradient g = aimed(shape, cx, cy, deg, len, from, to);
                                    std::vector<lint::LadderFinding> added;
                                    const CellEdit edit = rampGradient(field, ramp, g, options, &added);
                                    if (edit.empty())
                                        continue;
                                    ++painted;
                                    const Canvas out = applied(field, edit);
                                    std::vector<std::pair<int, int>> changed;
                                    for (const CellChange& c : edit.changes)
                                        changed.push_back({c.x, c.y});
                                    const std::vector<lint::LadderFinding> found = lint.judge(out, changed);
                                    INFO(named(ramp, g, ice) << " in " << box.x << "," << box.y << " " << box.width << "x"
                                                             << box.height << " on rung " << e);
                                    CHECK(added == found);
                                    // With iCE off it can't climb to a field past a rung that
                                    // needs a bright background.
                                    bool reach = true;
                                    for (int r = std::min(from, e); r <= std::max(from, e); ++r)
                                        reach = reach && (ice || rungCell(ramp, r).bg.paletteIndex() < 8);
                                    if (found.empty() || !reach)
                                        continue;
                                    std::set<int> shown;
                                    for (int y = box.y; y < box.y + box.height; ++y)
                                        for (int x = box.x; x < box.x + box.width; ++x)
                                            shown.insert(*rungOf(ramp, out.at(x, y)));
                                    const bool square = shape == GradientShape::Linear && deg % 90 == 0;
                                    bool wide = false;
                                    for (const auto& f : found) {
                                        INFO(f.message);
                                        const bool l = f.kind == lint::LadderFinding::Kind::WideRung && !square;
                                        wide = wide || l;
                                        CHECK((l || forgiven(f, shown, ramp, g, w, h, ice)));
                                        if (l)
                                            CHECK(onClimb(f, out, ramp, box, e, w, h));
                                    }
                                    joined += wide;
                                }
                }
        }
    }
    // Pinned as measured: a change to the fill that moves either is one to
    // look at, and to measure again.
    INFO(joined << " of " << painted << " fills joined a band to the climb");
    CHECK(painted == 25092);
    CHECK(joined == 138);
}

// A fill in a selection lays out only the box its cells take. A mask of the
// same cells whose bounds were stretched to the whole canvas (a corner cell
// set and cleared again) makes it lay out every cell, as it did before it was
// windowed: each edit must be the same, on a field round the box of one rung
// and on one striped with all of them.
TEST_CASE("gradient: a fill in a selection is the same as one laid out over the whole canvas", "[gradient]") {
    const int w = 40, h = 20;
    int compared = 0;
    for (const Ramp& ramp : namedRamps()) {
        const int top = ramp.topRung();
        Canvas striped(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                striped.at(x, y) = rungCell(ramp, (x / 3 + y / 2) % (top + 1));
        striped.at(8, 6).transparent = true;
        for (const Canvas& field : {filled(w, h, rungCell(ramp, top - 1)), striped})
            for (bool ice : {true, false})
                for (bool lock : {false, true})
                    for (const CellRect& box : {CellRect{6, 4, 24, 11}, CellRect{0, 0, 26, h}, CellRect{30, 15, 10, 5}}) {
                        Mask stretched = Mask::rect(w, h, box.x, box.y, box.x + box.width - 1, box.y + box.height - 1);
                        stretched.set(0, 0);
                        stretched.set(w - 1, h - 1);
                        if (!box.contains(0, 0))
                            stretched.set(0, 0, false);
                        if (!box.contains(w - 1, h - 1))
                            stretched.set(w - 1, h - 1, false);
                        REQUIRE(stretched.bounds() == CellRect{0, 0, w, h});
                        StrokeOptions windowed, whole;
                        windowed.ice = whole.ice = ice;
                        windowed.lockTransparent = whole.lockTransparent = lock;
                        windowed.selection = selectionMask(Selection(box), w, h);
                        whole.selection = std::make_shared<const Mask>(stretched);
                        REQUIRE(*windowed.selection == *whole.selection);
                        REQUIRE(windowed.selection->bounds() == box);
                        const int cx = box.x + box.width / 2, cy = box.y + box.height / 2;
                        for (GradientShape shape : {GradientShape::Linear, GradientShape::Radial})
                            for (int deg = 0; deg < 360; deg += 45)
                                for (auto [from, to] : {std::pair{0, top}, std::pair{top, 0}, std::pair{1, 3}}) {
                                    const Gradient g = aimed(shape, cx, cy, deg, 20.0, from, to);
                                    INFO(named(ramp, g, ice) << " in " << box.x << "," << box.y << " " << box.width
                                                             << "x" << box.height << (lock ? " locked" : ""));
                                    const CellEdit a = rampGradient(field, ramp, g, windowed);
                                    const CellEdit b = rampGradient(field, ramp, g, whole);
                                    CHECK(a.changes == b.changes);
                                    compared += !a.empty();
                                }
                    }
    }
    CHECK(compared > 1000);
}

// The window holds the layout and settling passes to the selection's box, but
// the clamp's walks along a line go on past it through a field of one rung,
// to another rung or the canvas's edge: on a field of steel's 4 a fill in a
// 20 x 10 box still costs more on a taller canvas (20 visits a row, the
// bottom edge's walks down), against 1,200 a move on a blank canvas. Pinned
// so the cost is known, not hidden; the edit is still the whole-canvas one.
TEST_CASE("gradient: the clamp's walks in a selection reach through the field round it to the canvas's edge",
          "[gradient]") {
    const Ramp ramp = steel();
    const int w = 80;
    const CellRect box{10, 5, 20, 10};
    Gradient g;
    g.x0 = 12, g.y0 = 7, g.x1 = 26, g.y1 = 12, g.fromRung = 0, g.toRung = 0;
    const auto visits = [&](int h) {
        const Canvas field = filled(w, h, rungCell(ramp, 4));
        StrokeOptions windowed, whole;
        windowed.selection = selectionMask(Selection(box), w, h);
        Mask stretched = Mask::rect(w, h, box.x, box.y, box.x + box.width - 1, box.y + box.height - 1);
        stretched.set(0, 0);
        stretched.set(w - 1, h - 1);
        stretched.set(0, 0, false);
        stretched.set(w - 1, h - 1, false);
        whole.selection = std::make_shared<const Mask>(stretched);
        REQUIRE(windowed.selection->bounds() == box);
        REQUIRE(whole.selection->bounds() == CellRect{0, 0, w, h});
        const long long before = gradientCellsVisited();
        const CellEdit edit = rampGradient(field, ramp, g, windowed);
        const long long cost = gradientCellsVisited() - before;
        INFO(w << "x" << h << ": " << cost << " visits");
        CHECK_FALSE(edit.empty());
        CHECK(edit.changes == rampGradient(field, ramp, g, whole).changes);
        return cost;
    };
    const long long shortField = visits(40), tallField = visits(120);
    CHECK(shortField == 2320);
    CHECK(tallField == 3920);
    CHECK(tallField - shortField == 20 * (120 - 40));
}

// The L's check reads the painted cells only: a wide rung wholly in the field
// round the box has no contact with the climb, whatever its rung.
TEST_CASE("gradient: a wide rung outside the selection isn't on the climb", "[gradient]") {
    const Ramp steel = *findRamp("steel");
    const int w = 40, h = 20;
    const CellRect box{6, 4, 24, 11};
    Canvas out = filled(w, h, rungCell(steel, 2));
    lint::LadderFinding f{.kind = lint::LadderFinding::Kind::WideRung, .x = 10, .y = 0, .nx = 14, .ny = 0};
    CHECK_FALSE(onClimb(f, out, steel, box, 2, w, h));
    f.y = f.ny = h - 1;
    CHECK_FALSE(onClimb(f, out, steel, box, 2, w, h));
    f.x = 0, f.nx = 4, f.y = f.ny = 8;
    CHECK_FALSE(onClimb(f, out, steel, box, 2, w, h));
    // the same run on the box's edge, a rung off the field's, is
    for (int x = 10; x <= 14; ++x)
        out.at(x, 4) = rungCell(steel, 3);
    f.x = 10, f.nx = 14, f.y = f.ny = 4;
    CHECK(onClimb(f, out, steel, box, 2, w, h));
    // and a cell in, not
    f.y = f.ny = 5;
    for (int x = 10; x <= 14; ++x)
        out.at(x, 5) = rungCell(steel, 3);
    CHECK_FALSE(onClimb(f, out, steel, box, 2, w, h));
}

TEST_CASE("gradient: on a tall canvas a square fill in a selection is clean and a slanted one adds only the L", "[gradient]") {
    const int w = 120, h = 300;
    const CellRect box{5, 5, w - 10, h - 10};
    int slanted = 0, joined = 0;
    for (const char* name : {"steel", "sunset"}) {
        const Ramp ramp = *findRamp(name);
        const int top = ramp.topRung();
        for (int e : {kRungsPerStep, top - 1}) {
            const Canvas field = filled(w, h, rungCell(ramp, e));
            StrokeLint lint(field, ramp);
            StrokeOptions options;
            options.selection = selectionMask(Selection(box), w, h);
            const int ends[][5] = {{0, 5, h / 2, w - 6, h / 2},    {0, w / 2, 5, w / 2, h - 6},
                                   {0, 10, 10, w - 10, h - 20},    {1, 10, 10, w - 10, h - 20},
                                   {0, w / 2, h / 2, w / 2 + 30, h / 2 + 40}, {1, w / 2, h / 2, w / 2 + 30, h / 2 + 40}};
            for (const auto& end : ends)
                for (auto [from, to] : {std::pair{1, 3}, std::pair{0, top}, std::pair{top, 0}, std::pair{6, 2}}) {
                    Gradient g;
                    g.shape = end[0] ? GradientShape::Radial : GradientShape::Linear;
                    g.x0 = end[1], g.y0 = end[2], g.x1 = end[3], g.y1 = end[4];
                    g.fromRung = from, g.toRung = to;
                    std::vector<lint::LadderFinding> added;
                    const CellEdit edit = rampGradient(field, ramp, g, options, &added);
                    const Canvas out = applied(field, edit);
                    std::vector<std::pair<int, int>> changed;
                    for (const CellChange& c : edit.changes)
                        changed.push_back({c.x, c.y});
                    const std::vector<lint::LadderFinding> found = lint.judge(out, changed);
                    INFO(named(ramp, g, true) << " on rung " << e);
                    CHECK(added == found);
                    const bool square = !end[0] && (end[1] == end[3] || end[2] == end[4]);
                    if (square) {
                        CHECK(found.empty());
                        continue;
                    }
                    ++slanted;
                    joined += !found.empty();
                    for (const auto& f : found) {
                        INFO(f.message);
                        CHECK(f.kind == lint::LadderFinding::Kind::WideRung);
                        CHECK(onClimb(f, out, ramp, box, e, w, h));
                    }
                }
        }
    }
    // Pinned as measured, like the counts above.
    INFO(joined << " of " << slanted << " slanted fills joined a band to the climb");
    CHECK(slanted == 64);
    CHECK(joined == 34);
}

// ---- the Bayer banding (#160) ---------------------------------------------

namespace {

std::vector<std::pair<int, int>> marks(const Canvas& c) {
    std::vector<std::pair<int, int>> at;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (c.at(x, y).dithered)
                at.emplace_back(x, y);
    return at;
}

Canvas unmarked(Canvas c) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            c.at(x, y).dithered = false;
    return c;
}

int specks(const lint::LadderReport& r) {
    return int(std::count_if(r.findings.begin(), r.findings.end(), [](const lint::LadderFinding& f) {
        return f.kind == lint::LadderFinding::Kind::Speckle;
    }));
}

Gradient bayer(Gradient g) {
    g.banding = GradientBanding::Bayer;
    return g;
}

} // namespace

TEST_CASE("gradient: the Bayer banding mixes each band edge in the 4×4 order, and marks the cells it mixes",
          "[gradient][dither]") {
    const Ramp ramp = steel();
    const Canvas blank(60, 8);
    Gradient g;
    g.x0 = 0, g.x1 = 59, g.fromRung = 0, g.toRung = ramp.topRung();
    const std::vector<int> cells = gradientRungs(ramp, g, 60, 8);
    g.banding = GradientBanding::Bayer;
    const CellEdit edit = rampGradient(blank, ramp, g, StrokeOptions{});
    CHECK(edit.dithers);
    const Canvas out = applied(blank, edit);
    // The rungs and the marks, a row at a time; the order repeats every four
    // rows. Black (rung 0) isn't mixed; each other edge is, plateau and shade
    // both ways.
    const char* want[] = {
        "000000000001122344444444445556678888888888999aabcccccccccccc",
        "................*.........*.....*.........*.....*...........",
        "000000000001122333444444444556677788888888899aabbbcccccccccc",
        ".................*...............*...............*..........",
        "000000000001122344344444445556678878888888999aabccbccccccccc",
        "................*.*.......*.....*.*.......*.....*.*.........",
        "000000000001122334444444444556677888888888899aabbccccccccccc",
        "............................................................",
    };
    for (int y = 0; y < 8; ++y) {
        std::string rungs, marked;
        for (int x = 0; x < 60; ++x) {
            rungs += "0123456789abcdef"[*rungOf(ramp, out.at(x, y))];
            marked += out.at(x, y).dithered ? '*' : '.';
        }
        INFO("row " << y);
        CHECK(rungs == want[2 * (y % 4)]);
        CHECK(marked == want[2 * (y % 4) + 1]);
    }
    // Every cell is the cell-banded fill's but a marked one, which is a rung
    // from it where its Bayer order is under the edge's threshold.
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 60; ++x) {
            INFO(x << "," << y);
            const int was = cells[size_t(y * 60 + x)], is = *rungOf(ramp, out.at(x, y));
            const int order = kBayer4[y % 4][x % 4];
            if (!out.at(x, y).dithered) {
                CHECK(is == was);
                continue;
            }
            CHECK(std::abs(is - was) == 1);
            CHECK(order < (was % kRungsPerStep ? kDitherBand : kDitherNear));
        }
    // gradientRungs gives what the fill paints
    CHECK(gradientRungs(ramp, g, 60, 8) == [&] {
        std::vector<int> r;
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 60; ++x)
                r.push_back(*rungOf(ramp, out.at(x, y)));
        return r;
    }());
}

TEST_CASE("gradient: a Bayer fill is ladder-lint clean at every angle, its dither specks only to a lint blind to the mark",
          "[gradient][dither]") {
    const Canvas blank = filled(64, 32, Cell{});
    int painted = 0, dithered = 0, unmarkedSpecks = 0;
    for (const Ramp& ramp : namedRamps())
        for (bool ice : {true, false})
            for (GradientShape shape : {GradientShape::Linear, GradientShape::Radial})
                for (int deg = 0; deg < 360; deg += 30)
                    for (double len : {6.0, 20.0, 33.0, 52.0, 90.0})
                        for (auto [from, to] : sweepRanges(ramp)) {
                            const Gradient g = bayer(aimed(shape, 31, 15, deg, len, from, to));
                            const int was = painted;
                            sweepBlank(blank, ramp, g, ice, painted);
                            if (painted == was)
                                continue;
                            StrokeOptions options;
                            options.ice = ice;
                            const Canvas out = applied(blank, rampGradient(blank, ramp, g, options));
                            dithered += int(marks(out).size());
                            unmarkedSpecks += specks(lint::checkLadder(unmarked(out), ramp));
                            // with iCE off the dither writes no bright background either
                            if (!ice)
                                for (const auto& [x, y] : marks(out))
                                    CHECK(out.at(x, y).bg.paletteIndex() < 8);
                        }
    CHECK(painted > 2000);
    INFO(dithered << " dithered cells, " << unmarkedSpecks << " specks without their marks");
    CHECK(dithered > 10 * painted);
    CHECK(unmarkedSpecks > painted);
}

TEST_CASE("gradient: in a selection the Bayer banding adds no finding the cell-banded fill doesn't", "[gradient][dither]") {
    const int w = 40, h = 20;
    int painted = 0;
    for (const Ramp& ramp : namedRamps()) {
        const int top = ramp.topRung();
        for (int e : {kRungsPerStep, top, 2, top - 1}) {
            const Canvas field = filled(w, h, rungCell(ramp, e));
            for (const CellRect& box : {CellRect{0, 0, 26, h}, CellRect{6, 4, 24, 11}}) {
                StrokeOptions options;
                options.selection = selectionMask(Selection(box), w, h);
                const int cx = box.x + box.width / 2, cy = box.y + box.height / 2;
                for (GradientShape shape : {GradientShape::Linear, GradientShape::Radial})
                    for (int deg = 0; deg < 360; deg += 45)
                        for (auto [from, to] : {std::pair{0, top}, std::pair{top, 0}, std::pair{6, 2}}) {
                            const Gradient g = aimed(shape, cx, cy, deg, 30.0, from, to);
                            std::vector<lint::LadderFinding> cells, dither;
                            rampGradient(field, ramp, g, options, &cells);
                            const CellEdit edit = rampGradient(field, ramp, bayer(g), options, &dither);
                            if (edit.empty())
                                continue;
                            ++painted;
                            INFO(named(ramp, g, true) << " on rung " << e);
                            CHECK(dither.size() <= cells.size());
                            for (const CellChange& c : edit.changes)
                                CHECK(options.selection->contains(c.x, c.y));
                        }
            }
        }
    }
    CHECK(painted > 1000);
}

// A Bayer fill with no cell to dither (both ends black) writes no fill out
// over the canvas and builds no lint index: a drag over a selection costs its
// cells, however large the canvas.
TEST_CASE("gradient: a Bayer fill with nothing to dither builds no lint index", "[gradient][dither]") {
    const Ramp ramp = steel();
    const int w = 400, h = 500;
    const Canvas blank(w, h);
    StrokeOptions options;
    options.selection = selectionMask(Selection(CellRect{100, 200, 20, 10}), w, h);
    Gradient g;
    g.x0 = 100, g.x1 = 119, g.y0 = g.y1 = 205, g.fromRung = 0, g.toRung = 0;
    const long long built = lint::indexesBuilt();
    const CellEdit flat = rampGradient(blank, ramp, bayer(g), options);
    CHECK(lint::indexesBuilt() == built);
    for (const CellChange& c : flat.changes)
        CHECK_FALSE(c.after.dithered);
    // a fill that dithers judges its dither with one index
    g.toRung = ramp.topRung();
    const CellEdit mixed = rampGradient(blank, ramp, bayer(g), options);
    REQUIRE_FALSE(marks(applied(blank, mixed)).empty());
    CHECK(lint::indexesBuilt() == built + 1);
}

TEST_CASE("gradient: a fill over a dithered one keeps no stale mark", "[gradient][dither]") {
    const Ramp ramp = steel();
    const Canvas blank(60, 4);
    Gradient g;
    g.x0 = 0, g.x1 = 59, g.toRung = ramp.topRung();
    const Canvas dithered = applied(blank, rampGradient(blank, ramp, bayer(g), StrokeOptions{}));
    REQUIRE_FALSE(marks(dithered).empty());
    // the cell-banded fill over it: the cells it leaves looking the same
    // still lose their marks
    const CellEdit again = rampGradient(dithered, ramp, g, StrokeOptions{});
    CHECK_FALSE(again.dithers);
    const Canvas out = applied(dithered, again);
    CHECK(marks(out).empty());
    CHECK(out == applied(blank, rampGradient(blank, ramp, g, StrokeOptions{})));
    // and the same Bayer fill again changes nothing
    CHECK(rampGradient(dithered, ramp, bayer(g), StrokeOptions{}).empty());
}

TEST_CASE("gradient: a dither saved as .ans comes back unmarked, and the lint calls it specks", "[gradient][dither]") {
    // An .ans has no place for the mark, and a dither by hand writes the
    // same bytes: read back, the dither is specks (a project keeps it).
    const Ramp ramp = steel();
    AnsiDocument doc;
    doc.canvas = Canvas(60, 8);
    Gradient g;
    g.x0 = 0, g.x1 = 59, g.toRung = ramp.topRung();
    doc.canvas = applied(doc.canvas, rampGradient(doc.canvas, ramp, bayer(g), StrokeOptions{}));
    REQUIRE(specks(lint::checkLadder(doc.canvas, ramp)) == 0);
    const AnsiDocument back = parseAnsi(writeAnsi(doc).bytes);
    CHECK(marks(back.canvas).empty());
    CHECK(specks(lint::checkLadder(back.canvas, ramp)) > 0);
}
