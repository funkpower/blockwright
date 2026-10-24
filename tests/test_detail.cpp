// Detail brushes and symmetry (PLAN §10, "Detail"): mirror X, mirror Y,
// both, and radial images of a stroke; the half-block pencil's 8-connected
// mode; the rim-light brush.

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

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <climits>
#include <cmath>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <set>

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

// One stroke of unjoined size-1 dabs: every dab a cell, every image a cell.
CellEdit dots(const Canvas& c, const Brush& b, const std::vector<Dab>& dabs, const StrokeOptions& o) {
    Stroke s(c, b, {}, o);
    for (const Dab& d : dabs)
        s.addDab(d, false);
    return s.edit();
}

StrokeOptions with(SymmetryMode m, int folds = 4, std::optional<Point> centre = {}) {
    StrokeOptions o;
    o.symmetry = Symmetry{m, folds, centre};
    return o;
}

std::vector<Point> positions(const std::vector<Dab>& dabs) {
    std::vector<Point> out;
    for (const Dab& d : dabs)
        out.emplace_back(d.x, d.y);
    return out;
}

} // namespace

TEST_CASE("symmetry images a dab", "[symmetry]") {
    const int w = 21, h = 11;  // centre cell (10, 5)
    const Dab d{14, 3};
    using P = std::vector<Point>;
    CHECK(positions(symmetryDabs({}, d, w, h)) == P{{14, 3}});
    CHECK(positions(symmetryDabs({SymmetryMode::MirrorX}, d, w, h)) == P{{14, 3}, {6, 3}});
    CHECK(positions(symmetryDabs({SymmetryMode::MirrorY}, d, w, h)) == P{{14, 3}, {14, 7}});
    CHECK(positions(symmetryDabs({SymmetryMode::MirrorXY}, d, w, h)) == P{{14, 3}, {6, 3}, {14, 7}, {6, 7}});
    // the images are numbered and flipped as they mirror
    const auto xy = symmetryDabs({SymmetryMode::MirrorXY}, d, w, h);
    for (int i = 0; i < 4; ++i) {
        CHECK(xy[size_t(i)].image == i);
        CHECK(xy[size_t(i)].flipX == (i == 1 || i == 3));
        CHECK(xy[size_t(i)].flipY == (i == 2 || i == 3));
    }
    // a half turn is exact and flips both ways
    const auto half = symmetryDabs({SymmetryMode::Radial, 2}, d, w, h);
    CHECK(positions(half) == P{{14, 3}, {6, 7}});
    CHECK((half[1].flipX && half[1].flipY));
    // a quarter turn is made on screen, where a row is two columns tall:
    // four columns right of the centre is two rows below it
    CHECK(positions(symmetryDabs({SymmetryMode::Radial, 4}, Dab{14, 5}, w, h)) ==
          P{{14, 5}, {10, 7}, {6, 5}, {10, 3}});
    // one row below is two columns left
    CHECK(positions(symmetryDabs({SymmetryMode::Radial, 4}, Dab{10, 6}, w, h)) ==
          P{{10, 6}, {8, 5}, {10, 4}, {12, 5}});
    // half a row rounds down the screen: one column right turns to 5.5 and
    // 4.5, rows 6 and 5
    CHECK(positions(symmetryDabs({SymmetryMode::Radial, 4}, Dab{11, 5}, w, h)) ==
          P{{11, 5}, {10, 6}, {9, 5}, {10, 5}});
    // radial about a set point; folds clamped to 2-8
    CHECK(positions(symmetryDabs({SymmetryMode::Radial, 2, Point{3, 3}}, Dab{5, 3}, w, h)) == P{{5, 3}, {1, 3}});
    CHECK(symmetryDabs({SymmetryMode::Radial, 1}, d, w, h).size() == 2);
    CHECK(symmetryDabs({SymmetryMode::Radial, 99}, d, w, h).size() == 8);
    for (int n = kMinFolds; n <= kMaxFolds; ++n)
        CHECK(symmetryDabs({SymmetryMode::Radial, n}, d, w, h).size() == size_t(n));
    // the centre is its own image under every mode
    for (const SymmetryMode m : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY,
                                 SymmetryMode::Radial})
        for (const Dab& im : symmetryDabs({m, 6}, Dab{10, 5}, w, h))
            CHECK(Point{im.x, im.y} == Point{10, 5});
}

TEST_CASE("a radial quarter turn turns the glyph as selection rotation does", "[symmetry][brush]") {
    // P3-7: four folds: the images a quarter turn clockwise, a half turn and
    // three quarters round, each glyph turned the same way
    const int w = 21, h = 11;
    const auto turnsOf = [&](int folds) {
        std::vector<int> out;
        for (const Dab& im : symmetryDabs({SymmetryMode::Radial, folds}, Dab{14, 3}, w, h))
            out.push_back(im.turns);
        return out;
    };
    CHECK(turnsOf(4) == std::vector<int>{0, 1, 0, 3});  // the half turn is both flips
    CHECK(turnsOf(8) == std::vector<int>{0, 0, 1, 0, 0, 0, 3, 0});
    CHECK(turnsOf(6) == std::vector<int>(6, 0));
    const Canvas c = blank(w, h);
    for (const char32_t ch : {U'▀', U'┌', U'►', U'/'}) {
        const auto out = written(paintStroke(c, GlyphBrush(glyph(ch)), {}, {{14, 5}}, [] {
            StrokeOptions o;
            o.symmetry = {SymmetryMode::Radial, 4};
            return o;
        }()));
        CHECK(out.at({14, 5}).ch == ch);
        CHECK(out.at({10, 7}).ch == rotateGlyph(ch, true));
        CHECK(out.at({6, 5}).ch == flipGlyph(flipGlyph(ch, Flip::Horizontal), Flip::Vertical));
        CHECK(out.at({10, 3}).ch == rotateGlyph(ch, false));
    }
}

TEST_CASE("radial images keep their ends in 64 bits when joined", "[symmetry][brush]") {
    // Codex round 1 #5: four folds about cell (0, 0) of a 10x10 canvas; the
    // quarter turn of (-200, INT_MIN) is (2^32, -100), beyond int. Saturated
    // to INT_MAX the joined line would cross the top row; kept, it passes
    // column 0 near row 33, and no image touches the canvas
    const Canvas c = blank(10, 10);
    const GlyphBrush pen(glyph(U'█'));
    StrokeOptions o;
    o.symmetry = {SymmetryMode::Radial, 4, Point{0, 0}};
    CHECK(paintStroke(c, pen, {}, {{-200, INT_MIN}, {200, 1073741824}}, o).empty());
    // a line from one end of int to the other down column 0: its quarter
    // turns run along row 0 from beyond one end of int to beyond the other,
    // and cross the whole top row
    std::set<Point> cells;
    for (const auto& [p, cell] : written(paintStroke(c, pen, {}, {{0, INT_MIN}, {0, INT_MAX}}, o)))
        cells.insert(p);
    std::set<Point> expected;
    for (int i = 0; i < 10; ++i) {
        expected.insert({0, i});
        expected.insert({i, 0});
    }
    CHECK(cells == expected);
}

TEST_CASE("every mirror mode paints the exact image of an asymmetric stroke", "[symmetry][brush]") {
    const int w = 21, h = 11;
    const Canvas c = blank(w, h);
    const GlyphBrush pen(glyph(U'▌'));
    BrushSettings s;
    s.size = 2;
    // an L, right of the centre and above it, with a tip that isn't
    // symmetric (size 2 reaches right and up)
    const std::vector<Dab> stroke = {{13, 1}, {17, 1}, {17, 4}};
    const auto plain = written(paintStroke(c, pen, s, stroke));
    REQUIRE(plain.size() > 6);
    const auto flipped = [](Cell cell, bool fx, bool fy) {
        if (fx)
            cell.ch = flipGlyph(cell.ch, Flip::Horizontal);
        if (fy)
            cell.ch = flipGlyph(cell.ch, Flip::Vertical);
        return cell;
    };
    for (const SymmetryMode m : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY}) {
        INFO("mode " << int(m));
        std::map<Point, Cell> want = plain;
        for (const auto& [p, cell] : plain) {
            const auto [x, y] = p;
            if (m != SymmetryMode::MirrorY)
                want[{w - 1 - x, y}] = flipped(cell, true, false);
            if (m != SymmetryMode::MirrorX)
                want[{x, h - 1 - y}] = flipped(cell, false, true);
            if (m == SymmetryMode::MirrorXY)
                want[{w - 1 - x, h - 1 - y}] = flipped(cell, true, true);
        }
        CHECK(written(paintStroke(c, pen, s, stroke, with(m))) == want);
    }
    // the glyph is mirrored with the stroke: ▌ on the right is ▐ on the left
    CHECK(written(paintStroke(c, pen, s, stroke, with(SymmetryMode::MirrorX)))[{w - 1 - 13, 1}].ch == U'▐');
    // a half turn is mirror X and Y in one image
    std::map<Point, Cell> want = plain;
    for (const auto& [p, cell] : plain)
        want[{w - 1 - p.first, h - 1 - p.second}] = flipped(cell, true, true);
    CHECK(written(paintStroke(c, pen, s, stroke, with(SymmetryMode::Radial, 2))) == want);
}

TEST_CASE("radial symmetry turns each dab about the centre on screen", "[symmetry][brush]") {
    // On 1:2 cells a turn by anything but a half is made on screen and
    // rounded to the nearest cell, halves down and right; the glyph isn't
    // turned.
    const int w = 41, h = 21;
    const int cx = 20, cy = 10;
    const Canvas c = blank(w, h);
    const GlyphBrush pen(glyph(U'█'));
    const std::vector<Dab> dabs = {{24, 10}, {28, 10}, {28, 8}, {32, 4}};
    // a quarter turn: (dx, dy) cells is (dx, 2 dy) on screen, turned to
    // (-2 dy, dx), back in cells (-2 dy, dx / 2)
    const auto round = [](double v) { return int(std::floor(v + 0.5)); };
    std::set<Point> want;
    for (const Dab& d : dabs) {
        const int dx = d.x - cx, dy = d.y - cy;
        want.insert({cx + dx, cy + dy});
        want.insert({cx - 2 * dy, cy + round(dx / 2.0)});
        want.insert({cx - dx, cy - dy});
        want.insert({cx + 2 * dy, cy + round(-dx / 2.0)});
    }
    std::set<Point> got;
    for (const auto& [p, cell] : written(dots(c, pen, dabs, with(SymmetryMode::Radial, 4))))
        got.insert(p);
    CHECK(got == want);
    // eight folds: every dab of a ring of radius 8 columns lands on the ring
    // of the same radius on screen, within half a cell
    std::set<Point> ring;
    for (const auto& [p, cell] : written(dots(c, pen, {{cx + 8, cy}}, with(SymmetryMode::Radial, 8))))
        ring.insert(p);
    CHECK(ring.size() == 8);
    for (const auto& [x, y] : ring) {
        const double r = std::hypot(double(x - cx), 2.0 * (y - cy));
        CHECK(std::abs(r - 8) <= 1.5);
    }
    // about a set point
    std::set<Point> about;
    for (const auto& [p, cell] : written(dots(c, pen, {{8, 3}}, with(SymmetryMode::Radial, 4, Point{6, 3}))))
        about.insert(p);
    CHECK(about == std::set<Point>{{8, 3}, {6, 4}, {4, 3}, {6, 2}});
}

TEST_CASE("symmetry keeps one step per stroke where images overlap", "[symmetry][brush]") {
    // A ramp shade stroke across the mirror line: the stroke and its image
    // reach the same cells, which still go up one rung, not two.
    const int w = 12, h = 5;
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c.at(x, y) = rungCell(steel(), 4);
    const RampShadeBrush up(steel(), ShadeDirection::Lighten);
    BrushSettings s;
    s.size = 3;
    for (const SymmetryMode m : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY,
                                 SymmetryMode::Radial}) {
        INFO("mode " << int(m));
        const CellEdit e = paintStroke(c, up, s, {{2, 2}, {9, 2}}, with(m));
        REQUIRE_FALSE(e.empty());
        for (const CellChange& ch : e.changes)
            CHECK(rungOf(steel(), ch.after) == 5);
    }
    // the stronger dab wins a cell its image also reaches: pressure scales
    // the strength, and the right half's harder dabs win the middle
    s.pressure = PressureTo::Strength;
    s.size = 4;
    const RampShadeBrush big(steel(), ShadeDirection::Lighten, 4);
    Stroke st(c, big, s, with(SymmetryMode::MirrorX));
    st.addDab({6, 2, 1.0}, false);
    st.addDab({4, 2, 0.25}, false);  // its image at 7 is weaker than the dab at 6
    for (const int x : {5, 6})
        CHECK(rungOf(steel(), *st.cellAt(x, 2)) == 8);
}

TEST_CASE("each image is clipped to the selection and the lock on its own", "[symmetry][brush]") {
    const int w = 20, h = 6;
    Canvas c = blank(w, h);
    c.at(3, 2).transparent = true;
    const GlyphBrush pen(glyph(U'█'));
    StrokeOptions o = with(SymmetryMode::MirrorX);
    o.selection = std::make_shared<Mask>(Mask::rect(w, h, 0, 0, 9, h - 1));  // the left half
    o.lockTransparent = true;
    // the stroke is all outside the selection; its image inside it
    const auto got = written(paintStroke(c, pen, {}, {{14, 2}, {17, 2}}, o));
    std::set<Point> cells;
    for (const auto& [p, cell] : got)
        cells.insert(p);
    CHECK(cells == std::set<Point>{{2, 2}, {4, 2}, {5, 2}});  // (3, 2) is locked
}

TEST_CASE("half-block pixels take every symmetry mode", "[symmetry][halfblock]") {
    // 10 x 5 cells is 10 x 10 square pixels: quarter turns are exact
    const int w = 10, h = 5, ph = 2 * h;
    const Canvas c = blank(w, h);
    const std::vector<Point> stroke = {{6, 1}, {8, 1}, {8, 3}};
    const auto lit = [&](const CellEdit& e) {
        Canvas out = c;
        e.apply(out);
        std::set<Point> s;
        for (int py = 0; py < ph; ++py)
            for (int x = 0; x < w; ++x)
                if (pickColour(out, x, py, false) == Color::indexed(White))
                    s.insert({x, py});
        return s;
    };
    const auto draw = [&](const StrokeOptions& o) {
        HalfBlockStroke s(c, White, o);
        for (const auto& [x, py] : stroke)
            s.addPoint(x, py);
        return lit(s.edit());
    };
    const std::set<Point> plain = draw({});
    REQUIRE(plain.size() == 5);
    const auto image = [&](auto f) {
        std::set<Point> s = plain;
        for (const auto& [x, y] : plain)
            for (const Point& q : f(x, y))
                s.insert(q);
        return s;
    };
    using V = std::vector<Point>;
    CHECK(draw(with(SymmetryMode::MirrorX)) == image([&](int x, int y) { return V{{w - 1 - x, y}}; }));
    CHECK(draw(with(SymmetryMode::MirrorY)) == image([&](int x, int y) { return V{{x, ph - 1 - y}}; }));
    CHECK(draw(with(SymmetryMode::MirrorXY)) == image([&](int x, int y) {
              return V{{w - 1 - x, y}, {x, ph - 1 - y}, {w - 1 - x, ph - 1 - y}};
          }));
    CHECK(draw(with(SymmetryMode::Radial, 4)) == image([&](int x, int y) {
              return V{{9 - y, x}, {9 - x, 9 - y}, {y, 9 - x}};
          }));
    CHECK(draw(with(SymmetryMode::Radial, 2)) == image([&](int x, int y) { return V{{9 - x, 9 - y}}; }));
    // the old mirror flag is mirror X
    HalfBlockStroke old(c, White, {}, true);
    for (const auto& [x, py] : stroke)
        old.addPoint(x, py);
    CHECK(lit(old.edit()) == draw(with(SymmetryMode::MirrorX)));
    // shapes and fills plot through the painter and get the same images
    CHECK(lit(plotPixels(c, {{6, 1}}, White, with(SymmetryMode::MirrorXY))) ==
          std::set<Point>{{6, 1}, {3, 1}, {6, 8}, {3, 8}});
    // eight folds: each image of a long stroke is joined, not dotted
    HalfBlockStroke ring(c, White, with(SymmetryMode::Radial, 8));
    ring.addPoint(5, 1);
    ring.addPoint(8, 1);
    const auto all = lit(ring.edit());
    CHECK(all.size() >= 8 * 3);
}

TEST_CASE("a pixel off the canvas still has the images that land on it", "[symmetry][halfblock]") {
    // 10 x 3 cells, 10 x 6 pixels, turned about the middle: pixel (4, -1),
    // above the top edge, turns onto (8, 2) and (1, 3)
    const Canvas c = blank(10, 3);
    const auto cellsOf = [](const CellEdit& e) {
        std::set<Point> s;
        for (const CellChange& ch : e.changes)
            s.insert({ch.x, ch.y});
        return s;
    };
    CHECK(cellsOf(plotPixels(c, {{4, -1}}, White, with(SymmetryMode::Radial, 4))) == std::set<Point>{{8, 1}, {1, 1}});
    // and a pixel's own image off the canvas is still not drawn
    CHECK(cellsOf(plotPixels(c, {{4, -1}}, White)).empty());
}

TEST_CASE("a shape off the canvas is rasterised as far as its images reach", "[symmetry][halfblock]") {
    const Canvas c = blank(10, 3);
    const StrokeOptions o = with(SymmetryMode::Radial, 4);
    // a line one pixel above the top edge: none of it is on the canvas,
    // but its quarter turns are
    Pixels whole;
    for (int x = 2; x <= 7; ++x)
        whole.push_back({x, -1});
    const CellEdit want = plotPixels(c, whole, White, o);
    REQUIRE_FALSE(want.empty());
    const PixelRect reach = symmetryReach(o.symmetry, 10, 3);
    CHECK(plotPixels(c, lineShape(2, -1, 7, -1, reach), White, o).changes == want.changes);
    // clipped to the canvas, the shape had no pixels to turn
    CHECK(lineShape(2, -1, 7, -1, 10, 6).empty());
    // a rectangle and an ellipse dragged past the corner keep theirs too
    for (const bool filled : {false, true}) {
        Pixels rect, oval;
        for (int py = -8; py <= 3; ++py)
            for (int x = -6; x <= 4; ++x)
                if (filled || x == -6 || x == 4 || py == -8 || py == 3)
                    rect.push_back({x, py});
        CHECK(plotPixels(c, rectShape(-6, -8, 4, 3, filled, reach), White, o).changes ==
              plotPixels(c, rect, White, o).changes);
        const PixelRect all{-100, -100, 100, 100};
        CHECK(plotPixels(c, ellipseShape(-6, -8, 4, 3, filled, reach), White, o).changes ==
              plotPixels(c, ellipseShape(-6, -8, 4, 3, filled, all), White, o).changes);
    }
}

TEST_CASE("every pixel with an image on the canvas is within the symmetry's reach", "[symmetry]") {
    for (const auto& [w, h] : std::vector<Point>{{10, 3}, {7, 5}, {1, 1}, {30, 2}})
        for (int n = 1; n <= 8; ++n)
            for (const auto& centre : std::vector<std::optional<Point>>{std::nullopt, Point{0, 0}, Point{w - 1, h - 1}}) {
                const Symmetry s{n == 1 ? SymmetryMode::MirrorXY : SymmetryMode::Radial, std::max(n, 2), centre};
                const PixelRect r = symmetryReach(s, w, h);
                for (int py = -80; py <= 80; ++py)
                    for (int x = -80; x <= 80; ++x) {
                        bool lands = false;
                        for (const auto& [ix, iy] : symmetryPixels(s, x, py, w, h))
                            lands = lands || (ix >= 0 && ix < w && iy >= 0 && iy < 2 * h);
                        if (lands && !(x >= r.x0 && x <= r.x1 && py >= r.y0 && py <= r.y1)) {
                            INFO(w << "x" << h << " folds " << n << " pixel " << x << "," << py);
                            CHECK(false);
                        }
                    }
            }
}

TEST_CASE("line8 steps diagonally and clips", "[halfblock]") {
    using P = std::vector<Point>;
    CHECK(line8(0, 0, 3, 3, 0, 0, 9, 9) == P{{0, 0}, {1, 1}, {2, 2}, {3, 3}});
    CHECK(line8(3, 3, 0, 0, 0, 0, 9, 9) == P{{3, 3}, {2, 2}, {1, 1}, {0, 0}});
    CHECK(line8(0, 0, 4, 2, 0, 0, 9, 9) == P{{0, 0}, {1, 1}, {2, 1}, {3, 2}, {4, 2}});
    CHECK(line8(0, 0, 1, 4, 0, 0, 9, 9) == P{{0, 0}, {0, 1}, {1, 2}, {1, 3}, {1, 4}});
    CHECK(line8(2, 2, 2, 2, 0, 0, 9, 9) == P{{2, 2}});
    CHECK(line8(2, 2, 2, 2, 3, 3, 9, 9).empty());
    // only the part in the box, in order, from a line far outside it
    CHECK(line8(-1000000, -1000000, 1000000, 1000000, 0, 0, 2, 2) == P{{0, 0}, {1, 1}, {2, 2}});
    CHECK(line8(INT_MIN, 0, INT_MAX, 0, 0, 0, 3, 0) == P{{0, 0}, {1, 0}, {2, 0}, {3, 0}});
    // every step moves one pixel each way at most
    const P l = line8(1, 7, 23, 2, 0, 0, 30, 30);
    for (size_t i = 1; i < l.size(); ++i) {
        CHECK(std::abs(l[i].first - l[i - 1].first) <= 1);
        CHECK(std::abs(l[i].second - l[i - 1].second) <= 1);
    }
    CHECK(l.size() == 23);
}

TEST_CASE("the pencil joins 4-connected by default, 8-connected on request", "[halfblock]") {
    const Canvas c = blank(8, 4);
    const auto count = [&](Connectivity k) {
        HalfBlockStroke s(c, White, {}, false, k);
        s.addPoint(0, 0);
        s.addPoint(5, 5);
        Canvas out = c;
        s.edit().apply(out);
        std::set<Point> lit;
        for (int py = 0; py < 8; ++py)
            for (int x = 0; x < 8; ++x)
                if (pickColour(out, x, py, false) == Color::indexed(White))
                    lit.insert({x, py});
        return lit;
    };
    const auto four = count(Connectivity::Four), eight = count(Connectivity::Eight);
    CHECK(four.size() == 11);  // a staircase: every pixel touches the next by a side
    CHECK(eight == std::set<Point>{{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}});
    HalfBlockStroke dflt(c, White, {});
    dflt.addPoint(0, 0);
    dflt.addPoint(5, 5);
    HalfBlockStroke explicitFour(c, White, {}, false, Connectivity::Four);
    explicitFour.addPoint(0, 0);
    explicitFour.addPoint(5, 5);
    CHECK(dflt.edit().changes.size() == explicitFour.edit().changes.size());
}

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

// A disc of one cell on black, round on screen.
Canvas disc(int w, int h, int size, const Cell& cell) {
    Canvas c(w, h);
    for (const auto& [x, y] : footprint(w / 2, h / 2, size, Tip::Round, w, h))
        c.at(x, y) = cell;
    return c;
}

} // namespace

TEST_CASE("edges and normals of a ramp shape", "[detail]") {
    const Canvas c = blob();
    using N = std::pair<double, double>;
    CHECK(edgeNormal(c, steel(), 2, 2) == N{-1, 0});
    CHECK(edgeNormal(c, steel(), 6, 3) == N{1, 0});
    CHECK(edgeNormal(c, steel(), 4, 1) == N{0, -1});
    CHECK(edgeNormal(c, steel(), 4, 4) == N{0, 1});
    // a corner points out diagonally on screen, where a row is two columns
    // tall: the outside within three columns of it lies as far left as up
    const double d = 1 / std::sqrt(2.0);
    const auto corner = edgeNormal(c, steel(), 2, 1);
    REQUIRE(corner);
    CHECK(std::abs(corner->first + d) < 1e-12);
    CHECK(std::abs(corner->second + d) < 1e-12);
    // the edge is the 3x3 one: a cell with the outside only diagonally
    // is on it
    Canvas notch(3, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            notch.at(x, y) = rungCell(steel(), 8);
    notch.at(0, 0) = Cell{};
    CHECK(onEdge(notch, steel(), 1, 1));
    CHECK(edgeNormal(notch, steel(), 1, 1));
    CHECK_FALSE(onEdge(c, steel(), 3, 2));
    CHECK_FALSE(edgeNormal(c, steel(), 4, 2));  // inside
    CHECK_FALSE(edgeNormal(c, steel(), 0, 0));  // black: not in the shape
    // off the canvas is not outside
    Canvas full(3, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            full.at(x, y) = rungCell(steel(), 8);
    CHECK_FALSE(edgeNormal(full, steel(), 0, 0));
    // another ramp's colour is outside
    full.at(1, 0) = glyph(U'█', LightRed);
    CHECK(edgeNormal(full, steel(), 1, 1) == N{0, -1});
}

TEST_CASE("the rim brush lights the edges facing the light by one rung", "[detail]") {
    const Canvas c = blob();
    BrushSettings s;
    s.size = 12;
    const auto lit = [&](Compass from) {
        std::map<Point, int> out;
        for (const auto& [p, cell] : written(paintStroke(c, RimLightBrush(steel(), from), s, {{4, 2}}))) {
            REQUIRE(rungOf(steel(), cell));
            out[p] = *rungOf(steel(), cell) - *rungOf(steel(), c.at(p.first, p.second));
        }
        return out;
    };
    // the normals are read on screen over a window three columns across,
    // so a block this small reads with rounded corners. From the top left:
    // the corner and the cells either side of it along the top and down the
    // left side; W at the right end is the top of the ramp
    CHECK(lit(Compass::NW) ==
          std::map<Point, int>{{{2, 1}, 1}, {{3, 1}, 1}, {{4, 1}, 1}, {{2, 2}, 1}, {{2, 3}, 1}});
    CHECK(lit(Compass::N) == std::map<Point, int>{{{2, 1}, 1}, {{3, 1}, 1}, {{4, 1}, 1}, {{5, 1}, 1}});
    CHECK(lit(Compass::S) == std::map<Point, int>{{{2, 4}, 1}, {{3, 4}, 1}, {{4, 4}, 1}, {{5, 4}, 1}});
    // from the left: the left column and the rounding of its corners
    CHECK(lit(Compass::W) == std::map<Point, int>{{{2, 1}, 1}, {{2, 2}, 1}, {{2, 3}, 1}, {{2, 4}, 1},
                                                  {{3, 1}, 1}, {{3, 4}, 1}});
    // from the right: the right column is W already, so only the rounding
    CHECK(lit(Compass::E) == std::map<Point, int>{{{5, 1}, 1}, {{5, 4}, 1}});
    CHECK(lit(Compass::SW) ==
          std::map<Point, int>{{{2, 2}, 1}, {{2, 3}, 1}, {{2, 4}, 1}, {{3, 4}, 1}, {{4, 4}, 1}});
    // all eight points of the compass are unit vectors 45° apart
    for (int i = 0; i < 8; ++i) {
        const auto [x, y] = compassVector(Compass(i));
        CHECK(std::abs(std::hypot(x, y) - 1) < 1e-12);
        const auto [nx, ny] = compassVector(Compass((i + 1) % 8));
        CHECK(std::abs(x * nx + y * ny - std::sqrt(0.5)) < 1e-12);
    }
    CHECK(compassVector(Compass::N) == std::pair{0.0, -1.0});
    CHECK(compassVector(Compass::E) == std::pair{1.0, 0.0});
    // iCE off: a rung that needs a bright background isn't written
    const Ramp ice = *findRamp("ice");  // k b B C W: B and C are both bright
    Canvas blue(5, 4);
    for (int y = 1; y <= 2; ++y)
        for (int x = 1; x <= 3; ++x)
            blue.at(x, y) = rungCell(ice, 8);
    StrokeOptions off;
    off.ice = false;
    CHECK(paintStroke(blue, RimLightBrush(ice, Compass::N), s, {{2, 1}}, off).empty());
    CHECK_FALSE(paintStroke(blue, RimLightBrush(ice, Compass::N), s, {{2, 1}}).empty());
}

TEST_CASE("the rim light's lit arc is centred on the light on screen", "[detail]") {
    // a disc round on screen (a row two columns tall): under each diagonal
    // light the lit cells, measured on screen from the centre, balance about
    // the light's direction. Normals taken in cells would tip the arc
    // towards the vertical, by about 19° here. The cells don't balance
    // exactly: on the steep part of the rim nearer the light (for NE, the
    // right side above the middle) the rim is two cells wide and the inner
    // cell sees the light past the corner of the row above, while at the
    // steep end of the arc (the right side below the middle) the inner
    // cell's only outside is the corner below, behind the light, so it
    // isn't lit (#54). The arc tips about 5.6° towards the shallow end
    const Canvas c = disc(61, 31, 41, rungCell(steel(), 8));
    const double cx = c.width() / 2, cy = c.height() / 2;
    BrushSettings s;
    s.size = kMaxBrushSize;
    for (const Compass from : {Compass::NE, Compass::SE, Compass::SW, Compass::NW}) {
        const auto lit = written(paintStroke(c, RimLightBrush(steel(), from), s, {{30, 15}}));
        REQUIRE(lit.size() > 10);
        double sx = 0, sy = 0;
        for (const auto& [p, cell] : lit) {
            const double X = p.first - cx, Y = 2 * (p.second - cy);
            const double r = std::hypot(X, Y);
            sx += X / r;
            sy += Y / r;
        }
        const auto [lx, ly] = compassVector(from);
        const double angle = std::acos((sx * lx + sy * ly) / std::hypot(sx, sy)) * 180 / std::acos(-1.0);
        CHECK(angle < 7);
        // and its two ends lie about as far either side of the light
        double lo = 180, hi = -180;
        const double towards = std::atan2(ly, lx);
        for (const auto& [p, cell] : lit) {
            double a = (std::atan2(2 * (p.second - cy), p.first - cx) - towards) * 180 / std::acos(-1.0);
            a = std::remainder(a, 360.0);
            lo = std::min(lo, a);
            hi = std::max(hi, a);
        }
        CHECK(std::abs(lo + hi) < 12);
        CHECK(hi < 90);
    }
}

TEST_CASE("a symmetry image beyond int is left out", "[symmetry]") {
    // Codex round 6: the mirror of INT_MIN on a 10-wide canvas is column
    // 2^31 + 9, past INT_MAX; it is omitted, not moved to INT_MAX
    const auto ims = symmetryDabs(Symmetry{SymmetryMode::MirrorX}, Dab{INT_MIN, 0}, 10, 10);
    REQUIRE(ims.size() == 1);
    CHECK(ims[0].x == INT_MIN);
    CHECK(ims[0].y == 0);
    CHECK(ims[0].image == 0);
    CHECK_FALSE(ims[0].flipX);
}

namespace {

// A filled steel (w) ellipse drawn as the Ellipse tool draws it, on black:
// its rims are half-block fringes wherever it starts or ends on a cell's
// lower or upper pixel.
Canvas ellipse(int w, int h, int x0, int py0, int x1, int py1) {
    Canvas c(w, h);
    plotPixels(c, ellipseShape(x0, py0, x1, py1, true, w, 2 * h), 7).apply(c);
    return c;
}

bool isFringe(const Cell& c) { return c.ch == kUpperHalf || c.ch == kLowerHalf; }

// A rim-light pass over the whole canvas as one stroke, checked against the
// stroke contract: preview is the canvas with the edit applied, each changed
// cell listed once with its value at the start, and every change one rung
// lighter. The cells it lit.
std::set<Point> rimLit(const Canvas& c, Compass from) {
    const RimLightBrush rim(steel(), from);
    BrushSettings s;
    s.size = kMaxBrushSize;
    Stroke st(c, rim, s);
    st.addDab({c.width() / 2, c.height() / 2});
    const CellEdit e = st.edit();
    Canvas applied = c;
    e.apply(applied);
    const Canvas preview = st.preview();
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            CHECK(preview.at(x, y) == applied.at(x, y));
    std::set<Point> out;
    for (const CellChange& ch : e.changes) {
        CHECK(out.insert({ch.x, ch.y}).second);
        CHECK(ch.before == c.at(ch.x, ch.y));
        REQUIRE(rungOf(steel(), ch.before));
        REQUIRE(rungOf(steel(), ch.after));
        CHECK(*rungOf(steel(), ch.after) == *rungOf(steel(), ch.before) + 1);
    }
    CHECK(paintRegion(c, rim, {}).changes == e.changes);
    return out;
}

} // namespace

namespace {

constexpr Compass kCompass[] = {Compass::N, Compass::NE, Compass::E, Compass::SE,
                                Compass::S, Compass::SW, Compass::W, Compass::NW};

// A step towards each point of the compass, in cells (y down), in the order
// of Compass.
constexpr std::pair<int, int> kStep[] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};

// The compass point mirrored left to right (`acrossX`) or top to bottom.
Compass mirrored(Compass from, bool acrossX) {
    const auto [lx, ly] = compassVector(from);
    for (const Compass c : kCompass) {
        const auto [mx, my] = compassVector(c);
        if (std::abs(mx - (acrossX ? -lx : lx)) < 1e-9 && std::abs(my - (acrossX ? ly : -ly)) < 1e-9)
            return c;
    }
    return from;
}

// Pixels drawn in steel w on black.
Canvas drawn(int w, int h, const Pixels& px) {
    Canvas c(w, h);
    plotPixels(c, px, 7).apply(c);
    return c;
}

// The cells a rim-light pass over the whole canvas lights.
std::set<Point> region(const Canvas& c, Compass from) {
    std::set<Point> out;
    for (const CellChange& ch : paintRegion(c, RimLightBrush(steel(), from), {}).changes)
        out.insert({ch.x, ch.y});
    return out;
}

// Whether the cell at (x, y) of a canvas w x h drawn from the pixels `on`
// has a black pixel on the canvas among its eight neighbours' on the side
// the light comes from: one whose centre, on screen, is no further from
// the light than the cell's own (a dot product with the light's direction
// of 0 or more). A pixel is a column wide and half a row tall, so pixel
// 2(y + dy) + h has its centre 2dy + h - 1/2 columns below the cell's. The
// light can only fall on a cell past outside that lies beside it.
bool outsideTowards(const std::set<Point>& on, int w, int h, int x, int y, Compass from) {
    const auto [lx, ly] = compassVector(from);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            for (int k = 0; k < 2; ++k) {
                const int px = x + dx, py = 2 * (y + dy) + k;
                if ((dx || dy) && px >= 0 && px < w && py >= 0 && py < 2 * h && !on.count({px, py}) &&
                    dx * lx + (2 * dy + k - 0.5) * ly >= 0)
                    return true;
            }
    return false;
}

// Every cell the rim light lights from each point of the compass, on a
// canvas w x h drawn from `px`, has black beside it towards the light;
// the first that hasn't, or empty.
std::string litFromBehind(const Pixels& px, int w, int h) {
    const std::set<Point> on(px.begin(), px.end());
    const Canvas c = drawn(w, h, px);
    for (const Compass from : kCompass)
        for (const auto& [x, y] : region(c, from))
            if (!outsideTowards(on, w, h, x, y, from))
                return "(" + std::to_string(x) + "," + std::to_string(y) + ") from " + std::to_string(int(from));
    return {};
}

} // namespace

TEST_CASE("the rim light moves in past a half-block fringe", "[detail]") {
    // Claude round 6 P2-1: a fringe can't go a rung lighter, so a shape
    // drawn with the Ellipse tool whose top row is fringes had no rim on
    // top. The fringes stay as they are, and a full cell next to one is on
    // the edge; it is lit where its own normal, taken on the half-block
    // pixel grid with the fringe's black half outside, faces the light. The
    // ellipse's top and bottom rows are fringes, and so are the ends of the
    // rows round its sides:
    //
    //   .......ffffff.......
    //   ....foooooooooof....
    //   ..foooooooooooooof..
    //   ..oooooooooooooooo..
    //   ..foooooooooooooof..
    //   ....foooooooooof....
    //   .......ffffff.......
    const Canvas c = ellipse(20, 9, 2, 3, 17, 14);
    const auto full = [&](const Point& p) {
        return c.contains(p.first, p.second) && !(c.at(p.first, p.second) == Cell{}) &&
               !isFringe(c.at(p.first, p.second));
    };
    // Walking in from the light, a line at a time (a column from N or S, a
    // row from E or W): where the first shape cell is a fringe, the first
    // full cell past the fringes.
    const auto inward = [&](Compass from) {
        std::vector<Point> out;
        const bool columns = from == Compass::N || from == Compass::S;
        const bool back = from == Compass::S || from == Compass::E;
        const int lines = columns ? c.width() : c.height(), length = columns ? c.height() : c.width();
        for (int l = 0; l < lines; ++l) {
            const auto at = [&](int i) {
                const int j = back ? length - 1 - i : i;
                return columns ? Point{l, j} : Point{j, l};
            };
            int i = 0;
            while (i < length && c.at(at(i).first, at(i).second) == Cell{})
                ++i;
            if (i == length || !isFringe(c.at(at(i).first, at(i).second)))
                continue;
            while (i < length && isFringe(c.at(at(i).first, at(i).second)))
                ++i;
            if (i < length && full(at(i)))
                out.push_back(at(i));
        }
        return out;
    };
    for (const Compass from : kCompass) {
        INFO("light " << int(from));
        const std::set<Point> lit = rimLit(c, from);
        // every full cell next to a fringe is on the edge, and lit exactly
        // where its own normal faces the light
        int litBeside = 0;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x) {
                bool beside = false;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        beside = beside || (c.contains(x + dx, y + dy) && isFringe(c.at(x + dx, y + dy)));
                if (!beside || !full({x, y}))
                    continue;
                INFO("cell " << x << "," << y);
                REQUIRE(onEdge(c, steel(), x, y));
                const auto n = edgeNormal(c, steel(), x, y);
                REQUIRE(n);
                CHECK(bool(lit.count({x, y})) == RimLightBrush::facesLight(*n, from));
                litBeside += int(lit.count({x, y}));
            }
        CHECK(litBeside >= 4);
        // from a side: each line whose first cell is a fringe has its first
        // full cell lit where that cell's own normal faces the light
        if (int(from) % 2 == 0) {
            int litInward = 0;
            for (const Point& p : inward(from)) {
                const auto n = edgeNormal(c, steel(), p.first, p.second);
                REQUIRE(n);
                if (RimLightBrush::facesLight(*n, from)) {
                    INFO("cell " << p.first << "," << p.second);
                    CHECK(lit.count(p));
                    ++litInward;
                }
            }
            CHECK(litInward >= (from == Compass::N || from == Compass::S ? 8 : 2));
        }
        // the fringes stay as they were (rimLit checks every change is a
        // rung lighter, which a half block can't be)
        for (const Point& p : lit)
            CHECK_FALSE(isFringe(c.at(p.first, p.second)));
    }
    // From N the whole top is lit, the row under the top fringes and the
    // shoulders; the cells under the side fringes at (2,4) and (17,4) face
    // W and E and stay unlit
    std::set<Point> top;
    for (int x = 5; x <= 14; ++x)
        top.insert({x, 2});
    for (const int x : {3, 4, 5, 14, 15, 16})
        top.insert({x, 3});
    CHECK(rimLit(c, Compass::N) == top);
}

TEST_CASE("a fringe's black half keeps the light off the shape beyond it", "[detail]") {
    // Codex round 7: (5,2) hangs from the upper bar and faces down; the
    // fringe under it faces up, but its black upper half lies between the
    // two shapes. Light from N doesn't cross it
    Canvas c(11, 7);
    const Ramp r = steel();
    for (int x = 2; x <= 8; ++x) {
        c.at(x, 1) = rungCell(r, 8);
        c.at(x, 4) = rungCell(r, 4);
        c.at(x, 5) = rungCell(r, 4);
    }
    c.at(5, 2) = rungCell(r, 8);
    c.at(5, 3) = halfBlockCell({Black, DarkGrey});
    CHECK(paintStroke(c, RimLightBrush(r, Compass::N), {}, {{5, 2}}).empty());
}

TEST_CASE("a bar three pixels tall faces down on its full row", "[detail]") {
    // Claude round 7 P2-1: row 1 is ▄ fringes, row 2 the bar's full row
    // and its underside. Its normal, with the fringes' black halves above
    // it, faces S: lit from S, not from N
    Pixels px;
    for (int x = 2; x <= 21; ++x)
        for (int py = 3; py <= 5; ++py)
            px.push_back({x, py});
    const Canvas c = drawn(24, 5, px);
    const std::set<Point> n = region(c, Compass::N), s = region(c, Compass::S);
    CHECK(n != s);
    for (int x = 2; x <= 21; ++x)
        CHECK_FALSE(n.count({x, 2}));
    for (int x = 5; x <= 18; ++x)
        CHECK(s.count({x, 2}));
}

TEST_CASE("the rim light doesn't depend on where the shape sits on the canvas", "[detail]") {
    // Claude round 7 P2-2 (a): the round-6 ellipse, its ▀ fringes on the
    // last row of a 20 x 8 canvas, lights as on a canvas a row taller
    Pixels px;
    for (int py = 0; py < 16; ++py)
        for (int x = 0; x < 20; ++x) {
            const double ex = (x - 9.5) / 8.0, ey = (py - 8.5) / 6.3;
            if (ex * ex + ey * ey <= 1)
                px.push_back({x, py});
        }
    const Canvas c8 = drawn(20, 8, px), c9 = drawn(20, 9, px);
    for (const Compass from : kCompass) {
        INFO("light " << int(from));
        const std::set<Point> lit = region(c8, from);
        CHECK_FALSE(lit.empty());
        CHECK(lit == region(c9, from));
    }
    // (b): a shape whose ▄ fringes are canvas row 0 lights as the same
    // shape a cell lower
    Pixels top, down;
    for (int py = 1; py < 16; ++py)
        for (int x = 0; x < 20; ++x) {
            const double ex = (x - 9.5) / 8.0, ey = (py - 3) / 6.3;
            if (ex * ex + ey * ey <= 1) {
                top.push_back({x, py});
                down.push_back({x, py + 2});
            }
        }
    const Canvas a = drawn(20, 9, top), b = drawn(20, 10, down);
    for (const Compass from : kCompass) {
        INFO("light " << int(from));
        std::set<Point> raised;
        for (const auto& [x, y] : region(b, from))
            raised.insert({x, y - 1});
        CHECK(region(a, from) == raised);
        if (from == Compass::N)
            CHECK(raised.size() >= 10);
    }
}

TEST_CASE("the rim light on random half-block shapes faces the light and mirrors", "[detail]") {
    // 500 filled ellipses, rectangles and blobs of them with lines through,
    // drawn as the shape tools draw them, lit from all eight points. The
    // lit cells are worked out again here from the pixels alone: a full
    // cell with a black pixel among its eight neighbours' on the canvas,
    // whose normal on the pixel grid, over the black pixels in its window
    // that reach it, faces the light, with a black pixel among its
    // neighbours' on the light's side of it on screen, unless the cell a
    // step towards the light is full while the one a step away has a black
    // pixel. And the canvas mirrored, lit from the mirrored point, lights
    // the mirrored cells
    constexpr int W = 20, H = 10;
    std::mt19937 rng(50);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const auto box = [&](auto shape) {
        int x0 = pick(-3, W + 2), x1 = pick(-3, W + 2), y0 = pick(-3, 2 * H + 2), y1 = pick(-3, 2 * H + 2);
        return shape(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
    };
    const auto ellipsePx = [](int x0, int y0, int x1, int y1) { return ellipseShape(x0, y0, x1, y1, true, W, 2 * H); };
    const auto rectPx = [](int x0, int y0, int x1, int y1) { return rectShape(x0, y0, x1, y1, true, W, 2 * H); };
    const auto linePx = [](int x0, int y0, int x1, int y1) { return lineShape(x0, y0, x1, y1, W, 2 * H); };
    std::vector<std::string> bad;
    int litTotal = 0, held = 0, turned = 0, hidden = 0;
    for (int n = 0; n < 500; ++n) {
        Pixels px;
        const int kind = n % 3;
        for (int i = 0, parts = kind == 2 ? pick(2, 4) : 1; i < parts; ++i) {
            const Pixels p = kind == 0 || (kind == 2 && pick(0, 1)) ? box(ellipsePx) : box(rectPx);
            px.insert(px.end(), p.begin(), p.end());
            if (kind == 2 && pick(0, 1)) {
                const Pixels l = box(linePx);
                px.insert(px.end(), l.begin(), l.end());
            }
        }
        const std::set<Point> on(px.begin(), px.end());
        const auto empty = [&](int x, int py) { return x >= 0 && x < W && py >= 0 && py < 2 * H && !on.count({x, py}); };
        const auto full = [&](int x, int y) { return on.count({x, 2 * y}) && on.count({x, 2 * y + 1}); };
        // the normal on the pixel grid: the offsets from the cell's centre
        // to the black pixels of the cells in its window, on screen. With
        // `reaching`, only the black pixels that reach the cell: those
        // joined, by a side or a corner within the window, to one on the
        // rows from the pixel above the cell's to the one below and the
        // columns from the cell before to the cell after; failing any, to
        // one in those columns on the window's three rows of cells
        const auto faces = [&](int x, int y, Compass from, bool reaching) {
            // the window's pixels: row r (0-5) from the top pixel of the row
            // above, column dx (-3 to 3) from the cell
            const auto black = [&](int r, int dx) {
                const int dy = r / 2 - 1;
                return dx >= -3 && dx <= 3 && r >= 0 && r < 6 && dx * dx + 4 * dy * dy <= 9 &&
                       empty(x + dx, 2 * (y - 1) + r);
            };
            bool reach[6][7] = {};
            const auto seed = [&](int r0, int r1) {
                std::vector<Point> todo;
                for (int r = r0; r <= r1; ++r)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (black(r, dx) && !reach[r][dx + 3]) {
                            reach[r][dx + 3] = true;
                            todo.push_back({r, dx});
                        }
                const bool any = !todo.empty();
                while (!todo.empty()) {
                    const auto [r, dx] = todo.back();
                    todo.pop_back();
                    for (int rr = r - 1; rr <= r + 1; ++rr)
                        for (int dd = dx - 1; dd <= dx + 1; ++dd)
                            if (black(rr, dd) && !reach[rr][dd + 3]) {
                                reach[rr][dd + 3] = true;
                                todo.push_back({rr, dd});
                            }
                }
                return any;
            };
            if (reaching && !seed(1, 4))
                seed(0, 5);
            double gx = 0, gy = 0;
            for (int r = 0; r < 6; ++r)
                for (int dx = -3; dx <= 3; ++dx)
                    if (reaching ? reach[r][dx + 3] : black(r, dx)) {
                        gx += dx;
                        gy += 2 * (r / 2 - 1) + r % 2 - 0.5;
                    }
            const double len = std::hypot(gx, gy);
            const auto [lx, ly] = compassVector(from);
            return len > 1e-9 && (gx * lx + gy * ly) / len > 0.3;
        };
        const auto edge = [&](int x, int y) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (empty(x + dx, 2 * (y + dy)) || empty(x + dx, 2 * (y + dy) + 1))
                        return true;
            return false;
        };
        // the cells beside it don't hide it from the light
        const auto open = [&](int x, int y, Compass from) {
            const auto [sx, sy] = kStep[int(from)];
            const bool in = x + sx >= 0 && x + sx < W && y + sy >= 0 && y + sy < H && full(x + sx, y + sy);
            const bool behind = empty(x - sx, 2 * (y - sy)) || empty(x - sx, 2 * (y - sy) + 1);
            return !(in && behind);
        };
        const Canvas c = drawn(W, H, px);
        Pixels flipX, flipY;
        for (const auto& [x, py] : on) {
            flipX.push_back({W - 1 - x, py});
            flipY.push_back({x, 2 * H - 1 - py});
        }
        const Canvas cx = drawn(W, H, flipX), cy = drawn(W, H, flipY);
        for (const Compass from : kCompass) {
            const std::string where = "shape " + std::to_string(n) + " light " + std::to_string(int(from));
            const std::set<Point> lit = region(c, from);
            std::set<Point> expected, mx, my;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    if (full(x, y) && edge(x, y)) {
                        turned += int(faces(x, y, from, true) != faces(x, y, from, false));
                        if (!faces(x, y, from, true))
                            continue;
                        if (!outsideTowards(on, W, H, x, y, from))
                            ++hidden;
                        else if (open(x, y, from))
                            expected.insert({x, y});
                        else
                            ++held;
                    }
            for (const auto& [x, y] : lit) {
                if (!faces(x, y, from, true))
                    bad.push_back(where + ": (" + std::to_string(x) + "," + std::to_string(y) + ") faces away");
                // checked apart from the rule: black beside it towards the light
                if (!outsideTowards(on, W, H, x, y, from))
                    bad.push_back(where + ": (" + std::to_string(x) + "," + std::to_string(y) +
                                  ") has no outside beside it towards the light");
                mx.insert({W - 1 - x, y});
                my.insert({x, H - 1 - y});
            }
            if (lit != expected)
                bad.push_back(where + ": not the full edge cells facing the light");
            if (region(cx, mirrored(from, true)) != mx)
                bad.push_back(where + ": mirrored left to right differs");
            if (region(cy, mirrored(from, false)) != my)
                bad.push_back(where + ": mirrored top to bottom differs");
            litTotal += int(lit.size());
        }
    }
    INFO((bad.empty() ? std::string() : bad.front()) << " (" << bad.size() << " in all)");
    CHECK(bad.empty());
    CHECK(litTotal > 2000);
    CHECK(held > 50);
    // cells facing the light with no outside beside them on its side
    CHECK(hidden > 50);
    // cells whose facing the outside that doesn't reach them would change
    CHECK(turned > 50);
}

namespace {

// A steel w cell, both pixels drawn.
Cell fullW() {
    Canvas t(1, 1);
    plotPixels(t, {{0, 0}, {0, 1}}, 7).apply(t);
    return t.at(0, 0);
}

// A ▌ or ▐ fringe: its coloured half steel w, the other half black.
Cell side(char32_t ch) { return Cell{ch, Color::indexed(Grey), Color::indexed(Black)}; }

// The lit cells in column x.
std::set<Point> column(const std::set<Point>& lit, int x) {
    std::set<Point> out;
    for (const Point& p : lit)
        if (p.first == x)
            out.insert(p);
    return out;
}

} // namespace

TEST_CASE("a ▌ or ▐ fringe's black half is outside", "[detail]") {
    // Codex round 8: the ▌ above (5,3) has its black right half outside, so
    // the cell under it faces up and lights from N. With both halves of a
    // ▌/▐ counted inside, the normal summed to nothing
    for (const char32_t ch : {U'▌', U'▐'}) {
        INFO("glyph " << int(ch));
        Canvas c(11, 7);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 11; ++x)
                c.at(x, y) = rungCell(steel(), 8);
        c.at(5, 0) = Cell{};
        c.at(5, 1) = Cell{};
        c.at(5, 2) = side(ch);
        const CellEdit e = paintStroke(c, RimLightBrush(steel(), Compass::N), {}, {{5, 3}});
        CHECK(written(e) == std::map<Point, Cell>{{{5, 3}, rungCell(steel(), 9)}});
    }
}

TEST_CASE("a shape within three columns of the canvas's side edge can light differently", "[detail]") {
    // Claude round 8 P3-1 (a), Claude and Codex round 9 P3-1: off the canvas
    // is not outside, and the normal's window reaches three columns, so a
    // shape whose cells lie within three columns of the left or right edge
    // can light differently from the same shape further in, where the
    // window finds outside cells. A shape the edge cuts off has no rim
    // there.
    const Cell w = fullW();
    using Lit = std::map<Compass, std::set<Point>>;
    const auto lights = [](const Canvas& c) {
        Lit out;
        for (const Compass from : kCompass)
            out[from] = region(c, from);
        return out;
    };
    // the cells of `lit` moved `dx` columns
    const auto moved = [](const std::set<Point>& lit, int dx) {
        std::set<Point> out;
        for (const auto& [x, y] : lit)
            out.insert({x + dx, y});
        return out;
    };
    // ▐ fringes on column `fringe`, full cells on `first`-`last`, rows 1-4
    const auto shape = [&](char32_t ch, int fringe, int first, int last) {
        Canvas c(12, 6);
        for (int y = 1; y <= 4; ++y) {
            c.at(fringe, y) = side(ch);
            for (int x = first; x <= last; ++x)
                c.at(x, y) = w;
        }
        return c;
    };
    // (a) the round-8 shape, fringes on column 0, against the same shape a
    // column in (columns 1 to 4 in light alike). N, E, S, SW and NW light
    // alike; W, NE and SE differ at four cells, (1,1), (1,4), (2,1) and (2,4)
    const Canvas left = shape(U'▐', 0, 1, 6), leftIn = shape(U'▐', 1, 2, 7);
    const Lit a = lights(left), in = lights(leftIn);
    for (const Compass from : {Compass::N, Compass::E, Compass::S, Compass::SW, Compass::NW}) {
        INFO("light " << int(from));
        CHECK(a.at(from) == moved(in.at(from), -1));
    }
    CHECK(a.at(Compass::W) == std::set<Point>{{1, 2}, {1, 3}});
    CHECK(moved(in.at(Compass::W), -1) == std::set<Point>{{1, 1}, {1, 2}, {1, 3}, {1, 4}, {2, 1}, {2, 4}});
    // Codex round 9: (2,1) is lit from W only a column in
    CHECK_FALSE(a.at(Compass::W).count({2, 1}));
    CHECK(in.at(Compass::W).count({3, 1}));
    CHECK(a.at(Compass::NE) == std::set<Point>{{1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}, {6, 1}, {6, 2}, {6, 3}});
    CHECK(moved(in.at(Compass::NE), -1) == std::set<Point>{{3, 1}, {4, 1}, {5, 1}, {6, 1}, {6, 2}, {6, 3}});
    CHECK(a.at(Compass::SE) == std::set<Point>{{1, 4}, {2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 2}, {6, 3}, {6, 4}});
    CHECK(moved(in.at(Compass::SE), -1) == std::set<Point>{{3, 4}, {4, 4}, {5, 4}, {6, 2}, {6, 3}, {6, 4}});
    // the shape on the right edge is the mirror image, and lights mirrored
    const Canvas right = shape(U'▌', 11, 5, 10);
    for (const Compass from : kCompass) {
        INFO("light " << int(from));
        std::set<Point> m;
        for (const auto& [x, y] : a.at(from))
            m.insert({11 - x, y});
        CHECK(region(right, mirrored(from, true)) == m);
    }
    // (b) Codex round 9: three full columns. On column 1 the middle cells'
    // window finds the outside east of the shape and none west of it, so
    // they face E; a column in they face W. The rim normal reads only the
    // outside that reaches them, the ▐'s black half, so they face W at the
    // edge too (#54) and light from the west as a column in does; they
    // aren't lit from E (the cell a step east is in the shape and the
    // fringe a step west is behind them). Their corners, whose outside runs
    // round the shape's top and bottom to its east side, still light
    // differently
    const Canvas left3 = shape(U'▐', 0, 1, 3), left3In = shape(U'▐', 1, 2, 4);
    REQUIRE(edgeNormal(left3, steel(), 1, 2));
    REQUIRE(edgeNormal(left3In, steel(), 2, 2));
    CHECK(edgeNormal(left3, steel(), 1, 2)->first > 0);
    CHECK(edgeNormal(left3In, steel(), 2, 2)->first < 0);
    CHECK(rimNormal(left3, steel(), 1, 2) == std::pair{-1.0, 0.0});
    CHECK(rimNormal(left3In, steel(), 2, 2) == std::pair{-1.0, 0.0});
    CHECK(lights(left3) == Lit{{Compass::N, {{1, 1}, {2, 1}, {3, 1}}},
                               {Compass::NE, {{1, 1}, {2, 1}, {3, 1}, {3, 2}, {3, 3}}},
                               {Compass::E, {{2, 1}, {2, 4}, {3, 1}, {3, 2}, {3, 3}, {3, 4}}},
                               {Compass::SE, {{1, 4}, {2, 4}, {3, 2}, {3, 3}, {3, 4}}},
                               {Compass::S, {{1, 4}, {2, 4}, {3, 4}}},
                               {Compass::SW, {{1, 2}, {1, 3}}},
                               {Compass::W, {{1, 2}, {1, 3}}},
                               {Compass::NW, {{1, 2}, {1, 3}}}});
    CHECK(lights(left3In) == Lit{{Compass::N, {{2, 1}, {3, 1}, {4, 1}}},
                                 {Compass::NE, {{2, 1}, {3, 1}, {4, 1}, {4, 2}, {4, 3}}},
                                 {Compass::E, {{4, 1}, {4, 2}, {4, 3}, {4, 4}}},
                                 {Compass::SE, {{2, 4}, {3, 4}, {4, 2}, {4, 3}, {4, 4}}},
                                 {Compass::S, {{2, 4}, {3, 4}, {4, 4}}},
                                 {Compass::SW, {{2, 2}, {2, 3}, {2, 4}, {3, 4}}},
                                 {Compass::W, {{2, 1}, {2, 2}, {2, 3}, {2, 4}}},
                                 {Compass::NW, {{2, 1}, {2, 2}, {2, 3}, {3, 1}}}});
    // (c) Claude round 9: a stem a column wide, rows 1-6, full cells. On
    // column 1 or 2 its window finds outside only to the east, and it is lit
    // down its length from the east; from column 3 in, only its ends light
    const auto stem = [&](int x) {
        Canvas c(14, 8);
        for (int y = 1; y <= 6; ++y)
            c.at(x, y) = w;
        return lights(c);
    };
    const auto run = [](int x, int y0, int y1) {
        std::set<Point> out;
        for (int y = y0; y <= y1; ++y)
            out.insert({x, y});
        return out;
    };
    CHECK(stem(1) == Lit{{Compass::N, {}},
                         {Compass::NE, run(1, 1, 6)},
                         {Compass::E, run(1, 1, 6)},
                         {Compass::SE, run(1, 1, 6)},
                         {Compass::S, {}},
                         {Compass::SW, {}},
                         {Compass::W, {}},
                         {Compass::NW, {}}});
    CHECK(stem(2) == Lit{{Compass::N, {{2, 1}}},
                         {Compass::NE, run(2, 1, 5)},
                         {Compass::E, run(2, 1, 6)},
                         {Compass::SE, run(2, 2, 6)},
                         {Compass::S, {{2, 6}}},
                         {Compass::SW, {}},
                         {Compass::W, {}},
                         {Compass::NW, {}}});
    for (const int x : {3, 6}) {
        INFO("column " << x);
        CHECK(stem(x) == Lit{{Compass::N, {{x, 1}}},
                             {Compass::NE, {{x, 1}}},
                             {Compass::E, {}},
                             {Compass::SE, {{x, 6}}},
                             {Compass::S, {{x, 6}}},
                             {Compass::SW, {{x, 6}}},
                             {Compass::W, {}},
                             {Compass::NW, {{x, 1}}}});
    }
}

TEST_CASE("two shapes split by a ▌ light their facing edges as with an empty column", "[detail]") {
    // Claude round 8 P3-1 (b): a ▌ column (its right half black) between
    // two shapes is a half-column gap. The edges either side of it, columns
    // 5 and 7, light from every point as they do across an empty column
    const Cell w = fullW();
    const auto split = [&](const Cell& gap) {
        Canvas c(16, 6);
        for (int y = 1; y <= 4; ++y) {
            for (int x = 2; x <= 5; ++x)
                c.at(x, y) = w;
            c.at(6, y) = gap;
            for (int x = 7; x <= 12; ++x)
                c.at(x, y) = w;
        }
        return c;
    };
    const Canvas fringe = split(side(U'▌')), empty = split(Cell{});
    for (const Compass from : kCompass) {
        INFO("light " << int(from));
        const std::set<Point> a = region(fringe, from), b = region(empty, from);
        CHECK(column(a, 5) == column(b, 5));
        CHECK(column(a, 7) == column(b, 7));
    }
    CHECK(column(region(fringe, Compass::W), 7) == std::set<Point>{{7, 2}, {7, 3}});
    CHECK(column(region(fringe, Compass::E), 5) == std::set<Point>{{5, 2}, {5, 3}});
}

TEST_CASE("the column of a narrow shape that faces a gap isn't lit from behind", "[detail]") {
    // Claude round 9 P2-1: a shape two columns wide a column from another
    // (three wide across a ▌/▐ half-column gap) sees more outside on its far
    // side, within the window's three columns, than in the gap, so the
    // normal of its column beside the gap points across the shape. That
    // column was lit from the far side. The rim normal reads only the
    // outside that reaches the cell, the gap's (#54), so the column faces
    // the gap and is lit from the gap's side. And a cell isn't lit when the
    // cell a step towards the light is a full shape cell and the one a step
    // away is outside or a fringe
    const Cell w = fullW();
    using Lit = std::map<Compass, std::set<Point>>;
    // shapes on rows 1-8: columns 8-9, the gap on column 10, and `rw`
    // columns from 11
    const auto make = [&](const Cell& gap, int rw) {
        Canvas c(24, 10);
        for (int y = 1; y <= 8; ++y) {
            for (int x = 8; x <= 9; ++x)
                c.at(x, y) = w;
            c.at(10, y) = gap;
            for (int x = 11; x < 11 + rw; ++x)
                c.at(x, y) = w;
        }
        return c;
    };
    const auto columnLit = [](const Canvas& c, int x) {
        Lit out;
        for (const Compass from : kCompass)
            out[from] = column(region(c, from), x);
        return out;
    };
    // (a) full cells, an empty column between shapes two wide. The window
    // normal of column 11 still points E, across the shape; its rim normal
    // points W, into the gap. It isn't lit from E: its middle cells light
    // from W, NW and SW, and its end cells from the points that have the
    // outside above or below them. Column 9 is its mirror
    const Canvas a = make(Cell{}, 2);
    CHECK(edgeNormal(a, steel(), 11, 4) == std::pair{1.0, 0.0});
    CHECK(rimNormal(a, steel(), 11, 4) == std::pair{-1.0, 0.0});
    CHECK(rimNormal(a, steel(), 9, 4) == std::pair{1.0, 0.0});
    const auto run = [](int x, int y0, int y1) {
        std::set<Point> out;
        for (int y = y0; y <= y1; ++y)
            out.insert({x, y});
        return out;
    };
    CHECK(columnLit(a, 11) == Lit{{Compass::N, {{11, 1}}},
                                  {Compass::NE, {{11, 1}}},
                                  {Compass::E, {}},
                                  {Compass::SE, {{11, 8}}},
                                  {Compass::S, {{11, 8}}},
                                  {Compass::SW, run(11, 2, 7)},
                                  {Compass::W, run(11, 2, 7)},
                                  {Compass::NW, run(11, 2, 7)}});
    CHECK(columnLit(a, 9) == Lit{{Compass::N, {{9, 1}}},
                                 {Compass::NE, run(9, 2, 7)},
                                 {Compass::E, run(9, 2, 7)},
                                 {Compass::SE, run(9, 2, 7)},
                                 {Compass::S, {{9, 8}}},
                                 {Compass::SW, {{9, 8}}},
                                 {Compass::W, {}},
                                 {Compass::NW, {{9, 1}}}});
    // the columns on the outer sides light as a column's side does
    for (int y = 1; y <= 8; ++y) {
        CHECK(region(a, Compass::E).count({12, y}));
        CHECK(region(a, Compass::W).count({8, y}));
    }
    // (b) a ▌ or ▐ half-column gap between a shape two wide and one three
    // wide: column 11's window normal faces E across the shape, its rim
    // normal W into the gap, and it isn't lit from E. It and column 9 light
    // as they do across an empty column
    const Canvas blank = make(Cell{}, 3);
    const Lit gapSide{{Compass::N, {{11, 1}}},  {Compass::NE, {{11, 1}}}, {Compass::E, {}},
                      {Compass::SE, {{11, 8}}}, {Compass::S, {{11, 8}}},  {Compass::SW, run(11, 2, 8)},
                      {Compass::W, run(11, 2, 7)}, {Compass::NW, run(11, 1, 7)}};
    CHECK(columnLit(blank, 11) == gapSide);
    for (const char32_t ch : {U'▌', U'▐'}) {
        INFO("glyph " << int(ch));
        const Canvas b = make(side(ch), 3);
        CHECK(edgeNormal(b, steel(), 11, 4) == std::pair{1.0, 0.0});
        CHECK(rimNormal(b, steel(), 11, 4) == std::pair{-1.0, 0.0});
        CHECK(columnLit(b, 11) == gapSide);
        CHECK(columnLit(b, 9) == columnLit(blank, 9));
    }
    // (c) a stem a column wide far from the canvas edges: it cancels down
    // its length, and only its ends light
    Canvas stem(24, 10);
    for (int y = 1; y <= 8; ++y)
        stem.at(10, y) = w;
    CHECK(columnLit(stem, 10) == Lit{{Compass::N, {{10, 1}}},
                                     {Compass::NE, {{10, 1}}},
                                     {Compass::E, {}},
                                     {Compass::SE, {{10, 8}}},
                                     {Compass::S, {{10, 8}}},
                                     {Compass::SW, {{10, 8}}},
                                     {Compass::W, {}},
                                     {Compass::NW, {{10, 1}}}});
}

TEST_CASE("a counter's inner wall faces the counter", "[detail]") {
    // Claude round 10 P2-1 (#54): a block "o", full cells on columns 10-14
    // and rows 3-5 with (12,4) empty. The only open side of (13,4) is W,
    // the counter. Its window normal points E across the side stroke, and
    // it was lit from NE and SE; its rim normal reads only the counter and
    // points W, so it lights from W, NW and SW and from nowhere else. (11,4)
    // mirrors it
    const Cell w = fullW();
    Canvas c(24, 8);
    for (int y = 3; y <= 5; ++y)
        for (int x = 10; x <= 14; ++x)
            c.at(x, y) = w;
    c.at(12, 4) = Cell{};
    CHECK(edgeNormal(c, steel(), 13, 4) == std::pair{1.0, 0.0});
    CHECK(rimNormal(c, steel(), 13, 4) == std::pair{-1.0, 0.0});
    CHECK(rimNormal(c, steel(), 11, 4) == std::pair{1.0, 0.0});
    using Lit = std::map<Compass, std::set<Point>>;
    Lit lit;
    for (const Compass from : kCompass)
        lit[from] = region(c, from);
    CHECK(lit == Lit{{Compass::N, {{10, 3}, {11, 3}, {12, 3}, {13, 3}, {14, 3}}},
                     {Compass::NE, {{11, 4}, {12, 3}, {13, 3}, {14, 3}, {14, 4}}},
                     {Compass::E, {{11, 4}, {13, 3}, {13, 5}, {14, 3}, {14, 4}, {14, 5}}},
                     {Compass::SE, {{11, 4}, {12, 5}, {13, 5}, {14, 4}, {14, 5}}},
                     {Compass::S, {{10, 5}, {11, 5}, {12, 5}, {13, 5}, {14, 5}}},
                     {Compass::SW, {{10, 4}, {10, 5}, {11, 5}, {12, 5}, {13, 4}}},
                     {Compass::W, {{10, 3}, {10, 4}, {10, 5}, {11, 3}, {11, 5}, {13, 4}}},
                     {Compass::NW, {{10, 3}, {10, 4}, {11, 3}, {12, 3}, {13, 4}}}});
}

TEST_CASE("no ring is lit on a side that faces only away from the light", "[detail]") {
    // Claude round 10 (#54): block rings, side strokes one to three columns
    // wide and top and bottom strokes one or two rows, round counters one
    // to three columns wide and one or two rows tall, lit from all eight
    // points. A lit cell has some outside among its eight neighbours that
    // isn't wholly behind it: a neighbour a step (dx, dy) away with
    // dx·sx + dy·sy ≥ 0 for the light's step (sx, sy). The old window
    // normal lit the counter's corner cells from the far diagonal
    const Cell w = fullW();
    int rings = 0, lit = 0;
    for (int side = 1; side <= 3; ++side)
        for (int bar = 1; bar <= 2; ++bar)
            for (int cw = 1; cw <= 3; ++cw)
                for (int ch = 1; ch <= 2; ++ch) {
                    const int x0 = 8, y0 = 2, x1 = x0 + 2 * side + cw - 1, y1 = y0 + 2 * bar + ch - 1;
                    Canvas c(x1 + 9, y1 + 3);
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                            if (x < x0 + side || x > x1 - side || y < y0 + bar || y > y1 - bar)
                                c.at(x, y) = w;
                    ++rings;
                    for (const Compass from : kCompass) {
                        const auto [sx, sy] = kStep[int(from)];
                        for (const auto& [x, y] : region(c, from)) {
                            INFO("side " << side << " bar " << bar << " counter " << cw << "x" << ch << " light "
                                         << int(from) << " cell " << x << "," << y);
                            bool open = false;
                            for (int dy = -1; dy <= 1; ++dy)
                                for (int dx = -1; dx <= 1; ++dx)
                                    open = open || (c.at(x + dx, y + dy) == Cell{} && dx * sx + dy * sy >= 0);
                            CHECK(open);
                            ++lit;
                        }
                    }
                }
    CHECK(rings == 36);
    CHECK(lit > 1000);
}

TEST_CASE("a thin bar over another lights its underside from below", "[detail]") {
    // Codex round 10 #2 (#54): two bars three pixels tall, ▄ fringes over a
    // full row each, one black pixel row apart. The upper bar's full row is
    // its underside, but its window sees more outside above than in the one
    // black pixel row below, so its window normal faces N, and (10,2) was
    // lit from N. The outside above lies past the ▄ fringes' coloured
    // halves and doesn't reach the row; the black pixel row under it does,
    // so its rim normal faces S and it lights from S, not N
    const Ramp r = steel();
    const Cell lower{kLowerHalf, Color::indexed(Grey), Color::indexed(Black)};
    Canvas c(24, 6);
    for (int x = 2; x <= 21; ++x) {
        c.at(x, 1) = lower;
        c.at(x, 2) = rungCell(r, 8);
        c.at(x, 3) = lower;
        c.at(x, 4) = rungCell(r, 8);
    }
    CHECK(edgeNormal(c, r, 10, 2) == std::pair{0.0, -1.0});
    CHECK(rimNormal(c, r, 10, 2) == std::pair{0.0, 1.0});
    // the row lights from S along its length, and from N nowhere; the
    // outside round the bar's ends reaches the two cells at each end, which
    // light as ends do
    for (int x = 2; x <= 21; ++x) {
        INFO("cell " << x << ",2");
        CHECK_FALSE(region(c, Compass::N).count({x, 2}));
        CHECK(region(c, Compass::S).count({x, 2}) == (x >= 4 && x <= 19));
    }
    // without the lower bar the same cell faces down, as it did
    Canvas alone = c;
    for (int x = 2; x <= 21; ++x)
        alone.at(x, 3) = alone.at(x, 4) = Cell{};
    CHECK(edgeNormal(alone, r, 10, 2) == std::pair{0.0, 1.0});
    CHECK(rimNormal(alone, r, 10, 2) == std::pair{0.0, 1.0});
    CHECK_FALSE(region(alone, Compass::N).count({10, 2}));
    CHECK(region(alone, Compass::S).count({10, 2}));
}

TEST_CASE("the rim light needs outside beside the cell on the light's side", "[detail]") {
    // Codex round 11 #1 (#54): an ellipse with a rectangle beside it. Of
    // (5,3)'s eight neighbours only (6,4), a ▀ whose bottom half is black,
    // has outside in it, and that lies down and to the right of the cell:
    // nothing beside it is open to a light from the N, NE, W or NW. Its
    // rim normal, over the outside its neighbours reach, turned NE and it
    // was lit from NE
    const auto shape = [](int ex0, int ey0, int ex1, int ey1, int rx0, int ry0, int rx1, int ry1) {
        Pixels px = ellipseShape(ex0, ey0, ex1, ey1, true, 13, 14);
        const Pixels rect = rectShape(rx0, ry0, rx1, ry1, true, 13, 14);
        px.insert(px.end(), rect.begin(), rect.end());
        return px;
    };
    const Pixels px = shape(3, 3, 6, 9, 8, 7, 10, 9);
    const Canvas c = drawn(13, 7, px);
    CHECK_FALSE(region(c, Compass::NE).count({5, 3}));
    for (const Compass from : {Compass::N, Compass::NE, Compass::W, Compass::NW}) {
        INFO("light " << int(from));
        CHECK_FALSE(region(c, from).count({5, 3}));
    }
    CHECK(litFromBehind(px, 13, 7).empty());
    // mirrored left to right, top to bottom and both, lit from the
    // mirrored points: the mirrored cell isn't lit either
    for (const int m : {1, 2, 3}) {
        Pixels flipped;
        for (const auto& [x, py] : px)
            flipped.push_back({m & 1 ? 12 - x : x, m & 2 ? 13 - py : py});
        const Canvas f = drawn(13, 7, flipped);
        const Point cell{m & 1 ? 12 - 5 : 5, m & 2 ? 6 - 3 : 3};
        for (Compass from : {Compass::N, Compass::NE, Compass::W, Compass::NW}) {
            if (m & 1)
                from = mirrored(from, true);
            if (m & 2)
                from = mirrored(from, false);
            INFO("mirror " << m << " light " << int(from));
            CHECK_FALSE(region(f, from).count(cell));
        }
        INFO("mirror " << m);
        CHECK(litFromBehind(flipped, 13, 7).empty());
    }
    // the same from the other points of the compass: each cell's only
    // outside beside it lies away from the light. (8,6) has black only in
    // the lower halves of the ▀ either side of it, (8,0) only in the upper
    // halves of the ▀ either side, (0,2) only above it, (5,4) only above
    // and to its right, (2,2) only below and to its right
    struct Case {
        int ex0, ey0, ex1, ey1, rx0, ry0, rx1, ry1;
        Point cell;
        Compass from;
    };
    for (const Case& k : {Case{6, 1, 10, 13, 6, 9, 7, 10, {8, 6}, Compass::N},
                          Case{7, 0, 9, 8, 9, 9, 11, 12, {8, 0}, Compass::S},
                          Case{4, 8, 12, 13, 0, 3, 1, 13, {0, 2}, Compass::SE},
                          Case{4, 6, 12, 13, 4, 8, 6, 11, {5, 4}, Compass::SW},
                          Case{1, 1, 4, 7, 2, 4, 6, 10, {2, 2}, Compass::NW}}) {
        const Pixels p = shape(k.ex0, k.ey0, k.ex1, k.ey1, k.rx0, k.ry0, k.rx1, k.ry1);
        const std::set<Point> on(p.begin(), p.end());
        INFO("cell " << k.cell.first << "," << k.cell.second << " light " << int(k.from));
        REQUIRE(onEdge(drawn(13, 7, p), steel(), k.cell.first, k.cell.second));
        REQUIRE_FALSE(outsideTowards(on, 13, 7, k.cell.first, k.cell.second, k.from));
        CHECK_FALSE(region(drawn(13, 7, p), k.from).count(k.cell));
        CHECK(litFromBehind(p, 13, 7) == "");
    }
    // and over 3000 random pairs of a filled ellipse and a filled rectangle
    std::mt19937 rng(54);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    std::vector<std::string> bad;
    for (int n = 0; n < 3000; ++n) {
        const int a0 = pick(0, 10), a1 = pick(a0 + 1, 12), b0 = pick(0, 11), b1 = pick(b0 + 2, 13);
        const int c0 = pick(0, 10), c1 = pick(c0, 12), d0 = pick(0, 12), d1 = pick(d0, 13);
        const std::string at = litFromBehind(shape(a0, b0, a1, b1, c0, d0, c1, d1), 13, 7);
        if (!at.empty())
            bad.push_back("pair " + std::to_string(n) + ": " + at);
    }
    INFO((bad.empty() ? std::string() : bad.front()) << " (" << bad.size() << " in all)");
    CHECK(bad.empty());
}

TEST_CASE("a thin bar's underside two columns in from its ends faces down", "[detail]") {
    // Codex round 11 #2 (#54): the two bars three pixels tall of the test
    // above, one to twelve columns long, the lower one shifted up to two
    // columns either way or left out. A cell of the upper bar's full row
    // has the ▄ fringe's coloured half on top of it and the black pixel
    // row (or an empty cell) under it: it is the bar's underside. Two
    // columns or more in from both of the bar's ends, the outside past the
    // fringes above joins the outside below only beyond the cell's window,
    // so none of those cells faces up: none is lit from N, NE or NW, and
    // each is lit from S. Nearer the ends is a known limit (below)
    const Ramp r = steel();
    const Cell lower{kLowerHalf, Color::indexed(Grey), Color::indexed(Black)};
    int checked = 0;
    for (int len = 1; len <= 12; ++len)
        for (int shift = -2; shift <= 3; ++shift) {
            Canvas c(24, 7);
            for (int x = 6; x < 6 + len; ++x) {
                c.at(x, 1) = lower;
                c.at(x, 2) = rungCell(r, 8);
            }
            if (shift <= 2)  // 3: no lower bar
                for (int x = 6 + shift; x < 6 + shift + len; ++x) {
                    c.at(x, 3) = lower;
                    c.at(x, 4) = rungCell(r, 8);
                }
            const std::set<Point> n = region(c, Compass::N), s = region(c, Compass::S),
                                  ne = region(c, Compass::NE), nw = region(c, Compass::NW);
            for (int x = 8; x < 4 + len; ++x) {
                INFO("length " << len << " shift " << shift << " cell " << x << ",2");
                CHECK_FALSE(n.count({x, 2}));
                CHECK_FALSE(ne.count({x, 2}));
                CHECK_FALSE(nw.count({x, 2}));
                CHECK(s.count({x, 2}));
                ++checked;
            }
        }
    CHECK(checked == 6 * (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8));
}

TEST_CASE("known limit: a thin bar's underside by its ends can face up", "[detail]") {
    // Codex round 11 #2 (#54), a known limit: the same two bars, three
    // columns long, one over the other. (6,2), a column in from both ends,
    // has outside touching it only below, in the lower ▄ fringes' black
    // halves, but within its window that outside joins the outside past the
    // upper fringes round both ends of the bar, so all of it reaches the
    // cell. Its rim normal sums to straight up: it is lit from N, NE and NW
    // and not from S, where the cells of a longer bar two columns or more
    // from its ends light from the south (S, with no N, NE or NW). The end cells face out sideways
    const Ramp r = steel();
    const Cell lower{kLowerHalf, Color::indexed(Grey), Color::indexed(Black)};
    Canvas c(13, 7);
    for (int x = 5; x <= 7; ++x) {
        c.at(x, 1) = c.at(x, 3) = lower;
        c.at(x, 2) = c.at(x, 4) = rungCell(r, 8);
    }
    using N = std::pair<double, double>;
    CHECK(rimNormal(c, r, 6, 2) == N{0, -1});
    const std::map<Compass, std::set<Point>> lit = {
        {Compass::N, {{6, 2}}},
        {Compass::NE, {{6, 2}, {7, 2}, {7, 4}}},
        {Compass::E, {{7, 2}, {7, 4}}},
        {Compass::SE, {{7, 2}, {6, 4}, {7, 4}}},
        {Compass::S, {{5, 4}, {6, 4}, {7, 4}}},
        {Compass::SW, {{5, 2}, {5, 4}, {6, 4}}},
        {Compass::W, {{5, 2}, {5, 4}}},
        {Compass::NW, {{5, 2}, {6, 2}, {5, 4}}},
    };
    for (const auto& [from, cells] : lit) {
        INFO("light " << int(from));
        CHECK(region(c, from) == cells);
    }
}

TEST_CASE("known limit: a grazing cell on a rotated ellipse isn't lit", "[detail]") {
    // Codex round 10 #1, a known limit: an ellipse with semi-axes 9 and 3
    // columns turned 35° on screen, sampled on the sub-pixel grid. All the
    // outside in (5,3)'s window reaches it, so its rim normal is its window
    // normal, which sums to (-21, 10.5), a dot of 1/√10 ≈ 0.316 with NW: it
    // faces NW. But (4,2) is full and (6,4) a ▀ fringe, so the rule against
    // light from behind, which checks only that pair of neighbours, takes
    // it for a cell lit from behind and leaves it. The analytic normal of the ellipse where its edge crosses the
    // cell has a dot of about 0.17 with NW, under the 0.3 the brush needs,
    // so leaving it unlit is defensible
    const Ramp r = steel();
    const char* const rows[] = {"........................", "........................", "....####ddd.............",
                                ".....#######dd..........", "......u########d........", "........u########d......",
                                "...........u#######.....", "..............uuuuu.....", "........................",
                                "........................"};
    Canvas c(24, 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 24; ++x) {
            const char ch = rows[y][x];
            if (ch == '#')
                c.at(x, y) = rungCell(r, 8);
            else if (ch == 'u' || ch == 'd')
                c.at(x, y) = Cell{ch == 'u' ? kUpperHalf : kLowerHalf, Color::indexed(Grey), Color::indexed(Black)};
        }
    const auto n = edgeNormal(c, r, 5, 3);
    REQUIRE(n);
    CHECK(std::abs(n->first + 2 / std::sqrt(5.0)) < 1e-9);
    CHECK(std::abs(n->second - 1 / std::sqrt(5.0)) < 1e-9);
    CHECK(rimNormal(c, r, 5, 3) == n);
    CHECK(RimLightBrush::facesLight(*n, Compass::NW));
    CHECK(paintStroke(c, RimLightBrush(r, Compass::NW), {}, {{5, 3}}).empty());
    CHECK_FALSE(region(c, Compass::NW).count({5, 3}));
}

TEST_CASE("a half-column gap is not an empty column to the normal", "[detail]") {
    // Codex round 9 P3-2: shapes on columns 2-3 and 5-7, rows 1-4, with a
    // ▌ (or ▐) on column 4. The fringe's coloured half is inside, so the
    // window of (5,2) sees less outside to its west than across an empty
    // column: its normal sums to (3.75, 0) with ▌ and (2.25, 0) with ▐, and
    // points E, where an empty column 4 cancels it. Its rim normal reads
    // only the gap, and points W either way (#54): it lights from W, NW and
    // SW, and (3,2) beside it from E, NE and SE. Neither is lit across the
    // shape. The corners of the narrow shapes still light differently
    const Cell w = fullW();
    const auto make = [&](const Cell& gap) {
        Canvas c(12, 6);
        for (int y = 1; y <= 4; ++y) {
            for (const int x : {2, 3, 5, 6, 7})
                c.at(x, y) = w;
            c.at(4, y) = gap;
        }
        return c;
    };
    using Lit = std::map<Compass, std::set<Point>>;
    const auto lights = [](const Canvas& c) {
        Lit out;
        for (const Compass from : kCompass)
            out[from] = region(c, from);
        return out;
    };
    const Lit fringe{{Compass::N, {{2, 1}, {3, 1}, {5, 1}, {6, 1}, {7, 1}}},
                     {Compass::NE, {{3, 2}, {3, 3}, {5, 1}, {6, 1}, {7, 1}, {7, 2}, {7, 3}}},
                     {Compass::E, {{3, 2}, {3, 3}, {6, 1}, {6, 4}, {7, 1}, {7, 2}, {7, 3}, {7, 4}}},
                     {Compass::SE, {{3, 2}, {3, 3}, {5, 4}, {6, 4}, {7, 2}, {7, 3}, {7, 4}}},
                     {Compass::S, {{2, 4}, {3, 4}, {5, 4}, {6, 4}, {7, 4}}},
                     {Compass::SW, {{2, 2}, {2, 3}, {2, 4}, {3, 4}, {5, 2}, {5, 3}, {5, 4}}},
                     {Compass::W, {{2, 1}, {2, 2}, {2, 3}, {2, 4}, {5, 2}, {5, 3}}},
                     {Compass::NW, {{2, 1}, {2, 2}, {2, 3}, {3, 1}, {5, 1}, {5, 2}, {5, 3}}}};
    for (const char32_t ch : {U'▌', U'▐'}) {
        INFO("glyph " << int(ch));
        const Canvas c = make(side(ch));
        CHECK(edgeNormal(c, steel(), 5, 2) == std::pair{1.0, 0.0});
        CHECK(rimNormal(c, steel(), 5, 2) == std::pair{-1.0, 0.0});
        CHECK(rimNormal(c, steel(), 3, 2) == std::pair{1.0, 0.0});
        CHECK(lights(c) == fringe);
    }
    const Canvas blank = make(Cell{});
    CHECK(onEdge(blank, steel(), 5, 2));
    CHECK_FALSE(edgeNormal(blank, steel(), 5, 2));
    CHECK(rimNormal(blank, steel(), 5, 2) == std::pair{-1.0, 0.0});
    CHECK(lights(blank) == Lit{{Compass::N, {{2, 1}, {3, 1}, {5, 1}, {6, 1}, {7, 1}}},
                               {Compass::NE, {{2, 1}, {3, 2}, {3, 3}, {5, 1}, {6, 1}, {7, 1}, {7, 2}, {7, 3}}},
                               {Compass::E, {{3, 2}, {3, 3}, {6, 1}, {6, 4}, {7, 1}, {7, 2}, {7, 3}, {7, 4}}},
                               {Compass::SE, {{2, 4}, {3, 2}, {3, 3}, {5, 4}, {6, 4}, {7, 2}, {7, 3}, {7, 4}}},
                               {Compass::S, {{2, 4}, {3, 4}, {5, 4}, {6, 4}, {7, 4}}},
                               {Compass::SW, {{2, 2}, {2, 3}, {2, 4}, {3, 4}, {5, 2}, {5, 3}, {5, 4}, {6, 4}}},
                               {Compass::W, {{2, 1}, {2, 2}, {2, 3}, {2, 4}, {5, 2}, {5, 3}}},
                               {Compass::NW, {{2, 1}, {2, 2}, {2, 3}, {3, 1}, {5, 1}, {5, 2}, {5, 3}, {6, 1}}}});
    // Rectangles 1-5 columns wide either side of a ▌ or ▐ column, 1-8 rows
    // tall: the cells beside the gap light from every point as across an
    // empty column, but for their top and bottom cells, which can light
    // where across an empty column they don't
    int more = 0;
    for (const char32_t ch : {U'▌', U'▐'})
        for (int lw = 1; lw <= 5; ++lw)
            for (int rw = 1; rw <= 5; ++rw)
                for (int h = 1; h <= 8; ++h) {
                    const auto split = [&](const Cell& gap) {
                        Canvas c(24, 12);
                        for (int y = 1; y <= h; ++y) {
                            for (int x = 10 - lw; x < 10; ++x)
                                c.at(x, y) = w;
                            c.at(10, y) = gap;
                            for (int x = 11; x < 11 + rw; ++x)
                                c.at(x, y) = w;
                        }
                        return c;
                    };
                    const Canvas f = split(side(ch)), e = split(Cell{});
                    for (const Compass from : kCompass) {
                        INFO("glyph " << int(ch) << " widths " << lw << "," << rw << " rows " << h << " light "
                                      << int(from));
                        const std::set<Point> lf = region(f, from), le = region(e, from);
                        for (const int x : {9, 11})
                            for (int y = 1; y <= h; ++y) {
                                INFO("cell " << x << "," << y);
                                if (y > 1 && y < h)
                                    CHECK(lf.count({x, y}) == le.count({x, y}));
                                else
                                    CHECK(lf.count({x, y}) >= le.count({x, y}));
                                more += int(lf.count({x, y}) > le.count({x, y}));
                            }
                    }
                }
    CHECK(more == 112);
}

TEST_CASE("full cells and ▀/▄ fringes keep their half-block pixel normals", "[detail]") {
    // The normal is taken on 2 x 2 sub-pixels a cell. On canvases of full
    // cells, ▀/▄ fringes and outside cells it equals, bit for bit, the
    // normal on the half-block pixel grid it replaced: the offsets from the
    // cell's centre to the outside pixels in the window, a pixel a column
    // square, two to a cell
    std::mt19937 rng(8);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const Ramp r = steel();
    int compared = 0;
    for (int n = 0; n < 200; ++n) {
        const int W = pick(3, 14), H = pick(2, 8);
        Canvas c(W, H);
        // which of each cell's two pixels are in the shape
        std::vector<std::pair<bool, bool>> in(W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int k = pick(0, 9);
                Cell& cell = c.at(x, y);
                auto& [top, bottom] = in[y * W + x];
                if (k <= 3) {
                    cell = rungCell(r, pick(1, 15));
                    top = bottom = true;
                } else if (k == 4) {
                    cell = halfBlockCell({Grey, Black});
                    top = true, bottom = false;
                } else if (k == 5) {
                    cell = halfBlockCell({Black, White});
                    top = false, bottom = true;
                } else if (k == 6) {
                    cell = halfBlockCell({DarkGrey, Grey});
                    top = bottom = true;
                } else if (k == 7) {
                    cell = Cell{};
                    top = bottom = false;
                } else if (k == 8) {
                    cell = Cell{U'█', Color::indexed(Red), Color::indexed(Black)};
                    top = bottom = false;
                } else {
                    cell = rungCell(r, 0);
                    top = bottom = false;
                }
            }
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!onEdge(c, r, x, y))
                    continue;
                double gx = 0, gy = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) {
                        if (dx * dx + 4 * dy * dy > 9 || !c.contains(x + dx, y + dy))
                            continue;
                        const auto [top, bottom] = in[(y + dy) * W + x + dx];
                        for (const auto& [i, oy] : {std::pair{top, -0.5}, std::pair{bottom, 0.5}})
                            if (!i) {
                                gx += dx;
                                gy += 2 * dy + oy;
                            }
                    }
                const double len = std::hypot(gx, gy);
                const auto got = edgeNormal(c, r, x, y);
                INFO("canvas " << n << " cell " << x << "," << y);
                REQUIRE(bool(got) == (len >= 1e-9));
                if (got) {
                    CHECK(got->first == gx / len);
                    CHECK(got->second == gy / len);
                    ++compared;
                }
            }
    }
    CHECK(compared > 1000);
}

TEST_CASE("the rim light on random ▌/▐ shapes faces the light and mirrors", "[detail]") {
    // The random shapes above turned a quarter: drawn on a grid two
    // sub-columns a cell and a row tall, so their rims are ▌ and ▐
    // fringes. The lit cells are worked out again from the sub-pixels
    // alone: a full cell with an empty sub-pixel among its eight
    // neighbours' on the canvas, whose normal on the 2 x 2 sub-pixel grid
    // (a sub-pixel half a column wide and a column tall, weighted a half),
    // over the empty sub-pixels in its window that reach it, faces the
    // light, with an empty sub-pixel among its neighbours' on the light's
    // side of it on screen, unless the cell a step towards the light is
    // full while the one a step away has an empty sub-pixel. And the
    // mirrored canvas lights the mirrored cells
    constexpr int W = 20, H = 10, SW = 2 * W;
    std::mt19937 rng(80);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    // shapes drawn H wide and SW tall, then transposed
    const auto box = [&](auto shape) {
        int x0 = pick(-3, H + 2), x1 = pick(-3, H + 2), y0 = pick(-3, SW + 2), y1 = pick(-3, SW + 2);
        return shape(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
    };
    const auto ellipsePx = [](int x0, int y0, int x1, int y1) { return ellipseShape(x0, y0, x1, y1, true, H, SW); };
    const auto rectPx = [](int x0, int y0, int x1, int y1) { return rectShape(x0, y0, x1, y1, true, H, SW); };
    const auto linePx = [](int x0, int y0, int x1, int y1) { return lineShape(x0, y0, x1, y1, H, SW); };
    const Cell w = fullW();
    // the canvas for a set of (sub-column, row) pixels
    const auto draw = [&](const std::set<Point>& on) {
        Canvas c(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool l = on.count({2 * x, y}), r = on.count({2 * x + 1, y});
                c.at(x, y) = l && r ? w : l ? side(U'▌') : r ? side(U'▐') : Cell{};
            }
        return c;
    };
    std::vector<std::string> bad;
    int litTotal = 0, fringes = 0, held = 0, turned = 0, hidden = 0;
    for (int n = 0; n < 500; ++n) {
        Pixels px;
        const int kind = n % 3;
        for (int i = 0, parts = kind == 2 ? pick(2, 4) : 1; i < parts; ++i) {
            const Pixels p = kind == 0 || (kind == 2 && pick(0, 1)) ? box(ellipsePx) : box(rectPx);
            px.insert(px.end(), p.begin(), p.end());
            if (kind == 2 && pick(0, 1)) {
                const Pixels l = box(linePx);
                px.insert(px.end(), l.begin(), l.end());
            }
        }
        std::set<Point> on;
        for (const auto& [a, b] : px)
            on.insert({b, a});
        // sub-pixel (sx, sy) on the canvas and not in the shape
        const auto empty = [&](int sx, int sy) {
            return sx >= 0 && sx < SW && sy >= 0 && sy < 2 * H && !on.count({sx, sy / 2});
        };
        const auto full = [&](int x, int y) { return on.count({2 * x, y}) && on.count({2 * x + 1, y}); };
        // With `reaching`, the normal over only the empty sub-pixels that
        // reach the cell: those joined, by a side or a corner within the
        // window, to one touching the cell's own four; failing any, to one
        // in the cell's eight neighbours
        const auto faces = [&](int x, int y, Compass from, bool reaching) {
            // the window's sub-pixels: row r (0-5) from the top of the row
            // above, column k (0-13) from the left of three cells before
            const auto gap = [&](int r, int k) {
                const int dx = k / 2 - 3, dy = r / 2 - 1;
                return r >= 0 && r < 6 && k >= 0 && k < 14 && dx * dx + 4 * dy * dy <= 9 &&
                       empty(2 * (x - 3) + k, 2 * (y - 1) + r);
            };
            bool reach[6][14] = {};
            const auto seed = [&](int r0, int r1, int k0, int k1) {
                std::vector<Point> todo;
                for (int r = r0; r <= r1; ++r)
                    for (int k = k0; k <= k1; ++k)
                        if (gap(r, k) && !reach[r][k]) {
                            reach[r][k] = true;
                            todo.push_back({r, k});
                        }
                const bool any = !todo.empty();
                while (!todo.empty()) {
                    const auto [r, k] = todo.back();
                    todo.pop_back();
                    for (int rr = r - 1; rr <= r + 1; ++rr)
                        for (int kk = k - 1; kk <= k + 1; ++kk)
                            if (gap(rr, kk) && !reach[rr][kk]) {
                                reach[rr][kk] = true;
                                todo.push_back({rr, kk});
                            }
                }
                return any;
            };
            if (reaching && !seed(1, 4, 5, 8))
                seed(0, 5, 4, 9);
            double gx = 0, gy = 0;
            for (int r = 0; r < 6; ++r)
                for (int k = 0; k < 14; ++k)
                    if (reaching ? reach[r][k] : gap(r, k)) {
                        gx += (k / 2 - 3 + (k % 2 - 0.5) / 2) / 2;
                        gy += (2 * (r / 2 - 1) + r % 2 - 0.5) / 2;
                    }
            const double len = std::hypot(gx, gy);
            const auto [lx, ly] = compassVector(from);
            return len > 1e-9 && (gx * lx + gy * ly) / len > 0.3;
        };
        const auto edge = [&](int x, int y) {
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    for (int s = 0; s < 4; ++s)
                        if (empty(2 * (x + dx) + s % 2, 2 * (y + dy) + s / 2))
                            return true;
            return false;
        };
        // an empty sub-pixel among its neighbours' whose offset from the
        // cell's centre, on screen, has a positive dot with the light
        const auto towards = [&](int x, int y, Compass from) {
            const auto [lx, ly] = compassVector(from);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    for (int k = 0; k < 4; ++k)
                        if ((dx || dy) && empty(2 * (x + dx) + k % 2, 2 * (y + dy) + k / 2) &&
                            (dx + (k % 2 - 0.5) / 2) * lx + (2 * dy + k / 2 - 0.5) * ly > 0)
                            return true;
            return false;
        };
        const auto open = [&](int x, int y, Compass from) {
            const auto [sx, sy] = kStep[int(from)];
            const bool in = x + sx >= 0 && x + sx < W && y + sy >= 0 && y + sy < H && full(x + sx, y + sy);
            bool behind = false;
            for (int s = 0; s < 4; ++s)
                behind = behind || empty(2 * (x - sx) + s % 2, 2 * (y - sy) + s / 2);
            return !(in && behind);
        };
        const Canvas c = draw(on);
        std::set<Point> flipX, flipY;
        for (const auto& [sx, y] : on) {
            flipX.insert({SW - 1 - sx, y});
            flipY.insert({sx, H - 1 - y});
        }
        const Canvas cx = draw(flipX), cy = draw(flipY);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                fringes += int(c.at(x, y).ch == U'▌' || c.at(x, y).ch == U'▐');
        for (const Compass from : kCompass) {
            const std::string where = "shape " + std::to_string(n) + " light " + std::to_string(int(from));
            const std::set<Point> lit = region(c, from);
            std::set<Point> expected, mx, my;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    if (full(x, y) && edge(x, y)) {
                        turned += int(faces(x, y, from, true) != faces(x, y, from, false));
                        if (!faces(x, y, from, true))
                            continue;
                        if (!towards(x, y, from))
                            ++hidden;
                        else if (open(x, y, from))
                            expected.insert({x, y});
                        else
                            ++held;
                    }
            for (const auto& [x, y] : lit) {
                mx.insert({W - 1 - x, y});
                my.insert({x, H - 1 - y});
                if (!towards(x, y, from))
                    bad.push_back(where + ": (" + std::to_string(x) + "," + std::to_string(y) +
                                  ") has no outside beside it towards the light");
            }
            if (lit != expected)
                bad.push_back(where + ": not the full edge cells facing the light");
            if (region(cx, mirrored(from, true)) != mx)
                bad.push_back(where + ": mirrored left to right differs");
            if (region(cy, mirrored(from, false)) != my)
                bad.push_back(where + ": mirrored top to bottom differs");
            litTotal += int(lit.size());
        }
    }
    INFO((bad.empty() ? std::string() : bad.front()) << " (" << bad.size() << " in all)");
    CHECK(bad.empty());
    CHECK(fringes > 2000);
    CHECK(litTotal > 2000);
    CHECK(held > 50);
    CHECK(turned > 50);
    CHECK(hidden > 50);
}

TEST_CASE("the rim light is an accent and not held to the ladder lint", "[detail][lint]") {
    // Codex round 6: PLAN §10 says the rim light lights an edge cell one
    // rung without regard to the lint, which can then find a skip. From E
    // on the blob (rows w, W░w, W▒w, W▓w, W), the rounding at the top and
    // bottom right lights rung 11 to 12 beside rung 10
    const Canvas c = blob();
    REQUIRE(lint::checkLadder(c, steel()).clean());
    BrushSettings s;
    s.size = 12;
    const CellEdit e = paintStroke(c, RimLightBrush(steel(), Compass::E), s, {{4, 2}});
    REQUIRE(e.changes.size() == 2);
    for (const CellChange& ch : e.changes) {
        CHECK(ch.x == 5);
        CHECK((ch.y == 1 || ch.y == 4));
        CHECK(rungOf(steel(), ch.before) == 11);
        CHECK(rungOf(steel(), ch.after) == 12);
    }
    Canvas after = c;
    e.apply(after);
    std::set<Point> skips;
    for (const lint::LadderFinding& f : lint::checkLadder(after, steel()).findings)
        if (f.kind == lint::LadderFinding::Kind::RungSkip)
            skips.insert({f.x, f.y});
    CHECK(skips == std::set<Point>{{4, 1}, {4, 4}});
}

namespace {

// The rim light as it lit before keepLint: the default dab over its cells.
class Accent : public Brush {
public:
    Accent(Ramp ramp, Compass from) : rim_(std::move(ramp), from) {}
    std::string_view name() const override { return "accent"; }
    std::optional<Cell> paintCell(const Stroke& s, int x, int y, double strength) const override {
        return rim_.paintCell(s, x, y, strength);
    }

private:
    RimLightBrush rim_;
};

// What `after` adds to the lint findings of `before` by the lint's rule.
std::vector<std::string> lintAdded(const Canvas& before, const Canvas& after) {
    std::vector<std::pair<int, int>> changed;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x)
            if (!(before.at(x, y) == after.at(x, y)))
                changed.emplace_back(x, y);
    std::vector<std::string> out;
    for (const auto& f : lint::addedFindingsOnce(before, after, steel(), changed))
        out.push_back(f.message);
    return out;
}

std::string rungMap(const Canvas& c) {
    std::string out;
    for (int y = 0; y < c.height(); ++y) {
        for (int x = 0; x < c.width(); ++x) {
            const auto g = rungOf(steel(), c.at(x, y));
            out += !g ? '.' : *g < 10 ? char('0' + *g) : char('a' + *g - 10);
        }
        out += '\n';
    }
    return out;
}

// A shaded block on black: cells 2-13 by 1-6, rung 2 + (x + y) / 2 % 9
// (rows 1-6: 34455667788 9, ..., 66778899aa 22).
Canvas graded() {
    Canvas c(16, 8);
    for (int y = 1; y <= 6; ++y)
        for (int x = 2; x <= 13; ++x)
            c.at(x, y) = rungCell(steel(), 2 + (x + y) / 2 % 9);
    return c;
}

// One dab over the whole canvas.
Canvas lit(const Canvas& c, const Brush& b) {
    BrushSettings s;
    s.size = kMaxBrushSize;
    Stroke st(c, b, s);
    st.addDab({c.width() / 2, c.height() / 2});
    return st.preview();
}

std::set<Point> changedCells(const Canvas& before, const Canvas& after) {
    std::set<Point> out;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x)
            if (!(before.at(x, y) == after.at(x, y)))
                out.insert({x, y});
    return out;
}

// The cells the accent lit that the lint-aware stroke left unlit though,
// lit on what it left, they would add nothing to the start: none when it
// lit every cell it safely could, as one stamp does with its own cells.
// Over several stamps a change far from a cell left unlit can make it safe
// without its being tried again, so this can find cells there. Under a mirror a cell is lit with its
// images, as the accent lights them (`anywhere`), and one with an image
// the accent doesn't light can't light.
std::vector<Point> missed(const Canvas& start, const Canvas& accent, const Canvas& kept, const Symmetry& sym = {},
                          const Canvas* anywhere = nullptr) {
    std::vector<Point> out;
    const int W = start.width(), H = start.height();
    for (const auto& [x, y] : changedCells(start, accent))
        if (kept.at(x, y) == start.at(x, y)) {
            Canvas t = kept;
            t.at(x, y) = accent.at(x, y);
            bool whole = true;
            if (anywhere)
                for (const Dab& o : symmetryDabs(sym, Dab{x, y}, W, H)) {
                    whole = whole && !(anywhere->at(o.x, o.y) == start.at(o.x, o.y));
                    if (kept.at(o.x, o.y) == start.at(o.x, o.y))
                        t.at(o.x, o.y) = anywhere->at(o.x, o.y);
                }
            if (whole && lintAdded(start, t).empty())
                out.push_back({x, y});
        }
    return out;
}

// Every cell the lint-aware stroke changed, the accent changed the same way.
bool within(const Canvas& start, const Canvas& accent, const Canvas& kept) {
    for (const auto& [x, y] : changedCells(start, kept))
        if (!(kept.at(x, y) == accent.at(x, y)))
            return false;
    return true;
}

} // namespace

TEST_CASE("the lint-aware rim light leaves out a cell that would be a speck", "[detail][lint]") {
    // From N on the shaded block, the accent lights the top row and the
    // rounding down the right; (13,1), rung 9 lit to 10 beside 9s and black,
    // is then lighter than every neighbour. The lint-aware rim lights the
    // rest and leaves (13,1)
    const Canvas c = graded();
    REQUIRE(lint::checkLadder(c, steel()).clean());
    const Canvas accent = lit(c, Accent(steel(), Compass::N));
    const Canvas kept = lit(c, RimLightBrush(steel(), Compass::N, true));
    CHECK(lintAdded(c, accent) == std::vector<std::string>{"cell 13,1: a speck, lighter than every neighbour"});
    CHECK(lintAdded(c, kept).empty());
    std::set<Point> rest = changedCells(c, accent);
    REQUIRE(rest.erase({13, 1}) == 1);
    CHECK(changedCells(c, kept) == rest);
    CHECK(within(c, accent, kept));
    CHECK(missed(c, accent, kept).empty());
    // and the default is the accent
    CHECK(lit(c, RimLightBrush(steel(), Compass::N)) == accent);
}

TEST_CASE("the lint-aware rim light leaves out a cell that would skip a rung", "[detail][lint]") {
    // The blob from E: both cells the accent lights skip a rung beside
    // rung 10 (the accent test above), so neither lights
    const Canvas b = blob();
    BrushSettings s;
    s.size = 12;
    CHECK(paintStroke(b, RimLightBrush(steel(), Compass::E), s, {{4, 2}}).changes.size() == 2);
    CHECK(paintStroke(b, RimLightBrush(steel(), Compass::E, true), s, {{4, 2}}).empty());
    // From S on the shaded block the accent lights the bottom row two rungs
    // above the row over it, every other column; the lint-aware rim lights
    // only the cells that skip nothing
    const Canvas c = graded();
    const Canvas accent = lit(c, Accent(steel(), Compass::S));
    const Canvas kept = lit(c, RimLightBrush(steel(), Compass::S, true));
    const auto skips = lintAdded(c, accent);
    CHECK(skips.size() == 5);
    for (const std::string& m : skips)
        CHECK(m.find("more than one rung apart") != std::string::npos);
    CHECK(lintAdded(c, kept).empty());
    CHECK(within(c, accent, kept));
    CHECK(missed(c, accent, kept).empty());
    for (const Point p : {Point{2, 6}, Point{4, 6}, Point{6, 6}, Point{8, 6}, Point{10, 6}})
        CHECK(kept.at(p.first, p.second) == c.at(p.first, p.second));
    CHECK(changedCells(c, kept).size() == 6);
}

TEST_CASE("the lint-aware rim light lights a rim cell by cell", "[detail][lint]") {
    // A flat block lit from N a cell at a time (a region pass, or a stroke
    // along the top row): each top cell alone would be a speck, lighter than
    // its three neighbours on the ladder, so it waits for the next one along
    // and lights with it, and the whole row lights as the accent lights it
    Canvas c(12, 6);
    for (int y = 1; y <= 4; ++y)
        for (int x = 2; x <= 9; ++x)
            c.at(x, y) = rungCell(steel(), 6);
    const RimLightBrush rim(steel(), Compass::N, true);
    const CellEdit accent = paintRegion(c, Accent(steel(), Compass::N), {});
    REQUIRE(accent.changes.size() == 8);
    CHECK(paintRegion(c, rim, {}).changes == accent.changes);
    std::vector<Dab> along;
    for (int x = 2; x <= 9; ++x)
        along.push_back({x, 1});
    CHECK(paintStroke(c, rim, {}, along).changes == accent.changes);
    // a single cell of the row stays as it is
    CHECK(paintStroke(c, rim, {}, {{5, 1}}).empty());
    CHECK(paintStroke(c, Accent(steel(), Compass::N), {}, {{5, 1}}).changes.size() == 1);
}

TEST_CASE("the lint-aware rim light tries a refused cell again only beside a later stamp", "[detail][lint]") {
    // Codex round 2 on #128: two flat blocks on one canvas, rung 3 and rung
    // 8, with (5,1) at rung 2, lit from N with a square of 2. The stamp at
    // (6,1) refuses (5,1) (lit to 3, the blocks would show ▓ and █ only, one
    // shade glyph) and (6,1) (a speck). The stamp at (18,1) lights (17,1)
    // and (18,1) from 8 to 9, which brings in ░: lighting (5,1) alone now
    // adds nothing, but it is three blocks away, so it isn't tried again and
    // stays unlit (the known limit). The next stamp that would light a cell
    // beside it tries it again, and it lights
    Canvas c(24, 8);
    for (int y = 1; y <= 4; ++y) {
        for (int x = 2; x <= 9; ++x)
            c.at(x, y) = rungCell(steel(), 3);
        for (int x = 14; x <= 21; ++x)
            c.at(x, y) = rungCell(steel(), 8);
    }
    c.at(5, 1) = rungCell(steel(), 2);
    REQUIRE(lint::checkLadder(c, steel()).clean());
    BrushSettings s;
    s.size = 2;
    s.tip = Tip::Square;
    const Accent accent(steel(), Compass::N);
    const RimLightBrush rim(steel(), Compass::N, true);
    Stroke sa(c, accent, s), sk(c, rim, s);
    const auto dab = [&](Dab d) {
        sa.addDab(d, false);
        sk.addDab(d, false);
    };
    dab({6, 1});
    CHECK(changedCells(c, sa.preview()) == std::set<Point>{{5, 1}, {6, 1}});
    CHECK(changedCells(c, sk.preview()).empty());
    CHECK(missed(c, sa.preview(), sk.preview()).empty());
    dab({18, 1});
    CHECK(changedCells(c, sk.preview()) == std::set<Point>{{17, 1}, {18, 1}});
    CHECK(missed(c, sa.preview(), sk.preview()) == std::vector<Point>{{5, 1}});
    CHECK(sk.preview().at(5, 1) == c.at(5, 1));
    dab({4, 1});
    CHECK(sk.preview().at(5, 1) == sa.preview().at(5, 1));
    CHECK(lintAdded(c, sk.preview()).empty());
    CHECK(missed(c, sa.preview(), sk.preview()).empty());
}

namespace {

// A flat block of rung 6 on black: cells 2-9 by 1-4 of a 12 by 6 canvas.
Canvas flatBlock() {
    Canvas c(12, 6);
    for (int y = 1; y <= 4; ++y)
        for (int x = 2; x <= 9; ++x)
            c.at(x, y) = rungCell(steel(), 6);
    return c;
}

// A changed cell of `out` whose mirror orbit under `sym` isn't changed
// whole, each image to its rung; empty when every orbit is.
std::optional<Point> brokenOrbit(const Canvas& start, const Canvas& out, const Symmetry& sym) {
    for (const auto& [x, y] : changedCells(start, out))
        for (const Dab& o : symmetryDabs(sym, Dab{x, y}, out.width(), out.height()))
            if (out.at(o.x, o.y) == start.at(o.x, o.y) || rungOf(steel(), out.at(o.x, o.y)) != rungOf(steel(), out.at(x, y)))
                return Point{x, y};
    return std::nullopt;
}

// One dab over the whole canvas under these options.
Canvas litWith(const Canvas& c, const Brush& b, const StrokeOptions& o) {
    BrushSettings s;
    s.size = kMaxBrushSize;
    Stroke st(c, b, s, o);
    st.addDab({c.width() / 2, c.height() / 2});
    return st.preview();
}

} // namespace

TEST_CASE("the lint-aware rim light lights a mirror orbit whole or not at all", "[detail][lint][symmetry]") {
    // From N the flat block's top row lights, and under mirror X it is its
    // own image: it lights as without the mirror. Under mirror Y (or both)
    // a top cell's image is on the bottom row, which faces S and isn't lit
    // from N, so neither lights though the accent lights the top row
    const Canvas c = flatBlock();
    const RimLightBrush rim(steel(), Compass::N, true);
    const Accent accent(steel(), Compass::N);
    std::set<Point> top;
    for (int x = 2; x <= 9; ++x)
        top.insert({x, 1});
    for (const SymmetryMode mode : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY}) {
        INFO("mode " << int(mode));
        StrokeOptions o;
        o.symmetry = {mode};
        const Canvas a = litWith(c, accent, o), kept = litWith(c, rim, o);
        CHECK(changedCells(c, a) == top);
        CHECK_FALSE(brokenOrbit(c, kept, o.symmetry));
        CHECK(changedCells(c, kept) == (mode == SymmetryMode::MirrorX ? top : std::set<Point>{}));
        CHECK(lintAdded(c, kept).empty());
    }
    // a selection holding a cell but not its image keeps both as they are:
    // (2,1) and (3,1) are selected, their images (9,1) and (8,1) aren't
    StrokeOptions o;
    o.symmetry = {SymmetryMode::MirrorX};
    o.selection = std::make_shared<const Mask>(Mask::rect(12, 6, 2, 1, 3, 1));
    CHECK(changedCells(c, litWith(c, accent, o)) == std::set<Point>{{2, 1}, {3, 1}});
    CHECK(changedCells(c, litWith(c, rim, o)).empty());
    // with the images selected too, the row lights
    o.selection = std::make_shared<const Mask>(Mask::rect(12, 6, 2, 1, 9, 1));
    CHECK(changedCells(c, litWith(c, rim, o)) == top);
}

TEST_CASE("the lint-aware rim light looks up only the refusals beside a stamp's cells", "[detail][lint]") {
    // Flat blocks, each 6 by 4 in a tile 10 by 6 of one canvas, lit from N
    // one top cell at a time, each on a new segment: every cell alone is a
    // speck, so every stamp is refused. The last stamp, on a block of its
    // own, looks up its cell's eight neighbours after 10 refusals as after
    // 1000
    constexpr int TX = 40, TY = 26, TW = 10, TH = 6;
    Canvas c(TX * TW, TY * TH);
    for (int t = 0; t < TX * TY; ++t)
        for (int y = 1; y <= 4; ++y)
            for (int x = 2; x <= 7; ++x)
                c.at(t % TX * TW + x, t / TX * TH + y) = rungCell(steel(), 6);
    const RimLightBrush rim(steel(), Compass::N, true);
    const auto onBlock = [&](int t) { return Dab{t % TX * TW + 4, t / TX * TH + 1}; };
    const auto lastStamp = [&](int refusals) {
        Stroke st(c, rim, {});
        for (int t = 0; t < refusals; ++t)
            st.addDab(onBlock(t), false);
        REQUIRE(st.edit().empty());
        const long long before = RimLightBrush::refusalLookups();
        st.addDab(onBlock(TX * TY - 1), false);
        REQUIRE(st.edit().empty());
        return RimLightBrush::refusalLookups() - before;
    };
    const long long few = lastStamp(10), many = lastStamp(1000);
    CHECK(few == many);
    CHECK(many == 8);
}

namespace {

// A rung written with a bright background: a shade the other way round
// (K▓w as w░K), a bright solid colour as a space on it. Other cells as
// they are.
Cell brightBackground(Cell c) {
    if (c.ch == U'░' || c.ch == U'▒' || c.ch == U'▓') {
        c.ch = c.ch == U'░' ? U'▓' : c.ch == U'▓' ? U'░' : U'▒';
        std::swap(c.fg, c.bg);
    } else if (c.ch == U'█' && c.fg.index >= 8) {
        c.ch = U' ';
        c.bg = c.fg;
    }
    return c;
}

// A random canvas for the rim light: one to three filled ellipses and
// rectangles drawn as the shape tools draw them, in steel w, the full
// cells then shaded flat, as a gradient or at random rungs. On half the
// canvases about one full cell in eight is then written with a bright
// background and one in eight blinks. With `mx` and
// `my` the pixels and the rungs are mirrored left to right and top to
// bottom, and the shapes kept four columns and a row in from the canvas's
// edges, where the rim light lights a shape differently (its known limit).
Canvas rimShapes(std::mt19937& rng, int W, int H, bool mx = false, bool my = false) {
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const bool in = mx || my;
    const int lx = in ? 4 : -2, hx = in ? W - 5 : W + 1, ly = in ? 2 : -2, hy = in ? 2 * H - 3 : 2 * H + 1;
    Pixels px;
    for (int i = 0, n = pick(1, 3); i < n; ++i) {
        int x0 = pick(lx, hx), x1 = pick(lx, hx), y0 = pick(ly, hy), y1 = pick(ly, hy);
        if (x0 > x1)
            std::swap(x0, x1);
        if (y0 > y1)
            std::swap(y0, y1);
        const Pixels p = pick(0, 1) ? ellipseShape(x0, y0, x1, y1, true, W, 2 * H) : rectShape(x0, y0, x1, y1, true, W, 2 * H);
        px.insert(px.end(), p.begin(), p.end());
    }
    for (size_t i = 0, n = px.size(); i < n; ++i) {
        const auto [x, py] = px[i];
        if (mx)
            px.push_back({W - 1 - x, py});
        if (my)
            px.push_back({x, 2 * H - 1 - py});
        if (mx && my)
            px.push_back({W - 1 - x, 2 * H - 1 - py});
    }
    Canvas c(W, H);
    plotPixels(c, px, 7).apply(c);
    const int kind = pick(0, 2), top = steel().topRung();
    const int flat = pick(1, top - 1), from = pick(1, top / 2), every = pick(1, 3), way = pick(0, 3);
    std::vector<int> noise(size_t(W) * H), odd(size_t(W) * H);
    for (int& g : noise)
        g = pick(1, top);
    const bool odds = pick(0, 1);
    for (int& v : odd)
        v = odds ? pick(0, 7) : 2;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            // a full cell of w, as the shape tools write it
            if (!(c.at(x, y).ch == U' ' && c.at(x, y).bg.index == 7) && c.at(x, y).ch != U'█')
                continue;
            const int sx = mx ? std::min(x, W - 1 - x) : x, sy = my ? std::min(y, H - 1 - y) : y;
            const int along = way == 0 ? sx : way == 1 ? sy : way == 2 ? sx + sy : sx + H - sy;
            const int g = kind == 0 ? flat : kind == 1 ? std::min(top, from + along / every) : noise[size_t(sy) * W + sx];
            Cell cell = rungCell(steel(), g);
            if (const int v = odd[size_t(sy) * W + sx]; v == 0)
                cell = brightBackground(cell);
            else if (v == 1)
                cell.attrs |= attr::Blink;
            c.at(x, y) = cell;
        }
    return c;
}

// The lint-aware rim light, checked after every stamp for what it promises:
// each cell the stamp tried, with its images under a mirror (the cells it
// covers that the accent lights, and the cells an earlier stamp tried and
// left unlit beside them), is lit or would add a finding to the start lit
// alone on what the stroke has left. `anywhere` is the start as the accent
// lights all of it. A stamp under a mirror lights its images together, so
// an image the stroke stamps again after (the same dab, size and
// strength) tries nothing, as in the brush.
class StampChecked : public Brush {
public:
    StampChecked(const Canvas& start, const Canvas& anywhere, Compass from)
        : rim_(steel(), from, true), start_(start), anywhere_(anywhere) {}
    std::string_view name() const override { return "stamp-checked"; }
    std::optional<Cell> paintCell(const Stroke& s, int x, int y, double strength) const override {
        return rim_.paintCell(s, x, y, strength);
    }
    void dab(Stroke& s, const Dab& d, int size, double strength) const override {
        const int W = start_.width(), H = start_.height();
        const Symmetry& sym = s.options().symmetry;
        const bool mirror = sym.mode != SymmetryMode::None;
        std::vector<Dab> images{d};
        bool again = false;
        if (mirror) {
            if (d.image == 0 && lastImage_ != 0)
                done_.clear();
            lastImage_ = d.image;
            again = !done_.insert({d.x, d.y, d.flipX, d.flipY, size, strength}).second;
            images = symmetryDabs(sym, d, W, H);
            for (const Dab& o : images)
                done_.insert({o.x, o.y, o.flipX, o.flipY, size, strength});
        }
        const auto orbit = [&](Point p) {
            std::vector<Point> out{p};
            if (mirror)
                for (const Dab& o : symmetryDabs(sym, Dab{p.first, p.second}, W, H))
                    out.push_back({o.x, o.y});
            return out;
        };
        // every image unlit so far, and one the accent lights
        const auto open = [&](Point p) {
            for (const auto& [x, y] : orbit(p))
                if (s.painted(x, y) || !s.paintable(x, y) || anywhere_.at(x, y) == start_.at(x, y))
                    return false;
            return true;
        };
        std::set<Point> own;
        if (!again)
            for (const Dab& o : images)
                for (const auto& [x, y] : footprint(o.x, o.y, size, s.settings().tip, W, H, o.flipX, o.flipY))
                    if (!s.painted(x, y) && s.paintable(x, y) && anywhere_.at(x, y) != start_.at(x, y))
                        own.insert({x, y});
        std::set<Point> tried = own;
        for (const Point p : left_)
            if (!s.painted(p.first, p.second) && std::any_of(own.begin(), own.end(), [&](Point q) {
                    return std::abs(p.first - q.first) <= 1 && std::abs(p.second - q.second) <= 1;
                }))
                tried.insert(p);
        rim_.dab(s, d, size, strength);
        if (tried.empty() || !failure.empty())
            return;
        const Canvas now = s.preview();
        for (const Point p : tried) {
            if (!open(p)) {
                retried += !own.count(p) && s.painted(p.first, p.second);
                continue;
            }
            ++held;
            left_.insert(p);
            Canvas t = now;
            for (const auto& [x, y] : orbit(p))
                t.at(x, y) = anywhere_.at(x, y);
            if (lintAdded(start_, t).empty()) {
                failure = "stamp (" + std::to_string(d.x) + "," + std::to_string(d.y) + ") left (" +
                          std::to_string(p.first) + "," + std::to_string(p.second) +
                          ") unlit, which it tried and which adds nothing\n" + rungMap(now);
                return;
            }
        }
    }
    // The first stamp that left a cell it tried unlit though it was safe.
    mutable std::string failure;
    // How often a stamp left a cell it tried unlit, and how many cells left
    // unlit before a stamp lit beside its own (none without the retry).
    mutable int held = 0, retried = 0;

private:
    RimLightBrush rim_;
    const Canvas& start_;
    const Canvas& anywhere_;
    mutable std::set<std::tuple<int, int, bool, bool, int, double>> done_;
    mutable int lastImage_ = 0;
    // the cells a stamp has tried and left unlit, with their images open
    mutable std::set<Point> left_;
};

} // namespace

TEST_CASE("the lint-aware rim light adds nothing to the start and lights what each stamp tries safely", "[detail][lint][symmetry]") {
    // 600 random strokes over shaded shapes (some with bright backgrounds
    // and blinking cells; iCE on and off, no symmetry or a mirror, joined
    // walks and new segments, sizes 1-12 on both tips), lit from all eight
    // points. After every dab the lint-aware stroke adds no finding to its
    // start by the lint's own rule, every cell it changed is one the accent
    // lights, the same way (under a mirror it lights each stamp's images,
    // which can lie beside another image's joining line, so not only the
    // cells the accent's stroke lit), and after every stamp each cell the
    // stamp tried and left unlit (its own, and earlier refusals beside them)
    // would add a finding lit alone, with its images (StampChecked). A cell
    // the stamp didn't try isn't checked: a change further off can make it
    // safe to light, and it waits for a stamp beside it. Off, the brush
    // paints what the accent painted before the option, cell for cell
    constexpr int W = 24, H = 12;
    std::mt19937 rng(2026102454);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const SymmetryMode modes[4] = {SymmetryMode::None, SymmetryMode::MirrorX, SymmetryMode::MirrorY,
                                   SymmetryMode::MirrorXY};
    int failed = 0, dabs = 0, accentLit[2] = {}, keptLit[2] = {}, refusing = 0, mirrored = 0, iced = 0, odd[2] = {},
        held = 0, retried = 0;
    std::string first;
    for (int k = 0; k < 600; ++k) {
        const Canvas start = rimShapes(rng, W, H);
        const Compass from = kCompass[pick(0, 7)];
        StrokeOptions o;
        o.ice = pick(0, 1);
        o.symmetry = {modes[pick(0, 3)]};
        const bool mirror = o.symmetry.mode != SymmetryMode::None;
        mirrored += mirror;
        iced += o.ice;
        bool oddCells = false;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                oddCells = oddCells || (start.at(x, y).attrs & attr::Blink) || start.at(x, y).bg.index >= 8;
        odd[o.ice] += oddCells;
        BrushSettings s;
        s.size = pick(1, 12);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 1.0}[size_t(pick(0, 2))];
        const Accent accent(steel(), from);
        StrokeOptions plain;
        plain.ice = o.ice;
        const Canvas anywhere = litWith(start, accent, plain);
        const RimLightBrush off(steel(), from);
        const StampChecked on(start, anywhere, from);
        Stroke sa(start, accent, s, o), so(start, off, s, o), sk(start, on, s, o);
        int x = pick(0, W - 1), y = pick(0, H - 1);
        std::ostringstream trail;
        std::string failure;
        for (int i = 0, n = pick(1, 8); i < n && failure.empty(); ++i) {
            const bool join = i == 0 || pick(0, 4) != 0;
            if (!join) {
                x = pick(0, W - 1);
                y = pick(0, H - 1);
            }
            for (Stroke* st : {&sa, &so, &sk})
                st->addDab({x, y}, join);
            ++dabs;
            trail << "(" << x << "," << y << (join ? "" : " new") << ") ";
            const Canvas kept = sk.preview(), was = sa.preview();
            if (const auto found = lintAdded(start, kept); !found.empty())
                failure = "dab " + std::to_string(i) + " added " + found.front() + "\n" + rungMap(kept);
            else if (!within(start, anywhere, kept))
                failure = "dab " + std::to_string(i) + " changed a cell the accent doesn't light\n" + rungMap(kept);
            else if (!(so.preview() == was))
                failure = "dab " + std::to_string(i) + ": off isn't the accent\n" + rungMap(so.preview());
            else if (!on.failure.empty())
                failure = "dab " + std::to_string(i) + ": " + on.failure;
            x = std::clamp(x + pick(-4, 4), 0, W - 1);
            y = std::clamp(y + pick(-3, 3), 0, H - 1);
        }
        const Canvas kept = sk.preview(), was = sa.preview();
        if (failure.empty() && !(so.edit().changes == sa.edit().changes))
            failure = "off's edit isn't the accent's";
        accentLit[mirror] += int(changedCells(start, was).size());
        keptLit[mirror] += int(changedCells(start, kept).size());
        refusing += !lintAdded(start, was).empty();
        held += on.held;
        retried += on.retried;
        if (!failure.empty() && failed++ == 0) {
            std::ostringstream out;
            out << "stroke " << k << ": from " << int(from) << " ice " << o.ice << " symmetry "
                << int(o.symmetry.mode) << " size " << s.size << " tip " << int(s.tip) << "\n"
                << trail.str() << "\nstart\n"
                << rungMap(start) << "accent\n"
                << rungMap(was) << failure;
            first = out.str();
        }
    }
    INFO(failed << " of 600 strokes (" << dabs << " dabs, " << mirrored << " mirrored, " << iced << " iCE, "
                << odd[0] << " and " << odd[1] << " with bright backgrounds or blink, iCE off and on) failed; the accent lit "
                << accentLit[0] << " cells unmirrored and " << accentLit[1] << " mirrored and added findings in "
                << refusing << " strokes, the lint-aware rim lit " << keptLit[0] << " and " << keptLit[1]
                << "; stamps left a cell they tried unlit " << held << " times and lit " << retried
                << " cells refused before beside their own\n"
                << first);
    CHECK(failed == 0);
    CHECK(refusing > 100);
    CHECK(keptLit[0] > accentLit[0] / 5);
    CHECK(keptLit[1] > 0);
    CHECK(held > 1000);
    CHECK(retried > 0);
    CHECK(odd[0] > 100);
    CHECK(odd[1] > 100);
}

TEST_CASE("the lint-aware rim light keeps a symmetric canvas symmetric under a mirror", "[detail][lint][symmetry]") {
    // 400 random mirror strokes over shapes mirrored the same way, lit from
    // all eight points: after every dab each cell lit has every image the
    // mirror makes (both axes under mirror X and Y) lit, at its rung, though
    // the light may not face an image the way it faces the cell
    std::mt19937 rng(2026102455);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const SymmetryMode modes[3] = {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY};
    int failed = 0, lit = 0, held = 0, accentAsymmetric = 0, kept[8] = {};
    std::string first;
    for (int k = 0; k < 400; ++k) {
        const SymmetryMode mode = modes[k % 3];
        // odd and even sizes: the axis on a cell and between two
        const int W = pick(12, 28), H = pick(6, 13);
        const bool mx = mode != SymmetryMode::MirrorY, my = mode != SymmetryMode::MirrorX;
        const Canvas start = rimShapes(rng, W, H, mx, my);
        const int point = pick(0, 7);
        const Compass from = kCompass[point];
        StrokeOptions o;
        o.ice = pick(0, 1);
        o.symmetry = {mode};
        BrushSettings s;
        s.size = pick(1, 8);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        const Accent accent(steel(), from);
        const RimLightBrush rim(steel(), from, true);
        Stroke sa(start, accent, s, o), sk(start, rim, s, o);
        const bool join = pick(0, 1);
        std::ostringstream trail;
        bool broke = false, accentBroke = false;
        for (int i = pick(1, 5); i > 0 && !broke; --i) {
            const Dab d{pick(-1, W), pick(-1, H)};
            trail << "(" << d.x << "," << d.y << ") ";
            sa.addDab(d, join);
            sk.addDab(d, join);
            accentBroke = accentBroke || brokenOrbit(start, sa.preview(), o.symmetry);
            broke = bool(brokenOrbit(start, sk.preview(), o.symmetry));
        }
        const bool any = !changedCells(start, sk.preview()).empty();
        lit += any;
        kept[point] += any;
        accentAsymmetric += accentBroke;
        held += changedCells(start, sk.preview()) != changedCells(start, sa.preview());
        if (broke && failed++ == 0) {
            std::ostringstream m;
            m << "stroke " << k << ": mode " << int(mode) << " from " << int(from) << " " << W << "x" << H
              << " size " << s.size << " tip " << int(s.tip) << " join " << join << " accent broke " << accentBroke
              << "\n"
              << trail.str() << "\nstart\n"
              << rungMap(start) << "accent\n"
              << rungMap(sa.preview()) << "lint-aware\n"
              << rungMap(sk.preview());
            first = m.str();
        }
    }
    INFO(failed << " of 400 strokes broke symmetry; " << lit << " lit, " << held << " left a cell unlit, "
                << accentAsymmetric << " where the accent's stroke isn't symmetric\n"
                << first);
    CHECK(failed == 0);
    CHECK(accentAsymmetric > 100);
    CHECK(lit > 40);
    CHECK(held > 40);
    // every point lights something somewhere
    for (const int n : kept)
        CHECK(n > 0);
}

TEST_CASE("the half-block pencil joins radial images in 64 bits", "[symmetry][halfblock]") {
    // Codex round 2 #3: every image of this segment misses the canvas;
    // saturating an end at the ends of int bends one across it
    Canvas c(10, 10);
    StrokeOptions o;
    o.symmetry = {SymmetryMode::Radial, 8, std::pair{0, 0}};
    HalfBlockStroke s(c, White, o);
    s.addPoint(-2147483647, -2147482647);
    s.addPoint(1073741823, 1073741116);
    CHECK(s.edit().empty());
    // the 8-connected pencil too
    HalfBlockStroke e(c, White, o, false, Connectivity::Eight);
    e.addPoint(-2147483647, -2147482647);
    e.addPoint(1073741823, 1073741116);
    CHECK(e.edit().empty());
}
