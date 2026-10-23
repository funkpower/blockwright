// Outline brush (PLAN §10, "Detail").

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/halfblock.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/shapes.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <set>
#include <tuple>

using namespace bw;

namespace {

using Point = std::pair<int, int>;

Ramp steel() { return *findRamp("steel"); }  // k K w W

Canvas blank(int w, int h) { return Canvas(w, h); }

Cell glyph(char32_t ch, std::uint8_t fg = White) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    return c;
}

// The cells an edit writes, by position.
std::map<Point, Cell> written(const CellEdit& e) {
    std::map<Point, Cell> out;
    for (const CellChange& c : e.changes)
        out[{c.x, c.y}] = c.after;
    return out;
}

} // namespace

namespace {

// A steel blob on black: rows 1-4, columns 2-6, each row the gradient
// w, W░w, W▒w, W▓w, W (rungs 8-12).
Canvas blob() {
    Canvas c(10, 6);
    for (int y = 1; y <= 4; ++y)
        for (int x = 2; x <= 6; ++x)
            c.at(x, y) = rungCell(steel(), 8 + (x - 2));
    return c;
}

std::vector<lint::LadderFinding> skips(const Canvas& c) {
    std::vector<lint::LadderFinding> out;
    for (const auto& f : lint::checkLadder(c, steel()).findings)
        if (f.kind == lint::LadderFinding::Kind::RungSkip)
            out.push_back(f);
    return out;
}

} // namespace

TEST_CASE("the outline brush draws the one-cell end level the lint accepts", "[detail][lint]") {
    const Canvas c = blob();
    REQUIRE(skips(c).empty());
    const OutlineBrush outline(steel());
    BrushSettings s;
    s.size = 12;
    const CellEdit e = paintStroke(c, outline, s, {{4, 2}});
    Canvas out = c;
    e.apply(out);
    // the edge cells, and only they, are K: the colour below w, the darker
    // colour of every level inside the edge
    std::set<Point> changed;
    for (const CellChange& ch : e.changes) {
        changed.insert({ch.x, ch.y});
        CHECK(ch.after == rungCell(steel(), 4));
    }
    std::set<Point> edge;
    for (int y = 1; y <= 4; ++y)
        for (int x = 2; x <= 6; ++x)
            if (x == 2 || x == 6 || y == 1 || y == 4)
                edge.insert({x, y});
    CHECK(changed == edge);
    // a row reads K, W░w, W▒w, W▓w, K: without the outline rule K next to
    // W░w climbing on is a skipped rung
    CHECK(skips(out).empty());
    CHECK(lint::checkLadder(out, steel()).clean());
    // an outline of any other colour there is a skip
    Canvas wrong = out;
    wrong.at(2, 2) = rungCell(steel(), 0 + 4 * 0);  // black is ink, never a skip
    CHECK(skips(wrong).empty());
    wrong.at(2, 2) = glyph(U'█', Grey);  // w: K's level is gone, w then W░w is fine
    Canvas far = c;
    far.at(2, 2) = rungCell(steel(), 4);
    far.at(3, 2) = rungCell(steel(), 9);
    far.at(4, 2) = rungCell(steel(), 10);
    // two cells deep, the inner one off the silhouette's edge: no longer an
    // outline
    for (int y = 1; y <= 3; ++y)
        far.at(1, y) = rungCell(steel(), 4);
    CHECK_FALSE(skips(far).empty());
    // one stroke outlines one cell deep, however often it passes
    CHECK(written(paintStroke(c, outline, s, {{4, 2}, {5, 3}, {3, 2}, {4, 2}})) == written(e));
    // nothing goes to the ground: the darkest colour above it is the floor,
    // so K stays K and w░K beside it becomes K
    Canvas dark(5, 5);
    for (int y = 1; y <= 3; ++y)
        for (int x = 1; x <= 3; ++x)
            dark.at(x, y) = rungCell(steel(), 4);
    dark.at(3, 2) = rungCell(steel(), 5);
    const auto d = written(paintStroke(dark, outline, s, {{2, 2}}));
    CHECK(d == std::map<Point, Cell>{{{3, 2}, rungCell(steel(), 4)}});
    // a part with no interior near it has no level inside to take: left
    Canvas pair(4, 3);
    pair.at(1, 1) = rungCell(steel(), 4);
    pair.at(2, 1) = rungCell(steel(), 5);
    CHECK(paintStroke(pair, outline, s, {{1, 1}}).empty());
    // the selection and the lock hold
    StrokeOptions o;
    o.selection = std::make_shared<Mask>(Mask::rect(10, 6, 0, 0, 3, 5));
    for (const auto& [p, cell] : written(paintStroke(c, outline, s, {{4, 2}}, o)))
        CHECK(p.first <= 3);
    Canvas locked = c;
    locked.at(2, 2).transparent = true;
    o = {};
    o.lockTransparent = true;
    CHECK_FALSE(written(paintStroke(locked, outline, s, {{4, 2}}, o)).count({2, 2}));
}

namespace {

// The lint's findings as (kind, x, y).
std::set<std::tuple<int, int, int>> findings(const Canvas& c, const Ramp& r) {
    std::set<std::tuple<int, int, int>> out;
    for (const auto& f : lint::checkLadder(c, r).findings)
        out.insert({int(f.kind), f.x, f.y});
    return out;
}

// Findings in `after` that `before` didn't have.
std::set<std::tuple<int, int, int>> added(const Canvas& before, const Canvas& after, const Ramp& r) {
    std::set<std::tuple<int, int, int>> out;
    const auto was = findings(before, r);
    for (const auto& f : findings(after, r))
        if (!was.count(f))
            out.insert(f);
    return out;
}

Canvas applied(Canvas c, const CellEdit& e) {
    e.apply(c);
    return c;
}

// The whole canvas in one dab of the largest tip.
CellEdit outlineAll(const Canvas& c, const Ramp& r, const StrokeOptions& o = {}) {
    BrushSettings s;
    s.size = kMaxBrushSize;
    return paintStroke(c, OutlineBrush(r), s, {{c.width() / 2, c.height() / 2}}, o);
}

// The shape's cells with a cell outside it among their eight neighbours on
// the canvas: where the outline goes.
std::set<Point> rim(const Canvas& c, const Ramp& r) {
    std::set<Point> out;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            if (!inShape(r, c.at(x, y)))
                continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (c.contains(x + dx, y + dy) && !inShape(r, c.at(x + dx, y + dy)))
                        out.insert({x, y});
        }
    return out;
}

// A disc of one cell on black, round on screen.
Canvas disc(int w, int h, int size, const Cell& cell) {
    Canvas c(w, h);
    for (const auto& [x, y] : footprint(w / 2, h / 2, size, Tip::Round, w, h))
        c.at(x, y) = cell;
    return c;
}

} // namespace

TEST_CASE("the outline brush gives a ramp-shaded blob a line", "[detail][lint]") {
    // the PR's "things to try": a blob raised with the ramp shade brush on
    // black, several passes over one spot, each smaller, so its edge is soft
    // (K░k, K▒k, K▓k against black)
    Canvas c(80, 25);
    const RampShadeBrush lighten(steel(), ShadeDirection::Lighten);
    for (int size = 24; size >= 3; size -= 3) {
        BrushSettings s;
        s.size = size;
        s.tip = Tip::Round;
        paintStroke(c, lighten, s, {{40, 12}, {41, 12}}).apply(c);
    }
    const std::set<Point> edge = rim(c, steel());
    REQUIRE(edge.size() > 40);
    int soft = 0;
    for (const auto& [x, y] : edge)
        soft += *rungOf(steel(), c.at(x, y)) < 4;
    REQUIRE(soft > 20);
    const CellEdit e = outlineAll(c, steel());
    const Canvas out = applied(c, e);
    // every edge cell changes to the solid darkest colour above the ground
    // (nothing below K but the ground itself). Off the edge only a few
    // repairs: cells of the fade a speck inside the line, each K now and a
    // speck again if put back. The rest of the fade stays.
    std::set<Point> changed;
    for (const CellChange& ch : e.changes)
        changed.insert({ch.x, ch.y});
    for (const auto& p : edge)
        CHECK(changed.count(p));
    int repairs = 0, fade = 0;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (const auto g = rungOf(steel(), c.at(x, y)); g && *g > 0 && *g < 4 && !edge.count({x, y}))
                ++fade;
    for (const auto& [x, y] : changed) {
        CHECK(rungOf(steel(), out.at(x, y)) == 4);
        if (edge.count({x, y}))
            continue;
        ++repairs;
        INFO("repair at " << x << "," << y);
        REQUIRE(rungOf(steel(), c.at(x, y)) < 4);
        Canvas undone = out;
        undone.at(x, y) = c.at(x, y);
        CHECK(findings(undone, steel()).count({int(lint::LadderFinding::Kind::Speckle), x, y}));
    }
    CHECK(repairs > 0);
    CHECK(repairs * 4 < fade);
    CHECK(added(c, out, steel()).empty());
    // a second stroke changes nothing
    CHECK(outlineAll(out, steel()).empty());
}

TEST_CASE("the outline never turns the shape into the ground", "[detail][lint]") {
    // a solid K disc on black: K is the darkest colour above the ground, so
    // its edge is a line against the ground already and stays
    const Canvas dark = disc(40, 20, 17, rungCell(steel(), 4));
    CHECK(outlineAll(dark, steel()).empty());
    // a w disc: its edge becomes K, the interior stays w
    const Canvas grey = disc(40, 20, 17, rungCell(steel(), 8));
    const CellEdit e = outlineAll(grey, steel());
    const Canvas out = applied(grey, e);
    const std::set<Point> edge = rim(grey, steel());
    CHECK(e.changes.size() == edge.size());
    for (int y = 0; y < grey.height(); ++y)
        for (int x = 0; x < grey.width(); ++x)
            if (inShape(steel(), grey.at(x, y)))
                CHECK(rungOf(steel(), out.at(x, y)) == (edge.count({x, y}) ? 4 : 8));
    CHECK(lint::checkLadder(out, steel()).clean());
}

TEST_CASE("the outline leaves a valid outline as it is", "[detail][lint]") {
    // Codex round 1 #1: ice k b B C W, rows of rungs 8 13 14 15 on black.
    // Rung 8 (solid B) is already the line beside C░B
    const Ramp ice = *findRamp("ice");
    Canvas c(6, 5);
    const int rungs[] = {8, 13, 14, 15};
    for (int y = 1; y <= 3; ++y)
        for (int x = 1; x <= 4; ++x)
            c.at(x, y) = rungCell(ice, rungs[x - 1]);
    REQUIRE(skips(c).empty());
    BrushSettings one;
    CHECK(paintStroke(c, OutlineBrush(ice), one, {{1, 2}}).empty());
    // outlining the whole shape adds no skipped rung
    const Canvas out = applied(c, outlineAll(c, ice));
    for (const auto& f : lint::checkLadder(out, ice).findings)
        CHECK(f.kind != lint::LadderFinding::Kind::RungSkip);
    CHECK(rungOf(ice, out.at(1, 2)) == 8);
}

TEST_CASE("the outline closes round a hole off the ramp", "[detail][lint]") {
    // Codex round 1 #2: a w field with one RGB cell in it. Cells off the
    // ramp count as outside, and the line takes all eight cells round the
    // hole, so none of them is a speck among w
    for (const bool ice : {true, false}) {
        Canvas c(7, 7);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 7; ++x)
                c.at(x, y) = rungCell(steel(), 8);
        c.at(3, 3) = Cell{U'█', Color::fromRgb({170, 170, 170}), Color::indexed(Black)};
        StrokeOptions o;
        o.ice = ice;
        BrushSettings s;
        s.size = 7;
        const CellEdit e = paintStroke(c, OutlineBrush(steel()), s, {{3, 3}}, o);
        std::set<Point> changed;
        for (const CellChange& ch : e.changes) {
            changed.insert({ch.x, ch.y});
            CHECK(ch.after == rungCell(steel(), 4));
        }
        CHECK(changed == std::set<Point>{{2, 2}, {3, 2}, {4, 2}, {2, 3}, {4, 3}, {2, 4}, {3, 4}, {4, 4}});
        CHECK(lint::checkLadder(applied(c, e), steel()).clean());
    }
}

TEST_CASE("the outline finds edges whose sides cancel", "[detail]") {
    // Codex round 1 #3: a one-cell stem, outside on the left and the right,
    // on a w body (its interior the level inside the stem's line)
    Canvas c(7, 8);
    for (int y = 1; y <= 3; ++y)
        c.at(3, y) = rungCell(steel(), 8);
    for (int y = 4; y <= 6; ++y)
        for (int x = 1; x <= 5; ++x)
            c.at(x, y) = rungCell(steel(), 8);
    BrushSettings one;
    CHECK(written(paintStroke(c, OutlineBrush(steel()), one, {{3, 1}, {3, 3}})) ==
          std::map<Point, Cell>{
              {{3, 1}, rungCell(steel(), 4)}, {{3, 2}, rungCell(steel(), 4)}, {{3, 3}, rungCell(steel(), 4)}});
    // the middle of the stem alone would leave its tip a speck: the lint
    // keeps it as it is
    CHECK(paintStroke(c, OutlineBrush(steel()), one, {{3, 2}}).empty());
}

TEST_CASE("the outline takes half-block fringes as part of the shape", "[detail]") {
    // a w shape whose top row is ▄ fringe (w below, black above), as the
    // ellipse tool and the pencil leave it: the line goes into the fringe's
    // black half, outside it, and the cells under the fringe stay w
    Canvas c(10, 6);
    for (int y = 2; y <= 4; ++y)
        for (int x = 2; x <= 7; ++x)
            c.at(x, y) = rungCell(steel(), 8);
    for (int x = 3; x <= 6; ++x)
        c.at(x, 1) = Cell{kLowerHalf, Color::indexed(Grey), Color::indexed(Black)};
    for (const bool ice : {true, false}) {
        StrokeOptions o;
        o.ice = ice;
        const CellEdit e = outlineAll(c, steel(), o);
        const auto w = written(e);
        for (int x = 3; x <= 6; ++x) {
            REQUIRE(w.count({x, 1}));
            const Cell& f = w.at({x, 1});
            // K above, w below: with iCE off written ▀ K on w, keeping the
            // background dark
            if (ice)
                CHECK(f == Cell{kLowerHalf, Color::indexed(Grey), Color::indexed(DarkGrey)});
            else
                CHECK(f == Cell{kUpperHalf, Color::indexed(DarkGrey), Color::indexed(Grey)});
        }
        CHECK_FALSE(w.count({4, 2}));
        CHECK_FALSE(w.count({5, 2}));
        // a second pass leaves the fringe as it is
        CHECK(outlineAll(applied(c, e), steel(), o).empty());
    }
    // a K fringe has nothing darker above the ground: left alone
    Canvas k(6, 4);
    for (int x = 1; x <= 4; ++x) {
        k.at(x, 1) = Cell{kLowerHalf, Color::indexed(DarkGrey), Color::indexed(Black)};
        k.at(x, 2) = rungCell(steel(), 4);
    }
    CHECK(outlineAll(k, steel()).empty());
}

TEST_CASE("the outline reads a 3x3 neighbourhood: no notches, no dotted diagonals", "[detail]") {
    // a w block with a one-cell nub on top: the cell under the nub is
    // outlined too, so the line doesn't notch
    Canvas c(14, 9);
    for (int y = 3; y <= 7; ++y)
        for (int x = 2; x <= 11; ++x)
            c.at(x, y) = rungCell(steel(), 8);
    c.at(6, 2) = rungCell(steel(), 8);
    const Canvas out = applied(c, outlineAll(c, steel()));
    CHECK(rungOf(steel(), out.at(6, 2)) == 4);
    CHECK(rungOf(steel(), out.at(6, 3)) == 4);
    CHECK(rungOf(steel(), out.at(6, 4)) == 8);
    // a 45° side: every line cell touches another by a side
    Canvas tri(16, 12);
    for (int y = 1; y <= 10; ++y)
        for (int x = 1; x <= y + 2; ++x)
            tri.at(x, y) = rungCell(steel(), 8);
    const auto line = written(outlineAll(tri, steel()));
    REQUIRE(line.size() > 20);
    for (const auto& [p, cell] : line) {
        const auto [x, y] = p;
        CHECK((line.count({x + 1, y}) || line.count({x - 1, y}) || line.count({x, y + 1}) ||
               line.count({x, y - 1})));
    }
}

namespace {

// A second outline over the outlined canvas changes nothing.
bool settled(const Canvas& c, const Ramp& r, const StrokeOptions& o = {}) {
    return outlineAll(applied(c, outlineAll(c, r, o)), r, o).empty();
}

} // namespace

TEST_CASE("the outline leaves a fade off the edge alone", "[detail]") {
    // Claude round 2 P2-1, repro 1: a K░k / k▒K fog right of a w block. The
    // brush dragged down the block's right side reaches into the fog; only
    // the fog's own boundary (column 23) touches the outside
    Canvas fog(50, 20);
    for (int y = 0; y < 20; ++y)
        for (int x = 23; x < 50; ++x)
            fog.at(x, y) = rungCell(steel(), (x + y) % 3 == 0 ? 2 : 1);
    for (int y = 4; y <= 15; ++y)
        for (int x = 8; x <= 20; ++x)
            fog.at(x, y) = rungCell(steel(), 8);
    BrushSettings s;
    s.size = 9;
    std::vector<Dab> down;
    for (int y = 4; y <= 15; ++y)
        down.push_back({20, y});
    const CellEdit e = paintStroke(fog, OutlineBrush(steel()), s, down);
    REQUIRE_FALSE(e.empty());
    for (const CellChange& ch : e.changes)
        CHECK(ch.x < 24);
    // repro 2: a k▒K crease darkened into the middle of a w disc, nowhere
    // near an edge
    Canvas crease = disc(50, 22, 21, rungCell(steel(), 8));
    BrushSettings three;
    three.size = 3;
    for (int pass = 0; pass < 6; ++pass)
        paintStroke(crease, RampShadeBrush(steel(), ShadeDirection::Darken), three, {{18, 11}, {32, 11}})
            .apply(crease);
    REQUIRE(rungOf(steel(), crease.at(25, 11)) == 2);
    BrushSettings five;
    five.size = 5;
    CHECK(paintStroke(crease, OutlineBrush(steel()), five, {{25, 11}}).empty());
}

TEST_CASE("the outline is the same after a second stroke", "[detail]") {
    // Claude round 2 P2-2: an ice W body with a two-row arm. The arm has no
    // interior; a second stroke over the outlined shape changes nothing
    const Ramp ice = *findRamp("ice");
    Canvas c(30, 12);
    for (int y = 2; y <= 9; ++y)
        for (int x = 2; x <= 10; ++x)
            c.at(x, y) = rungCell(ice, 16);
    for (int y = 5; y <= 6; ++y)
        for (int x = 11; x <= 26; ++x)
            c.at(x, y) = rungCell(ice, 16);
    const Canvas once = applied(c, outlineAll(c, ice));
    CHECK(outlineAll(once, ice).empty());
    // the body's edge takes the line, and so does the arm along its whole
    // length: its nearest interior is the body's W, however far (round 3, B4)
    CHECK(rungOf(ice, once.at(2, 5)) == 12);
    CHECK(rungOf(ice, once.at(20, 5)) == 12);
    // every outline fixture settles after one stroke, iCE on and off
    for (const bool on : {true, false}) {
        StrokeOptions o;
        o.ice = on;
        CHECK(settled(c, ice, o));
        CHECK(settled(blob(), steel(), o));
        CHECK(settled(disc(40, 20, 17, rungCell(steel(), 8)), steel(), o));
        CHECK(settled(disc(40, 20, 17, rungCell(steel(), 12)), steel(), o));
        Canvas stem(5, 5);
        for (int y = 1; y <= 3; ++y)
            stem.at(2, y) = rungCell(steel(), 8);
        CHECK(settled(stem, steel(), o));
        Canvas hole(7, 7);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 7; ++x)
                hole.at(x, y) = rungCell(steel(), 8);
        hole.at(3, 3) = Cell{U'█', Color::fromRgb({170, 170, 170}), Color::indexed(Black)};
        CHECK(settled(hole, steel(), o));
        Canvas tri(16, 12);
        for (int y = 1; y <= 10; ++y)
            for (int x = 1; x <= y + 2; ++x)
                tri.at(x, y) = rungCell(steel(), std::max(8, 13 - x));
        CHECK(settled(tri, steel(), o));
    }
}

TEST_CASE("the outline's diagonal staircase is lint clean", "[detail][lint]") {
    // Codex round 2 #1: a shaded triangle whose 45° side is outlined as a
    // 4-connected staircase, two cells of the line on each row
    for (const bool on : {true, false}) {
        const Ramp r = steel();
        Canvas c(16, 12);
        for (int y = 1; y <= 10; ++y)
            for (int x = 1; x <= y + 2; ++x)
                c.at(x, y) = rungCell(r, std::max(8, 13 - x));
        REQUIRE(lint::checkLadder(c, r).clean());
        StrokeOptions o;
        o.ice = on;
        const Canvas out = applied(c, outlineAll(c, r, o));
        CHECK(lint::checkLadder(out, r).clean());
        // the staircase is there: row 4 ends in two line cells
        CHECK(rungOf(r, out.at(5, 4)) == 4);
        CHECK(rungOf(r, out.at(6, 4)) == 4);
    }
}

TEST_CASE("the outline of a broad end plane is lint clean", "[detail][lint]") {
    // Codex round 2 #2: a broad rung-9 plane at the gradient's dark end,
    // against black, then outlined
    for (const bool on : {true, false}) {
        const Ramp r = steel();
        Canvas c(16, 8);
        const int rungs[] = {9, 9, 9, 9, 9, 9, 9, 9, 10, 11, 12, 12, 12};
        for (int y = 1; y <= 6; ++y)
            for (int x = 1; x <= 13; ++x)
                c.at(x, y) = rungCell(r, rungs[x - 1]);
        REQUIRE(lint::checkLadder(c, r).clean());
        StrokeOptions o;
        o.ice = on;
        const Canvas out = applied(c, outlineAll(c, r, o));
        REQUIRE(rungOf(r, out.at(1, 3)) == 4);
        CHECK(lint::checkLadder(out, r).clean());
    }
}

namespace {

// A dab of `size` at each point, one stroke.
CellEdit dabs(const Canvas& c, const Brush& b, int size, const std::vector<Point>& at, const StrokeOptions& o = {}) {
    std::vector<Dab> ds;
    for (const auto& [x, y] : at)
        ds.push_back({x, y});
    BrushSettings s;
    s.size = size;
    return paintStroke(c, b, s, ds, o);
}

} // namespace

TEST_CASE("the outline repairs only next to the cells the stroke writes", "[detail][lint]") {
    // PR #50 round 3 (Codex 2, Claude P2-1): a dab over the interior writes
    // nothing, so it repairs nothing
    SECTION("Codex: a size-1 dab inside a fade") {
        Canvas c(9, 7);
        for (int y = 1; y <= 5; ++y)
            for (int x = 1; x <= 7; ++x)
                c.at(x, y) = rungCell(steel(), std::min(4, x + std::abs(y - 3)));
        CHECK(dabs(c, OutlineBrush(steel()), 1, {{2, 3}}).empty());
        StrokeOptions o;
        o.selection = std::make_shared<Mask>(Mask::rect(9, 7, 2, 3, 2, 3));
        CHECK(dabs(c, OutlineBrush(steel()), 3, {{2, 3}}, o).empty());
    }
    SECTION("Claude: size-1 dabs on the blob's repair cells, and a selection without the rim") {
        Canvas c(80, 25);
        const RampShadeBrush lighten(steel(), ShadeDirection::Lighten);
        for (int size = 24; size >= 3; size -= 3) {
            BrushSettings s;
            s.size = size;
            s.tip = Tip::Round;
            paintStroke(c, lighten, s, {{40, 12}, {41, 12}}).apply(c);
        }
        const std::set<Point> edge = rim(c, steel());
        std::vector<Point> repairs;
        for (const CellChange& ch : outlineAll(c, steel()).changes)
            if (!edge.count({ch.x, ch.y}))
                repairs.push_back({ch.x, ch.y});
        REQUIRE(!repairs.empty());
        for (const auto& p : repairs) {
            INFO("dab at " << p.first << "," << p.second);
            CHECK(dabs(c, OutlineBrush(steel()), 1, {p}).empty());
        }
        auto inner = std::make_shared<Mask>(Mask::rect(80, 25, 0, 0, 79, 24));
        for (const auto& [x, y] : edge)
            inner->set(x, y, false);
        StrokeOptions o;
        o.selection = inner;
        CHECK(outlineAll(c, steel(), o).empty());
    }
}

TEST_CASE("a second outline over a fringe changes nothing", "[detail]") {
    // PR #50 round 3 (Codex 3): a w block with a w/black fringe on its edge
    Canvas c(7, 7);
    for (int y = 1; y <= 5; ++y)
        for (int x = 1; x <= 5; ++x)
            c.at(x, y) = rungCell(steel(), 12);
    c.at(2, 3) = Cell{kUpperHalf, Color::indexed(White), Color::indexed(Black)};
    for (const bool on : {true, false}) {
        StrokeOptions o;
        o.ice = on;
        INFO("iCE " << on);
        CHECK(settled(c, steel(), o));
    }
}

TEST_CASE("the outline takes the darkest line the lint accepts", "[detail][lint]") {
    // PR #50 round 3 (Codex 4): a falling gradient, 11 at the left to 6 at
    // the right. The left end takes the line (K, below w). At the right end
    // any line under 7 (K or K-and-a-shade) is a skip, so the brush keeps
    // the 6 there: it takes the darkest line that leaves the ladder clean
    Canvas c(9, 7);
    for (int y = 1; y <= 5; ++y)
        for (int x = 1; x <= 7; ++x)
            c.at(x, y) = rungCell(steel(), 13 - x);
    const Canvas out = applied(c, outlineAll(c, steel()));
    CHECK(added(c, out, steel()).empty());
    for (int y = 1; y <= 5; ++y)
        CHECK(out.at(1, y) == rungCell(steel(), 4));
    for (int y = 2; y <= 4; ++y)
        CHECK(out.at(7, y) == c.at(7, y));
    CHECK(settled(c, steel()));
}

TEST_CASE("thin parts take the line along their whole length", "[detail][lint]") {
    // PR #50 round 3 (Claude P3-1): a body with an antenna one cell wide and
    // a leg two wide. Each part's line comes from the nearest interior, the
    // body's, however far along the part
    Canvas c(30, 20);
    for (int y = 6; y <= 13; ++y)
        for (int x = 8; x <= 18; ++x)
            c.at(x, y) = rungCell(steel(), 12);
    for (int y = 0; y <= 5; ++y)
        c.at(13, y) = rungCell(steel(), 12);
    for (int y = 14; y <= 19; ++y)
        for (int x = 10; x <= 11; ++x)
            c.at(x, y) = rungCell(steel(), 12);
    const Canvas out = applied(c, outlineAll(c, steel()));
    for (int y = 0; y <= 5; ++y)
        CHECK(rungOf(steel(), out.at(13, y)) == 8);
    for (int y = 14; y <= 19; ++y)
        for (int x = 10; x <= 11; ++x)
            CHECK(rungOf(steel(), out.at(x, y)) == 8);
    CHECK(rungOf(steel(), out.at(8, 9)) == 8);
    CHECK(rungOf(steel(), out.at(13, 9)) == 12);
    CHECK(added(c, out, steel()).empty());
    CHECK(settled(c, steel()));
}

namespace {

// A canvas from rows of base-36 rungs ('.' black, '#' an RGB cell), the
// first row at y0, each from x = 0.
Canvas rows36(int w, int h, int y0, const std::vector<std::string>& rows, const Ramp& r) {
    Canvas c(w, h);
    for (size_t j = 0; j < rows.size(); ++j)
        for (size_t i = 0; i < rows[j].size(); ++i) {
            const char ch = rows[j][i];
            Cell& cell = c.at(int(i), y0 + int(j));
            if (ch == '#') {
                cell.ch = U'█';
                cell.fg = Color::fromRgb(Rgb{200, 30, 40});
            } else if (ch != '.') {
                cell = rungCell(r, ch <= '9' ? ch - '0' : ch - 'a' + 10);
            }
        }
    return c;
}

// The line an edge cell would take: the outline colour of the darkest shape
// cell off the rim among its four neighbours, else its eight, else the
// darkest colour above black.
std::optional<int> lineOf(const Canvas& c, const Ramp& r, const std::set<Point>& edge, int x, int y) {
    for (const bool diagonal : {false, true}) {
        int best = INT_MAX;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if ((dx || dy) && (diagonal || !dx || !dy) && c.contains(x + dx, y + dy) &&
                    inShape(r, c.at(x + dx, y + dy)) && !edge.count({x + dx, y + dy}))
                    if (const auto g = rungOf(r, c.at(x + dx, y + dy)))
                        best = std::min(best, *g);
        if (best != INT_MAX)
            return lint::outlineRung(r, best).value_or(kRungsPerStep);
    }
    return std::nullopt;
}

// The line cells an outline leaves on their own: runs of fewer than three
// rim cells it took to the line (or darker), with no line cell beside them
// on the rim that was there before, next to a rim cell left off the line.
std::set<Point> fragments(const Canvas& before, const Canvas& after, const Ramp& r) {
    const std::set<Point> edge = rim(before, r);
    std::set<Point> line, open;
    for (const auto& [x, y] : edge) {
        const auto l = lineOf(before, r, edge, x, y);
        const auto g = rungOf(r, after.at(x, y));
        if (l && g && *g <= *l)
            line.insert({x, y});
        else if (l)
            open.insert({x, y});
    }
    std::set<Point> out, seen;
    for (const Point& p : line) {
        if (seen.count(p))
            continue;
        std::vector<Point> part{p}, work{p};
        seen.insert(p);
        bool written = true, broken = false;
        while (!work.empty()) {
            const auto [x, y] = work.back();
            work.pop_back();
            written = written && !(after.at(x, y) == before.at(x, y));
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const Point n{x + dx, y + dy};
                    broken = broken || open.count(n);
                    if (line.count(n) && !seen.count(n)) {
                        seen.insert(n);
                        part.push_back(n);
                        work.push_back(n);
                    }
                }
        }
        if (part.size() < 3 && written && broken)
            out.insert(part.begin(), part.end());
    }
    return out;
}

// A stroke, then the same stroke again over its result: the second must be
// empty, and the first must add no finding.
void repeatable(const Canvas& c, const Ramp& r, const BrushSettings& s, const std::vector<Dab>& at,
                const StrokeOptions& o) {
    const CellEdit first = paintStroke(c, OutlineBrush(r), s, at, o);
    const Canvas out = applied(c, first);
    CHECK(added(c, out, r).empty());
    CHECK(paintStroke(out, OutlineBrush(r), s, at, o).empty());
}

} // namespace

TEST_CASE("a partial outline adds no finding and is the same stroke again", "[detail][lint]") {
    // PR #50 round 4 (Codex 2): a shaded triangle and one size-1 dab inside
    // its rim, on four ramps; and the same through a selection of one cell
    for (const char* name : {"fire", "ice", "sunset", "toxic"}) {
        INFO(name);
        const Ramp r = *findRamp(name);
        Canvas c(11, 11);
        for (int y = 1; y <= 9; ++y)
            for (int x = 0; x <= std::min(y + 1, 8); ++x)
                c.at(x, y) = rungCell(r, std::min(16, 23 - x - y));
        REQUIRE(lint::checkLadder(c, r).clean());
        StrokeOptions o;
        o.ice = true;
        BrushSettings s;
        repeatable(c, r, s, {{6, 6}}, o);
        StrokeOptions one = o;
        auto m = std::make_shared<Mask>(c.width(), c.height());
        m->set(6, 6);
        one.selection = m;
        const Canvas out = applied(c, outlineAll(c, r, one));
        CHECK(added(c, out, r).empty());
        CHECK(outlineAll(out, r, one).empty());
    }
    // Claude round 4 P2-2: a sunset bar, a dab at its foot
    const Ramp sunset = *findRamp("sunset");
    const Canvas bar = rows36(16, 14, 9,
                              {".........kkjjii.", ".........kkjjii.", ".........kkjjii.", "............jii."}, sunset);
    REQUIRE(lint::checkLadder(bar, sunset).clean());
    for (const Tip tip : {Tip::Square, Tip::Round}) {
        BrushSettings s;
        s.tip = tip;
        repeatable(bar, sunset, s, {{12, 11}}, {});
        s.size = 2;
        repeatable(bar, sunset, s, {{11, 11}}, {});
        repeatable(bar, sunset, s, {{12, 11}}, {});
    }
}

TEST_CASE("the outline is its own fixed point on a piece that isn't lint clean", "[detail][lint]") {
    // PR #50 round 4 (Claude P2-1): wood with skips at the top of the 8
    // block and an RGB cell in it. A second outline changes nothing, the
    // first adds nothing, and the bottom line isn't dotted
    const Ramp wood = *findRamp("wood");
    const Canvas c = rows36(12, 10, 1,
                            {".aaabbbccc.", ".aaabbbccc.", ".aaabbbccc.", ".aaabbbccc.", ".aaab88888.", ".aaab888#8.",
                             ".aaab88888.", ".aaab77777."},
                            wood);
    REQUIRE_FALSE(lint::checkLadder(c, wood).clean());
    StrokeOptions o;
    o.ice = true;
    const Canvas one = applied(c, outlineAll(c, wood, o));
    CHECK(outlineAll(one, wood, o).empty());
    CHECK(added(c, one, wood).empty());
    CHECK(fragments(c, one, wood).empty());
}

TEST_CASE("the outline leaves no lone line cells", "[detail][lint]") {
    // PR #50 round 4 (Codex 3, Claude P3-2): where a side stays unlined
    // because any line would skip, the corner cells beside it stay too
    Canvas x13(9, 7);
    for (int y = 1; y <= 5; ++y)
        for (int x = 1; x <= 7; ++x)
            x13.at(x, y) = rungCell(steel(), 13 - x);
    const Canvas out = applied(x13, outlineAll(x13, steel()));
    CHECK(fragments(x13, out, steel()).empty());
    CHECK(out.at(7, 1) == x13.at(7, 1));
    CHECK(out.at(7, 5) == x13.at(7, 5));
    CHECK(added(x13, out, steel()).empty());
    // a falling gradient, 12 down to 6 in rungs three cells wide
    Canvas fall(28, 14);
    for (int y = 2; y <= 11; ++y)
        for (int x = 1; x <= 26; ++x)
            fall.at(x, y) = rungCell(steel(), x <= 5 ? 12 : std::max(6, 11 - (x - 6) / 3));
    REQUIRE(lint::checkLadder(fall, steel()).clean());
    const Canvas lined = applied(fall, outlineAll(fall, steel()));
    CHECK(fragments(fall, lined, steel()).empty());
    CHECK(lined.at(26, 2) == fall.at(26, 2));
    CHECK(lined.at(26, 11) == fall.at(26, 11));
    CHECK(added(fall, lined, steel()).empty());
    CHECK(settled(fall, steel()));
}

namespace {

Ramp fire() { return *findRamp("fire"); }

// Brush-outline review (P2-1): a wobbly steel ellipse shaded round a light
// up and to the left, and the path a hand takes round its edge: from its
// leftmost edge cell to the nearest edge cell not yet visited, and so on.
Canvas wobbly() {
    Canvas c(160, 200);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 160; ++x) {
            const double ex = (x - 80) / 67.2, ey = (y - 100) / 84.0;
            if (ex * ex + ey * ey + 0.08 * std::sin(0.4 * x) * std::cos(0.3 * y) > 1)
                continue;
            const double d = std::sqrt((x - 70.0) * (x - 70.0) + 4 * (y - 92.0) * (y - 92.0));
            c.at(x, y) = rungCell(steel(), std::clamp(12 - int(d / 2), 6, 12));
        }
    return c;
}

std::vector<Point> edgePath(const Canvas& c, const Ramp& r) {
    std::vector<Point> left;
    for (int x = 0; x < c.width(); ++x)
        for (int y = 0; y < c.height(); ++y)
            if (onEdge(c, r, x, y))
                left.push_back({x, y});
    std::vector<Point> path;
    Point at = left.front();
    left.erase(left.begin());
    path.push_back(at);
    while (!left.empty()) {
        auto best = left.begin();
        for (auto it = left.begin(); it != left.end(); ++it) {
            const auto dist = [&](const Point& p) {
                return (p.first - at.first) * (p.first - at.first) + (p.second - at.second) * (p.second - at.second);
            };
            if (dist(*it) < dist(*best))
                best = it;
        }
        at = *best;
        left.erase(best);
        path.push_back(at);
    }
    return path;
}

// Brush-outline review (P2-3, P3-1): a fire ellipse, 30 x 16 cells about
// (cx, 9.5), shaded round a light up and to the left of its centre.
void fireEllipse(Canvas& c, double cx) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const double ex = (x - cx) / 15, ey = (y - 9.5) / 8;
            if (ex * ex + ey * ey > 1)
                continue;
            const double d = std::sqrt((x - (cx - 4.5)) * (x - (cx - 4.5)) + 4 * (y - 6.0) * (y - 6.0));
            c.at(x, y) = rungCell(fire(), std::clamp(16 - int(d / 2), 6, 16));
        }
}

// The cells an edit writes with no other written cell among their eight
// neighbours.
std::set<Point> lone(const CellEdit& e) {
    const auto w = written(e);
    std::set<Point> out;
    for (const auto& [p, cell] : w) {
        bool beside = false;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                beside = beside || ((dx || dy) && w.count({p.first + dx, p.second + dy}));
        if (!beside)
            out.insert(p);
    }
    return out;
}

} // namespace

TEST_CASE("a long outline stroke keeps the line it has drawn", "[detail][lint]") {
    // brush-outline review (P2-1): the line followed the stroke round the
    // shape and then collapsed, 131 cells down to 16 at dab 128
    const Canvas c = wobbly();
    const auto path = edgePath(c, steel());
    REQUIRE(path.size() > 200);
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Square;
    const OutlineBrush brush(steel());
    Stroke stroke(c, brush, s);
    size_t most = 0;
    for (size_t i = 0; i < 200; ++i) {
        stroke.addDab({path[i].first, path[i].second});
        const size_t now = stroke.edit().changes.size();
        most = std::max(most, now);
        INFO("dab " << i);
        REQUIRE(now * 10 >= most * 9);
    }
    CHECK(most > 100);
}

TEST_CASE("an outline dab costs the same however long the stroke", "[detail]") {
    // brush-outline review (P2-2): each dab settled the whole stroke again,
    // 31.5 ms a dab a hundred dabs in. The cells read per dab, along the top
    // edge of a long fire strip, at 10 and 1000 dabs.
    Canvas c(1100, 40);
    for (int y = 1; y < 40; ++y)
        for (int x = 0; x < 1100; ++x)
            c.at(x, y) = rungCell(fire(), y == 1 ? (x == 1099 ? 16 : 15) : 8);
    for (int y = 20; y <= 21; ++y)
        for (int x = 1050; x <= 1051; ++x)
            c.at(x, y) = rungCell(fire(), 14);
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Square;
    const OutlineBrush brush(fire());
    // after the first dab, which copies the canvas and tallies its lint
    // once a stroke (the counter takes in the lint's reads and the copies;
    // how a dab's work goes with the canvas is the test further down)
    const auto perDab = [&](int n) {
        Stroke stroke(c, brush, s);
        stroke.addDab({0, 1});
        const long long before = detailCellsRead();
        for (int x = 1; x < n; ++x)
            stroke.addDab({x, 1});
        return double(detailCellsRead() - before) / (n - 1);
    };
    const double ten = perDab(10), thousand = perDab(1000);
    INFO("cells read a dab: " << ten << " at 10 dabs, " << thousand << " at 1000");
    CHECK(thousand <= 2 * ten);
}

TEST_CASE("one outline pass over the whole is the passes over its parts", "[detail][lint]") {
    // brush-outline review (P2-3): two shapes either side of a seam, the
    // outline not crossing it. One pass over the whole canvas writes what
    // a pass over each half does, the second on the first's result.
    Canvas c(80, 20);
    fireEllipse(c, 19.5);
    fireEllipse(c, 59.5);
    REQUIRE(lint::checkLadder(c, fire()).clean());
    StrokeOptions o;
    const CellEdit whole = outlineAll(c, fire(), o);
    o.selection = std::make_shared<Mask>(Mask::rect(80, 20, 0, 0, 39, 19));
    const CellEdit left = outlineAll(c, fire(), o);
    const Canvas half = applied(c, left);
    o.selection = std::make_shared<Mask>(Mask::rect(80, 20, 40, 0, 79, 19));
    const CellEdit right = outlineAll(half, fire(), o);
    auto parts = written(left);
    for (const auto& [p, cell] : written(right))
        parts[p] = cell;
    CHECK(written(whole) == parts);
    CHECK_FALSE(written(left).empty());
    CHECK_FALSE(written(right).empty());
}

TEST_CASE("an outline stroke leaves no one-cell fragments", "[detail][lint]") {
    // brush-outline review (P3-1): a stroke along the fire ellipse's lower
    // left edge wrote only (10,15), a lone cell of line
    Canvas c(40, 20);
    fireEllipse(c, 19.5);
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Square;
    const OutlineBrush brush(fire());
    const CellEdit e = paintStroke(c, brush, s, {{7, 12}, {7, 13}, {8, 13}, {8, 14}, {9, 14}, {9, 15}});
    // the one cell was all the line the lint let those cells take, so the
    // stroke writes nothing
    CHECK(lone(e).empty());
    // nor any short stroke over it, of any size, a click included
    std::mt19937 g(1010);
    const auto uni = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); };
    for (int i = 0; i < 150; ++i) {
        s.size = uni(1, 5);
        s.tip = uni(0, 1) ? Tip::Round : Tip::Square;
        std::vector<Dab> dabs;
        for (int n = uni(1, 4); n > 0; --n)
            dabs.push_back({uni(0, 39), uni(0, 19)});
        const CellEdit r = paintStroke(c, brush, s, dabs);
        INFO("stroke " << i);
        CHECK(lone(r).empty());
    }
}

namespace {

// A fire ellipse shaded in rings about its centre cell (30,15): the same
// under either mirror and the half turn.
Canvas rings() {
    Canvas c(61, 31);
    for (int y = 0; y < 31; ++y)
        for (int x = 0; x < 61; ++x) {
            const double ex = (x - 30) / 25.0, ey = (y - 15) / 12.0;
            if (ex * ex + ey * ey > 1)
                continue;
            const double d = std::sqrt((x - 30) * (x - 30) + 4 * (y - 15) * (y - 15));
            c.at(x, y) = rungCell(fire(), std::clamp(16 - int(d / 2), 6, 16));
        }
    return c;
}

// A stroke along the ellipse's upper left edge.
std::vector<Dab> upperLeft(const Canvas& c) {
    std::vector<Dab> out;
    for (int x = 4; x < 26; ++x)
        for (int y = 0; y < 13; ++y)
            if (inShape(fire(), c.at(x, y))) {
                out.push_back({x, y});
                break;
            }
    return out;
}

} // namespace

TEST_CASE("an outline stroke draws every symmetry image of its line", "[detail][lint]") {
    // PLAN §10: symmetry is in the core stroke. Each image of the stroke
    // writes what a stroke along that image alone does: the line the
    // canvas there takes. (The lint reads rows left to right, so the line
    // a mirror image takes need not be the mirror of the stroke's.)
    const Canvas c = rings();
    REQUIRE(lint::checkLadder(c, fire()).clean());
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Square;
    const OutlineBrush brush(fire());
    const std::vector<Dab> path = upperLeft(c);
    for (const Symmetry sym : {Symmetry{SymmetryMode::MirrorX, 4, Point{30, 15}},
                               Symmetry{SymmetryMode::MirrorY, 4, Point{30, 15}},
                               Symmetry{SymmetryMode::MirrorXY, 4, Point{30, 15}},
                               Symmetry{SymmetryMode::Radial, 2, Point{30, 15}}}) {
        StrokeOptions o;
        o.symmetry = sym;
        const CellEdit e = paintStroke(c, brush, s, path, o);
        INFO("mode " << int(sym.mode));
        const size_t images = symmetryDabs(sym, path.front(), 61, 31).size();
        std::map<Point, Cell> each;
        for (size_t k = 0; k < images; ++k) {
            std::vector<Dab> image;
            for (const Dab& d : path)
                image.push_back(symmetryDabs(sym, d, 61, 31)[k]);
            const auto w = written(paintStroke(c, brush, s, image));
            CHECK(w.size() >= 20);
            each.insert(w.begin(), w.end());
        }
        CHECK(written(e) == each);
        CHECK(lint::checkLadder(applied(c, e), fire()).clean());
    }
}

TEST_CASE("an outline stroke changes a cell once, from its stroke-start value", "[detail][lint]") {
    // PLAN §10: going back over the line it has drawn, in the same stroke,
    // reads the canvas as the stroke found it and changes nothing more
    const Canvas c = rings();
    BrushSettings s;
    s.size = 3;
    s.tip = Tip::Square;
    const OutlineBrush brush(fire());
    // the path 4-connected, so back covers the cells there did and no more
    // (a join between two dabs walks another way back)
    std::vector<Dab> there;
    for (const Dab& d : upperLeft(c)) {
        if (!there.empty())
            for (int y = there.back().y; y != d.y; y += d.y > y ? 1 : -1)
                there.push_back({there.back().x, y + (d.y > y ? 1 : -1)});
        there.push_back(d);
    }
    std::vector<Dab> back = there;
    back.insert(back.end(), there.rbegin(), there.rend());
    const CellEdit once = paintStroke(c, brush, s, there);
    REQUIRE_FALSE(once.empty());
    CHECK(written(paintStroke(c, brush, s, back)) == written(once));
    // back along the path as it was, whose joins cover more cells, keeps
    // the line there drew
    std::vector<Dab> loose = upperLeft(c), looseBack = loose;
    looseBack.insert(looseBack.end(), loose.rbegin(), loose.rend());
    const auto wider = written(paintStroke(c, brush, s, looseBack));
    for (const auto& [p, cell] : written(paintStroke(c, brush, s, loose))) {
        REQUIRE(wider.count(p));
        CHECK(wider.at(p) == cell);
    }
    for (const CellChange& ch : once.changes)
        CHECK(ch.before == c.at(ch.x, ch.y));
    // and the same stroke on the same canvas is the same edit
    CHECK(written(paintStroke(c, brush, s, there)) == written(once));
}

namespace {

// Brush-outline round 1 (Claude P2-1): a wobbly steel ellipse filling about
// three quarters of a w x h canvas, shaded round a light up and to the left
// of its centre. At 160 x 100 it is the review's canvas.
Canvas shadedEllipse(int w, int h) {
    Canvas c(w, h);
    const double ax = 0.375 * w, ay = 0.4 * h, lx = w / 2.0 - w / 16.0, ly = h / 2.0 - 0.08 * h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double ex = (x - w / 2) / ax, ey = (y - h / 2) / ay;
            if (ex * ex + ey * ey + 0.08 * std::sin(0.4 * x) * std::cos(0.3 * y) > 1)
                continue;
            const double d = std::sqrt((x - lx) * (x - lx) + 4 * (y - ly) * (y - ly));
            c.at(x, y) = rungCell(steel(), std::clamp(12 - int(d / 2), 6, 12));
        }
    return c;
}

BrushSettings square(int size) {
    BrushSettings s;
    s.size = size;
    s.tip = Tip::Square;
    return s;
}

// The cells written after each dab of a stroke: none of them may change
// again or go back, dab to dab.
void keepsItsLine(const Canvas& c, const Brush& brush, const BrushSettings& s, const std::vector<Dab>& dabs,
                  const StrokeOptions& o = {}) {
    Stroke stroke(c, brush, s, o);
    std::map<Point, Cell> was;
    for (size_t i = 0; i < dabs.size(); ++i) {
        stroke.addDab(dabs[i]);
        const auto now = written(stroke.edit());
        INFO("dab " << i << " at " << dabs[i].x << "," << dabs[i].y);
        for (const auto& [p, cell] : was) {
            INFO("cell " << p.first << "," << p.second);
            REQUIRE(now.count(p));
            CHECK(now.at(p) == cell);
        }
        was = now;
    }
}

// Codex round 1 (#1): a fire slope, rows from (0,0).
Canvas slope() {
    return rows36(11, 9, 0, {"a9", "a99", "a998", "98877", "988776", "9887766", "87766555"}, fire());
}

} // namespace

TEST_CASE("an outline dab keeps the line an earlier dab of the stroke drew", "[detail][lint]") {
    // Codex round 1 (#1): the second dab took back the first one's line
    const Canvas c = slope();
    const OutlineBrush brush(fire());
    Stroke stroke(c, brush, square(3));
    stroke.addDab({5, 4});
    const auto first = written(stroke.edit());
    for (const Point p : {Point{4, 3}, Point{4, 4}}) {
        REQUIRE(first.count(p));
        CHECK(first.at(p) == rungCell(fire(), 6));
    }
    stroke.addDab({4, 4});
    const auto both = written(stroke.edit());
    for (const auto& [p, cell] : first) {
        REQUIRE(both.count(p));
        CHECK(both.at(p) == cell);
    }
    CHECK(added(c, applied(c, stroke.edit()), fire()).empty());
    // a 4-connected path over the same cells, either way round
    std::vector<Dab> path{{6, 8}, {6, 7}, {5, 7}, {5, 6}, {5, 5}, {5, 4}, {4, 4}, {4, 3}, {4, 2}};
    for (int way = 0; way < 2; ++way) {
        INFO("way " << way);
        keepsItsLine(c, brush, square(3), path);
        const CellEdit e = paintStroke(c, brush, square(3), path);
        CHECK(added(c, applied(c, e), fire()).empty());
        // either way it draws a line, and the cell both dabs above line
        // (which neighbours it takes with it depends on the order: a cell
        // the stroke has written stays)
        const auto w = written(e);
        REQUIRE(w.count({4, 4}));
        CHECK(w.at({4, 4}) == rungCell(fire(), 6));
        std::reverse(path.begin(), path.end());
    }
}

TEST_CASE("an outline stroke's dabs again in the same stroke change nothing", "[detail][lint]") {
    // Codex round 1 (#1): random strokes on random shaded shapes; the
    // stroke keeps every cell it has written, dab to dab, and its dabs
    // again, in the same stroke, leave the edit as it was
    std::mt19937 g(5501);
    const auto uni = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); };
    for (int i = 0; i < 60; ++i) {
        Canvas c(40, 20);
        const Ramp r = uni(0, 1) ? fire() : steel();
        const int top = int(r.colours.size()) * kRungsPerStep - 1;
        for (int n = uni(1, 3); n > 0; --n) {
            const int cx = uni(5, 34), cy = uni(4, 15), ax = uni(3, 12), ay = uni(2, 6);
            for (int y = 0; y < 20; ++y)
                for (int x = 0; x < 40; ++x) {
                    const double ex = double(x - cx) / ax, ey = double(y - cy) / ay;
                    if (ex * ex + ey * ey > 1)
                        continue;
                    const double d = std::sqrt((x - cx + 2) * (x - cx + 2) + 4 * (y - cy + 1) * (y - cy + 1));
                    c.at(x, y) = rungCell(r, std::clamp(top - int(d / 2), 6, top));
                }
        }
        BrushSettings s = square(uni(1, 5));
        s.tip = uni(0, 1) ? Tip::Round : Tip::Square;
        std::vector<Dab> dabs;
        for (int n = uni(1, 12); n > 0; --n)
            dabs.push_back({uni(0, 39), uni(0, 19)});
        INFO("stroke " << i);
        keepsItsLine(c, OutlineBrush(r), s, dabs);
        const OutlineBrush brush(r);
        Stroke stroke(c, brush, s);
        for (const Dab& d : dabs)
            stroke.addDab(d);
        const auto once = written(stroke.edit());
        for (size_t j = 0; j < dabs.size(); ++j)
            stroke.addDab(dabs[j], j > 0);
        CHECK(written(stroke.edit()) == once);
        CHECK(added(c, applied(c, stroke.edit()), r).empty());
    }
}

namespace {
// The cells an edit changes, as (x, y) pairs.
std::vector<std::pair<int, int>> cellsOf(const CellEdit& e) {
    std::vector<std::pair<int, int>> out;
    for (const auto& [p, cell] : written(e))
        out.push_back(p);
    return out;
}
// What an edit adds as a brush's guard counts it (lint::LadderIndex), and as
// the whole-canvas lint compared by kind and cell.
bool addsNothing(const Canvas& c, const CellEdit& e, const Ramp& r) {
    const Canvas after = applied(c, e);
    return lint::addedFindingsOnce(c, after, r, cellsOf(e)).empty() && added(c, after, r).empty();
}
// A shaded ellipse or two on black, lint clean or not.
Canvas randomShaded(std::mt19937& g, int w, int h, const Ramp& r) {
    const auto uni = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); };
    Canvas c(w, h);
    const int top = r.topRung();
    for (int n = uni(1, 2); n > 0; --n) {
        const int cx = uni(w / 4, 3 * w / 4), cy = uni(h / 4, 3 * h / 4), ax = uni(4, w / 2), ay = uni(3, h / 2);
        const int lx = uni(-3, 3), ly = uni(-2, 2);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double ex = double(x - cx) / ax, ey = double(y - cy) / ay;
                if (ex * ex + ey * ey > 1)
                    continue;
                const double d = std::sqrt((x - cx - lx) * (x - cx - lx) + 4.0 * (y - cy - ly) * (y - cy - ly));
                c.at(x, y) = rungCell(r, std::clamp(top - int(d / 2), 6, top));
            }
    }
    return c;
}
// The canvas with its left half copied mirrored onto its right half.
Canvas mirroredX(Canvas c) {
    const int w = c.width();
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < w / 2; ++x)
            c.at(w - 1 - x, y) = c.at(x, y);
    return c;
}
} // namespace

TEST_CASE("an outline image writes what a stroke along it alone does", "[detail][lint]") {
    // Codex round 1 (#3): two clicks a cell apart write nothing, each a
    // lone fragment; a mirror click made them settle together
    Canvas c(10, 8);
    for (int y = 2; y <= 5; ++y)
        for (int x = 2; x <= 7; ++x)
            c.at(x, y) = rungCell(steel(), 8);
    const OutlineBrush brush(steel());
    CHECK(paintStroke(c, brush, square(1), {{4, 2}}).empty());
    CHECK(paintStroke(c, brush, square(1), {{5, 2}}).empty());
    StrokeOptions o;
    o.symmetry = Symmetry{SymmetryMode::MirrorX};
    CHECK(paintStroke(c, brush, square(1), {{4, 2}}, o).empty());
    // Random strokes whose images overlap. The composite is judged as a
    // whole (Claude round 2, N1), so it is the union of each image's own
    // edit only where that union is itself lint clean and no two images
    // write one cell; everywhere it adds no finding.
    std::mt19937 g(5503);
    const auto uni = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); };
    int unions = 0, same = 0;
    for (int i = 0; i < 60; ++i) {
        const int w = uni(12, 24), h = uni(8, 14);
        Canvas r(w, h);
        const int x0 = uni(0, w / 2 - 1), y0 = uni(0, h / 2 - 1), x1 = uni(w / 2, w - 1), y1 = uni(h / 2, h - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                r.at(x, y) = rungCell(steel(), std::clamp(6 + uni(0, 1) + (x + y) % 6, 6, 12));
        const Symmetry sym{std::array{SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY}[uni(0, 2)]};
        BrushSettings s = square(uni(1, 4) * 2 - 1);
        std::vector<Dab> dabs;
        for (int n = uni(1, 6); n > 0; --n)
            dabs.push_back({uni(w / 2 - 3, w / 2 + 2), uni(h / 2 - 3, h / 2 + 2)});
        StrokeOptions so;
        so.symmetry = sym;
        const CellEdit e = paintStroke(r, brush, s, dabs, so);
        INFO("stroke " << i << " mode " << int(sym.mode));
        CHECK(lint::addedFindingsOnce(r, applied(r, e), steel(), cellsOf(e)).empty());
        const size_t images = symmetryDabs(sym, dabs.front(), w, h).size();
        std::map<Point, Cell> each;
        bool overlap = false;
        for (size_t k = 0; k < images; ++k) {
            std::vector<Dab> image;
            for (const Dab& d : dabs)
                image.push_back(symmetryDabs(sym, d, w, h)[k]);
            for (const auto& [p, cell] : written(paintStroke(r, brush, s, image)))
                overlap = !each.emplace(p, cell).second || overlap;
        }
        Canvas u = r;
        std::vector<std::pair<int, int>> at;
        for (const auto& [p, cell] : each) {
            u.at(p.first, p.second) = cell;
            at.push_back(p);
        }
        if (overlap || !lint::addedFindingsOnce(r, u, steel(), at).empty())
            continue;
        ++unions;
        same += written(e) == each;
        CHECK(written(e) == each);
    }
    CHECK(unions >= 10);
}

TEST_CASE("an outline symmetry stroke adds no finding where its images meet", "[detail][lint]") {
    // Claude round 2 (N1): images settled apart, each clean, met in a rung
    // skip at (5,6) ("rung 8 next to rung 4 at cell 6,6")
    {
        const Canvas c = rows36(18, 14, 5,
                                {".....8......8.....", "788999999999999887", "8899aaaaaaaaaa9988",
                                 "899aabbbbbbbbaa998", ".....b......b....."},
                                steel());
        REQUIRE(lint::checkLadder(c, steel()).findings.empty());
        BrushSettings s = square(4);
        s.tip = Tip::Round;
        StrokeOptions o;
        o.symmetry = Symmetry{SymmetryMode::MirrorY};
        const CellEdit e = paintStroke(c, OutlineBrush(steel()), s, {{5, 5}, {8, 8}, {7, 2}, {14, 3}}, o);
        CHECK_FALSE(e.empty());
        CHECK(addsNothing(c, e, steel()));
    }
    // Codex round 2 (#1): each image removed its own ▒ cells and kept the
    // other's, so only together did they leave a far rung-5 field the one
    // shade glyph
    {
        Canvas c(70, 65);
        for (int y = 2; y <= 6; ++y)
            for (int x = 2; x <= 6; ++x)
                c.at(x, y) = rungCell(fire(), 5);
        for (const int x0 : {10, 55})
            for (int y = 40; y <= 44; ++y)
                for (int x = x0; x <= x0 + 4; ++x)
                    c.at(x, y) = rungCell(fire(), x > x0 && x < x0 + 4 && y == 40 ? 6 : 8);
        REQUIRE(lint::checkLadder(c, fire()).findings.empty());
        StrokeOptions o;
        o.symmetry = Symmetry{SymmetryMode::MirrorX};
        const CellEdit e = paintStroke(c, OutlineBrush(fire()), square(3), {{12, 40}}, o);
        CHECK(addsNothing(c, e, fire()));
        CHECK_FALSE(e.empty());
        // Codex round 3 (#1): refusing both images together wrote nothing
        // where a symmetric subset is clean. The composite settles first,
        // putting (11,40) and its mirror (58,40) back: 16 cells.
        const CellEdit five = paintStroke(c, OutlineBrush(fire()), square(5), {{12, 40}}, o);
        CHECK(addsNothing(c, five, fire()));
        const auto w = written(five);
        CHECK(w.size() == 16);
        for (const auto& [p, cell] : w) {
            INFO("cell " << p.first << "," << p.second);
            CHECK(w.count({69 - p.first, p.second}));
        }
    }
    // and random mirror strokes on shaded shapes: none adds a finding as
    // the guard counts them, nor on a lint-clean canvas as the whole lint
    // compared by kind and cell does
    std::mt19937 g(5528);
    const auto uni = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); };
    int clean = 0, wrote = 0;
    for (int i = 0; i < 400; ++i) {
        const Ramp r = uni(0, 1) ? fire() : steel();
        const int w = uni(16, 50), h = uni(10, 26);
        const Canvas c = randomShaded(g, w, h, r);
        const bool isClean = lint::checkLadder(c, r).findings.empty();
        const Symmetry sym{std::array{SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY}[uni(0, 2)]};
        BrushSettings s = square(uni(0, 6) ? uni(1, 6) : kMaxBrushSize);
        s.tip = uni(0, 1) ? Tip::Round : Tip::Square;
        std::vector<Dab> dabs;
        for (int n = uni(1, 6); n > 0; --n)
            dabs.push_back({uni(0, w - 1), uni(0, h - 1)});
        StrokeOptions o;
        o.symmetry = sym;
        const CellEdit e = paintStroke(c, OutlineBrush(r), s, dabs, o);
        INFO("stroke " << i);
        CHECK(lint::addedFindingsOnce(c, applied(c, e), r, cellsOf(e)).empty());
        if (isClean)
            CHECK(added(c, applied(c, e), r).empty());
        clean += isClean;
        wrote += !e.empty();
    }
    CHECK(clean > 150);
    CHECK(wrote > 250);
}

TEST_CASE("a mirror stroke on a mirrored canvas draws a mirrored line", "[detail][lint]") {
    // Claude round 2 (N2): the lower image won ties on both sides of the
    // axis, so (6,4)=6 but (11,4)=7, and (6,6)=6 but (11,6)=5
    const auto symmetric = [](const Canvas& a) {
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
                if (!(a.at(x, y) == a.at(a.width() - 1 - x, y)))
                    return false;
        return true;
    };
    {
        const Canvas c = rows36(18, 16, 4,
                                {"......bbbbbb......", ".....aaaaaaaa.....", "....6.999999.6....", "..66666....66666..",
                                 ".7777777..7777777.", ".7888888..8888887.", ".8999999..9999998.", "899aaaaaaaaaaaa998",
                                 ".9aabbbb..bbbbaa9.", ".9aabbcc..ccbbaa9.", ".9aabbbb..bbbbaa9.", "..9aaaa....aaaa9.."},
                                steel());
        REQUIRE(symmetric(c));
        StrokeOptions o;
        o.symmetry = Symmetry{SymmetryMode::MirrorX};
        const CellEdit e = paintStroke(c, OutlineBrush(steel()), square(4), {{4, 12}, {7, 7}, {13, 2}, {1, 7}}, o);
        CHECK_FALSE(e.empty());
        CHECK(symmetric(applied(c, e)));
    }
    // Claude round 3 (R2) and Codex round 3 (#2): joined strokes, which
    // stamped the stroke's whole segment before its image's
    {
        const Canvas c = rows36(32, 16, 4,
                                {"...............aa...............", "..............abba..............",
                                 ".............abbbba.............", "..............bbbb..............",
                                 "...............bb..............."},
                                fire());
        REQUIRE(symmetric(c));
        BrushSettings s = square(5);
        s.tip = Tip::Round;
        StrokeOptions o;
        o.symmetry = Symmetry{SymmetryMode::MirrorX};
        const CellEdit e = paintStroke(c, OutlineBrush(fire()), s, {{30, 5}, {11, 1}, {14, 6}, {17, 3}, {1, 8}}, o);
        CHECK_FALSE(e.empty());
        CHECK(symmetric(applied(c, e)));
    }
    {
        const Canvas c = rows36(18, 13, 0,
                                {"..................", "..................", ".......cccc.......", ".....cddddddc.....",
                                 "....cddeeeeddc....", "...cddeeffeeddc...", "...cddeeffeeddc...", "..ccddeeffeeddcc..",
                                 "...ccddeeeeddcc...", "...bccddddddccb...", "....bbccccccbb....", ".....bbbbbbbb.....",
                                 ".......aaaa......."},
                                fire());
        REQUIRE(symmetric(c));
        BrushSettings s = square(6);
        s.tip = Tip::Round;
        StrokeOptions o;
        o.symmetry = Symmetry{SymmetryMode::MirrorX};
        const CellEdit e = paintStroke(c, OutlineBrush(fire()), s, {{0, 8}, {13, 10}}, o);
        CHECK_FALSE(e.empty());
        CHECK(symmetric(applied(c, e)));
    }
    // Random clicks, unjoined and joined strokes on mirrored shaded shapes,
    // even and odd widths: the stroke stamps point k of every image, then
    // k + 1, so each point's images are judged together.
    std::mt19937 g(5529);
    const auto uni = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); };
    int wrote = 0, joined = 0;
    for (int i = 0; i < 300; ++i) {
        const Ramp r = uni(0, 1) ? fire() : steel();
        const int w = uni(8, 25) * 2 + uni(0, 1), h = uni(10, 24);
        const Canvas c = mirroredX(randomShaded(g, w, h, r));
        BrushSettings s = square(uni(0, 6) ? uni(1, 6) : kMaxBrushSize);
        s.tip = uni(0, 1) ? Tip::Round : Tip::Square;
        StrokeOptions o;
        o.symmetry = Symmetry{SymmetryMode::MirrorX};
        const OutlineBrush brush(r);
        Stroke stroke(c, brush, s, o);
        const bool join = i >= 150;
        for (int n = uni(1, 6); n > 0; --n)
            stroke.addDab({uni(0, w - 1), uni(0, h - 1)}, join);
        const CellEdit e = stroke.edit();
        INFO("stroke " << i << " width " << w << (join ? " joined" : ""));
        CHECK(symmetric(applied(c, e)));
        wrote += !e.empty();
        joined += join && !e.empty();
    }
    CHECK(wrote > 100);
    CHECK(joined > 50);
}

TEST_CASE("an outline dab adds no finding anywhere on the canvas", "[detail][lint]") {
    // Codex round 1 (#2): the last ▒ cells go, and a far rung-5 field is
    // left the shading's one glyph
    Canvas c(70, 65);
    for (int y = 2; y <= 6; ++y)
        for (int x = 2; x <= 6; ++x)
            c.at(x, y) = rungCell(fire(), 5);
    for (int y = 40; y <= 44; ++y)
        for (int x = 40; x <= 44; ++x)
            c.at(x, y) = rungCell(fire(), 8);
    for (int x = 41; x <= 43; ++x)
        c.at(x, 40) = rungCell(fire(), 6);
    REQUIRE(lint::checkLadder(c, fire()).clean());
    StrokeOptions o;
    o.selection = std::make_shared<Mask>(Mask::rect(70, 65, 41, 40, 43, 40));
    CHECK(added(c, applied(c, paintStroke(c, OutlineBrush(fire()), square(3), {{42, 40}}, o)), fire()).empty());
    // and a wide row's end that hid the wide columns across it
    Canvas runs(70, 65);
    for (int y = 2; y <= 59; ++y)
        for (int x = 2; x <= 60; ++x)
            runs.at(x, y) = rungCell(fire(), 9);
    for (int y = 2; y <= 59; ++y)
        runs.at(1, y) = rungCell(fire(), 8);
    for (int x = 2; x <= 60; ++x)
        runs.at(x, 1) = rungCell(fire(), 8);
    runs.at(61, 35) = runs.at(62, 35) = rungCell(fire(), 10);
    for (int x = 10; x <= 13; ++x)
        runs.at(x, 60) = rungCell(fire(), 10);
    o.selection = std::make_shared<Mask>(Mask::rect(70, 65, 60, 34, 60, 36));
    const CellEdit e = paintStroke(runs, OutlineBrush(fire()), square(3), {{60, 35}}, o);
    CHECK(added(runs, applied(runs, e), fire()).empty());
}

TEST_CASE("the outline judges the shading's glyphs on the whole canvas", "[detail][lint]") {
    // Claude round 1 (P2-2): the ▓ beyond the dab's crop keeps two glyphs
    Canvas c(50, 25);
    for (int y = 0; y < 25; ++y)
        for (int x = 5; x < 50; ++x)
            c.at(x, y) = rungCell(steel(), 8);
    c.at(5, 12) = rungCell(steel(), 9);
    for (int x = 6; x <= 40; ++x)
        c.at(x, 12) = rungCell(steel(), 10);
    c.at(41, 12) = rungCell(steel(), 11);
    for (int x = 42; x < 50; ++x)
        c.at(x, 12) = rungCell(steel(), 12);
    REQUIRE(lint::checkLadder(c, steel()).findings.empty());
    const CellEdit e = paintStroke(c, OutlineBrush(steel()), square(3), {{5, 12}});
    const Cell k = rungCell(steel(), 4);
    CHECK(written(e) == std::map<Point, Cell>{{{5, 11}, k}, {{5, 12}, k}, {{5, 13}, k}});
    CHECK(added(c, applied(c, e), steel()).empty());
}

TEST_CASE("the outline writes the line wherever one is under the brush", "[detail][lint]") {
    // Claude round 1 (P2-1): a whole-canvas click on a piece-sized shaded
    // shape wrote nothing, nor any click on its rim
    const Canvas c = shadedEllipse(160, 100);
    REQUIRE(lint::checkLadder(c, steel()).findings.empty());
    const OutlineBrush brush(steel());
    const CellEdit all = paintStroke(c, brush, square(kMaxBrushSize), {{80, 50}});
    CHECK(added(c, applied(c, all), steel()).empty());
    // a stroke round the whole rim, a size-3 brush
    const auto path = edgePath(c, steel());
    std::vector<Dab> round;
    for (const auto& [x, y] : path)
        round.push_back({x, y});
    const CellEdit stroke = paintStroke(c, brush, square(3), round);
    CHECK(added(c, applied(c, stroke), steel()).empty());
    const auto a = written(all), b = written(stroke);
    size_t differ = 0;
    for (const auto& [p, cell] : a)
        differ += !b.count(p) || !(b.at(p) == cell);
    for (const auto& [p, cell] : b)
        differ += !a.count(p);
    INFO("click " << a.size() << " cells, stroke " << b.size() << ", " << differ << " differ");
    CHECK(b.size() > 250);
    CHECK(a.size() * 10 >= b.size() * 9);
    CHECK(differ * 10 <= b.size());
    // a click on the top of the rim, and a stroke's first dabs
    const CellEdit click = paintStroke(c, brush, square(3), {{82, 9}});
    CHECK_FALSE(click.empty());
    CHECK(added(c, applied(c, click), steel()).empty());
    // a stroke's first dabs from there, either way round the rim, write
    // the line the click writes under them (west of it the click leaves
    // the rim, as a stroke does)
    const size_t start = size_t(std::find(path.begin(), path.end(), Point{82, 9}) - path.begin());
    REQUIRE(start < path.size());
    size_t most = 0;
    for (const long way : {1L, -1L})
        for (const long n : {1L, 5L, 10L}) {
            INFO("way " << way << ", " << n << " dabs");
            std::vector<Dab> first;
            std::set<Point> under;
            for (long k = 0; k < n; ++k) {
                const Point q = path[size_t(long(start) + long(path.size()) + way * k) % path.size()];
                first.push_back({q.first, q.second});
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        under.insert({q.first + dx, q.second + dy});
            }
            const auto s = written(paintStroke(c, brush, square(3), first));
            CHECK_FALSE(s.empty());
            size_t same = 0, clicked = 0;
            for (const Point p : under)
                if (a.count(p)) {
                    ++clicked;
                    same += s.count(p) && s.at(p) == a.at(p);
                }
            CHECK(same == clicked);
            CHECK(same == s.size());
            most = std::max(most, s.size());
        }
    CHECK(most >= 10);
    // Claude round 1 (P3-1): a crescent's rim, clicked on and beside
    const Canvas moon = rows36(44, 26, 0,
                               {"............................................",
                                "............................................",
                                "............................................",
                                "................998877766...................",
                                "............aaaaa998887766666...............",
                                "..........bbbbbbaa9988776666666.............",
                                "........abbcccbbaa998877....................",
                                ".......aabbbbbbbaa998.......................",
                                "......99aaaaaaaaa99.........................",
                                "......899999999999..........................",
                                ".....778888888888...........................",
                                ".....677777777777...........................",
                                ".....666666666666...........................",
                                ".....666666666666...........................",
                                ".....666666666666...........................",
                                ".....666666666666...........................",
                                "......666666666666..........................",
                                "......6666666666666.........................",
                                ".......66666666666666.......................",
                                "........6666666666666666....................",
                                "..........666666666666666666666.............",
                                "............66666666666666666...............",
                                "................666666666...................",
                                "............................................",
                                "............................................",
                                "............................................"},
                               steel());
    // Claude round 2 (N7), Codex round 2 (#4), Codex round 3 (#4): every
    // click on the rim writes a line. At (12,5) and (12,4) the darkest line
    // puts K beside rung 8 at (11,5), a skip between two cells the line
    // changed; stepping both lightened the whole piece away. The careful
    // settle steps the one more findings involve and then brings lost cells
    // back, writing Codex's line: (12,4) and (13,4) K, (11,5) and (12,5) 8.
    for (const Point p : {Point{11, 4}, Point{10, 5}, Point{11, 5}, Point{12, 5}, Point{12, 4}, Point{13, 5}}) {
        INFO("click " << p.first << "," << p.second);
        const CellEdit e = paintStroke(moon, brush, square(3), {{p.first, p.second}});
        const auto w = written(e);
        CHECK(added(moon, applied(moon, e), steel()).empty());
        CHECK(lint::addedFindingsOnce(moon, applied(moon, e), steel(), cellsOf(e)).empty());
        // a piece of three or more 4-connected cells
        size_t biggest = 0;
        std::set<Point> seen;
        for (const auto& [q, cell] : w) {
            if (seen.count(q))
                continue;
            std::vector<Point> todo{q};
            seen.insert(q);
            size_t n = 0;
            while (!todo.empty()) {
                const Point at = todo.back();
                todo.pop_back();
                ++n;
                for (const Point s : {Point{at.first + 1, at.second}, Point{at.first - 1, at.second},
                                      Point{at.first, at.second + 1}, Point{at.first, at.second - 1}})
                    if (w.count(s) && seen.insert(s).second)
                        todo.push_back(s);
            }
            biggest = std::max(biggest, n);
        }
        CHECK(biggest >= 3);
    }
    {
        const auto w = written(paintStroke(moon, brush, square(3), {{12, 5}}));
        const std::map<Point, int> want{{{12, 4}, 4}, {{13, 4}, 4}, {{11, 5}, 8}, {{12, 5}, 8}};
        CHECK(w.size() == want.size());
        for (const auto& [q, g] : want) {
            INFO("cell " << q.first << "," << q.second);
            REQUIRE(w.count(q));
            CHECK(rungOf(steel(), w.at(q)) == g);
        }
    }
    // Claude round 1 (P2-1): a finding the canvas has already, far off,
    // doesn't keep the line off the rest of the shape
    Canvas two(70, 100);
    for (int y = 5; y <= 94; ++y)
        for (int x = 5; x <= 55; ++x)
            two.at(x, y) = rungCell(steel(), 8);
    for (int y = 8; y <= 90; ++y)
        two.at(30, y) = rungCell(steel(), 10);
    two.at(30, 7) = rungCell(steel(), 9);
    two.at(30, 91) = rungCell(steel(), 11);
    two.at(5, 50) = rungCell(steel(), 9);
    for (int x = 6; x <= 40; ++x)
        two.at(x, 50) = rungCell(steel(), 10);
    two.at(41, 50) = rungCell(steel(), 11);
    for (int x = 42; x <= 55; ++x)
        two.at(x, 50) = rungCell(steel(), 12);
    REQUIRE(findings(two, steel()).size() == 2);
    const CellEdit lined = paintStroke(two, brush, square(kMaxBrushSize), {{35, 50}});
    CHECK(added(two, applied(two, lined), steel()).empty());
    const auto w = written(lined);
    int left = 0, top = 0, bottom = 0;
    for (int y = 6; y <= 93; ++y)
        left += w.count({5, y}) > 0;
    for (int x = 6; x <= 54; ++x) {
        top += w.count({x, 5}) > 0;
        bottom += w.count({x, 94}) > 0;
    }
    INFO("left " << left << " top " << top << " bottom " << bottom);
    CHECK(left >= 80);
    CHECK(top >= 45);
    CHECK(bottom >= 45);
}

TEST_CASE("an outline dab's work is flat in the stroke and linear in the canvas's sides", "[detail]") {
    // Codex round 1 (#4), Claude round 1 (P3-2): the cells a dab reads,
    // the lint's and the copies' included, after the stroke's first dab
    // (which copies the canvas and tallies it once), along the rim
    const auto perDab = [](const Canvas& c, const Ramp& r, const std::vector<Dab>& path, size_t n) {
        const OutlineBrush brush(r);
        Stroke stroke(c, brush, square(3));
        stroke.addDab(path.front());
        const long long before = detailCellsRead();
        for (size_t i = 1; i <= n; ++i)
            stroke.addDab(path[i % path.size()]);
        return double(detailCellsRead() - before) / double(n);
    };
    const auto rimOf = [](const Canvas& c) {
        std::vector<Dab> out;
        for (const auto& [x, y] : edgePath(c, steel()))
            out.push_back({x, y});
        return out;
    };
    std::map<int, double> per;
    for (const auto& [w, h] : {Point{80, 40}, Point{160, 100}, Point{400, 160}}) {
        const Canvas c = shadedEllipse(w, h);
        per[w] = perDab(c, steel(), rimOf(c), 40);
    }
    INFO("cells read a dab: " << per[80] << " at 80x40, " << per[160] << " at 160x100, " << per[400]
                              << " at 400x160");
    // the sides grow 4.7 times, the area 20 times
    CHECK(per[400] <= 4.7 * 2 * per[80]);
    // Codex's strip: an N x 40 canvas of steel w, a dab on its top row
    std::map<int, double> strip;
    for (const int n : {200, 800}) {
        Canvas c(n, 40);
        for (int y = 1; y < 40; ++y)
            for (int x = 0; x < n; ++x)
                c.at(x, y) = rungCell(steel(), 8);
        std::vector<Dab> path;
        for (int x = 100; x < 110; ++x)
            path.push_back({x, 1});
        strip[n] = perDab(c, steel(), path, 9);
    }
    INFO("on the strip: " << strip[200] << " at 200 wide, " << strip[800] << " at 800");
    CHECK(strip[800] <= 2 * (840.0 / 240) * strip[200]);
    // Codex round 2 (#3), Claude round 2 (N4): a square of rung 9 between a
    // column of 8 and one of 10, where three top cells shorten every
    // column's run; an unjoined dab after one at the corner
    std::map<int, double> field;
    for (const int n : {40, 160, 320}) {
        Canvas c(n + 4, n + 4);
        for (int y = 2; y <= n + 1; ++y) {
            for (int x = 2; x <= n + 1; ++x)
                c.at(x, y) = rungCell(steel(), 9);
            c.at(1, y) = rungCell(steel(), 8);
            c.at(n + 2, y) = rungCell(steel(), 10);
        }
        const OutlineBrush brush(steel());
        Stroke stroke(c, brush, square(3));
        stroke.addDab({0, 0}, false);
        const long long before = detailCellsRead();
        stroke.addDab({n / 2, 2}, false);
        field[n] = double(detailCellsRead() - before);
    }
    INFO("on the square: " << field[40] << " at 40, " << field[160] << " at 160, " << field[320] << " at 320");
    // the side grows 8 times; quadratic work would be 64
    CHECK(field[320] <= 2 * 8.0 * field[40]);
    // Codex round 3 (#3): wide columns a three-cell dab exposes on a field
    // of rung 9, walked along their length by the blame across them
    std::map<int, double> exposed;
    for (const int n : {40, 80, 160, 320}) {
        Canvas c(n + 10, n + 10);
        for (int y = 2; y <= n + 1; ++y)
            for (int x = 2; x <= n + 1; ++x)
                c.at(x, y) = rungCell(fire(), 9);
        for (int k = 2; k <= n + 1; ++k) {
            c.at(1, k) = rungCell(fire(), 8);
            c.at(k, 1) = rungCell(fire(), 8);
        }
        const int mid = n / 2;
        c.at(n + 2, mid) = rungCell(fire(), 10);
        c.at(n + 3, mid) = rungCell(fire(), 10);
        for (int x = n / 4; x <= n / 4 + 3; ++x)
            c.at(x, n + 2) = rungCell(fire(), 10);
        StrokeOptions o;
        o.selection = std::make_shared<Mask>(Mask::rect(n + 10, n + 10, n + 1, mid - 1, n + 1, mid + 1));
        const OutlineBrush brush(fire());
        Stroke stroke(c, brush, square(3), o);
        stroke.addDab({0, 0}, false);
        const long long before = detailCellsRead();
        stroke.addDab({n + 1, mid}, false);
        exposed[n] = double(detailCellsRead() - before);
    }
    INFO("exposed columns: " << exposed[40] << " at 40, " << exposed[80] << " at 80, " << exposed[160] << " at 160, "
                             << exposed[320] << " at 320");
    CHECK(exposed[320] <= 2 * 8.0 * exposed[40]);
    // and flat in the stroke's length
    const Canvas c = shadedEllipse(160, 100);
    const auto path = rimOf(c);
    const double ten = perDab(c, steel(), path, 10), more = perDab(c, steel(), path, 400);
    INFO("at 10 dabs " << ten << ", at 400 " << more);
    CHECK(more <= 2 * ten);
    // Claude round 4 (F2): on baud-dudes-all (80 x 2312), a square-161 click
    // at (60,1541) read 2.4e9 cells (16 s), the careful settle running again
    // on each of 15 rounds. The canvas here is that piece's rows 1441-1641
    // (the fixture: 44 KB of the file's 277 KB) repeated down 2312 rows, each
    // row where the piece has it. The click read 1.0e9 cells, a careful pass
    // in every round that wrote nothing; with careful passes started only
    // while the outline has read under 5e7 cells it reads 3.8e8.
    {
        const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / "brushes" / "baud-dudes-1441.ans"));
        const Canvas& rows = doc.canvas;
        const int n = rows.height();
        Canvas c(rows.width(), 2312);
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                c.at(x, y) = rows.at(x, ((y - 1441) % n + n) % n);
        StrokeOptions o;
        o.ice = doc.ice;
        const OutlineBrush brush(*findRamp("skin"));
        const long long before = detailCellsRead();
        const CellEdit e = paintStroke(c, brush, square(161), {{60, 1541}}, o);
        const long long reads = detailCellsRead() - before;
        INFO("long canvas: " << written(e).size() << " cells, " << reads << " reads");
        CHECK(!written(e).empty());
        CHECK(reads <= 500'000'000);
    }
}

TEST_CASE("a larger outline click never writes less of the line on a piece", "[detail][lint]") {
    // Claude round 2 (N3): on downfall-of-numenor, steel, a square click at
    // (75,32) wrote 823 cells at size 91 and 237 at 121, where the first
    // trial took the noise just over its limit and the one finding was
    // blamed on every cell of the line. Claude round 3 (R1): on pieces over
    // the noise limit already, lrh-show (90,34) wrote 193 at size 61 and 7
    // at 91-256, durin-gate (60,27) 22 at 31 and 0 at 61, avatars-generic-2
    // (28,12) 66 at 31 and 0 at 61; noise is now blamed on the cells whose
    // own change raises it. Claude round 4 (F1): where noise was the one
    // finding and no cell's change raised it alone, the line went back in
    // raster order until it cleared: 20-shades (12,5) wrote 6 at 3 and 2 at
    // 9, psyop-radar (20,13) 112 at 61 and 30 at 91; the line now goes back
    // on the index cell by cell, each kept that leaves the noise as it was.
    struct Piece {
        std::string name, ramp;
        std::vector<Point> points;
    };
    const std::vector<Piece> pieces{{"downfall-of-numenor", "steel", {{75, 32}, {40, 20}, {90, 50}}},
                                    {"byzantine-eagle", "steel", {{40, 23}, {20, 10}}},
                                    {"lrh-show", "steel", {{90, 34}, {45, 20}}},
                                    {"durin-gate", "ice", {{60, 27}, {30, 15}}},
                                    {"avatars-generic-2", "steel", {{28, 12}}},
                                    {"20-shades", "skin", {{12, 5}}},
                                    {"psyop-radar", "steel", {{20, 13}}}};
    for (const auto& [name, ramp, points] : pieces) {
        const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / "brushes" / (name + ".ans")));
        const Canvas& c = doc.canvas;
        const Ramp r = *findRamp(ramp);
        StrokeOptions o;
        o.ice = doc.ice;
        const OutlineBrush brush(r);
        for (const Point p : points) {
            size_t was = 0;
            for (const int size : {3, 9, 31, 61, 91, 121, 161, 201, kMaxBrushSize}) {
                const CellEdit e = paintStroke(c, brush, square(size), {{p.first, p.second}}, o);
                INFO(name << " click " << p.first << "," << p.second << " size " << size << ", " << written(e).size()
                          << " cells after " << was);
                CHECK(lint::addedFindingsOnce(c, applied(c, e), r, cellsOf(e)).empty());
                CHECK(written(e).size() >= was);
                was = written(e).size();
            }
        }
    }
}

TEST_CASE("a small dab on a thin part takes the line of its body far off", "[detail][lint]") {
    // Codex round 2 (#5): the dab's box ended at row 22 with no interior in
    // it, so the stem had no line; the level inside is now the whole
    // canvas's
    Canvas c(40, 60);
    for (int y = 35; y <= 45; ++y)
        for (int x = 15; x <= 25; ++x)
            c.at(x, y) = rungCell(steel(), 12);
    for (int y = 1; y <= 34; ++y)
        c.at(20, y) = rungCell(steel(), 12);
    const CellEdit e = paintStroke(c, OutlineBrush(steel()), square(3), {{20, 10}});
    const auto w = written(e);
    for (int y = 9; y <= 11; ++y) {
        INFO("cell 20," << y);
        REQUIRE(w.count({20, y}));
        CHECK(rungOf(steel(), w.at({20, y})) == 8);
    }
    CHECK(lint::addedFindingsOnce(c, applied(c, e), steel(), cellsOf(e)).empty());
}
