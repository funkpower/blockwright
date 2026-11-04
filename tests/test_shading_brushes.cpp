// The form, soft and despeckle brushes (PLAN §10, "Shading"), and the
// done-when sphere of Phase 2b: a ball painted by hand with the form brush
// over a round selection, then a despeckle pass, that passes the ladder
// lint.

#include "blockwright/ansi.h"
#include "cpu_time.h"
#include "portable_random.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/distance.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/brushes/soft.h"
#include "blockwright/lint.h"
#include "blockwright/palette.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace bw;
using Kind = lint::LadderFinding::Kind;

namespace {

// The form brush's options and the light it reads from the brush settings.
struct FormCase : FormOptions {
    Compass light = Compass::NW;
};

Ramp steel() { return *findRamp("steel"); }  // k K w W

Cell cell(char32_t g, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = g;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

Canvas filled(int w, int h, const Cell& c) {
    Canvas canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.at(x, y) = c;
    return canvas;
}

Canvas black(int w, int h) { return filled(w, h, cell(U' ', Black, Black)); }

// A disc that looks round on screen: `across` columns wide, half as many
// rows tall (a cell is 8×16).
Mask disc(int w, int h, double cx, double cy, double across) {
    Mask m(w, h);
    const double r = across / 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double dx = x - cx, dy = (y - cy) * 2;
            if (dx * dx + dy * dy <= r * r)
                m.set(x, y);
        }
    return m;
}

int count(const lint::LadderReport& r, Kind k) {
    int n = 0;
    for (const auto& f : r.findings)
        n += f.kind == k;
    return n;
}

// The canvas's rungs as text, a character a cell (0-9 then a-z, '.' off the
// ladder), for a failure message.
std::string rungMap(const Canvas& c, const Ramp& r) {
    std::ostringstream out;
    for (int y = 0; y < c.height(); ++y) {
        for (int x = 0; x < c.width(); ++x) {
            const auto g = rungOf(r, c.at(x, y));
            out << (!g ? '.' : *g < 10 ? char('0' + *g) : char('a' + *g - 10));
        }
        out << '\n';
    }
    return out.str();
}

std::string findings(const lint::LadderReport& r) {
    std::string s;
    for (const auto& f : r.findings)
        s += f.message + "\n";
    return s;
}

std::shared_ptr<const Mask> shared(Mask m) { return std::make_shared<const Mask>(std::move(m)); }

// Rows of dabs back and forth over the box, the way a hand fills a shape:
// a round tip, each row's dabs two columns apart and joined by the stroke,
// the pressure rising and falling along it, a new stroke (undo step) for
// each row.
void scribble(Canvas& c, const Brush& brush, BrushSettings s, const StrokeOptions& o, int x0, int y0, int x1,
              int y1) {
    for (int y = y0, row = 0; y <= y1; y += 2, ++row) {
        std::vector<Dab> dabs;
        for (int i = 0, x = row % 2 ? x1 : x0; row % 2 ? x >= x0 : x <= x1; ++i, x += row % 2 ? -2 : 2)
            dabs.push_back({x, y, 0.6 + 0.4 * std::abs(std::sin(i * 0.7 + row))});
        paintStroke(c, brush, s, dabs, o).apply(c);
    }
}

// A shape's form tones laid on a black canvas.
Canvas toneCanvas(const Ramp& r, const Mask& m, const FormCase& o) {
    const auto tones = formRungs(r, m, o, o.light);
    Canvas c = black(m.width(), m.height());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (const int t = tones[static_cast<size_t>(y) * m.width() + x]; t >= 0)
                c.at(x, y) = rungCell(r, t);
    return c;
}

// Seams: three or more cells down a column that are each one cell lighter
// than both their neighbours along the row (on the shape): a one-cell
// ridge, which reads as a light line drawn down the form.
int seams(const Canvas& c, const Ramp& r, const Mask& m) {
    auto peak = [&](int x, int y) {
        if (!m.contains(x - 1, y) || !m.contains(x, y) || !m.contains(x + 1, y))
            return false;
        const int g = *rungOf(r, c.at(x, y));
        return g > *rungOf(r, c.at(x - 1, y)) && g > *rungOf(r, c.at(x + 1, y));
    };
    int found = 0;
    for (int x = 0; x < m.width(); ++x)
        for (int y = 0, run = 0; y <= m.height(); ++y) {
            if (y < m.height() && peak(x, y)) {
                ++run;
                continue;
            }
            found += run >= 3;
            run = 0;
        }
    return found;
}

void checkClean(const lint::LadderReport& report) {
    CHECK(count(report, Kind::RungSkip) == 0);
    CHECK(count(report, Kind::WideRung) == 0);
    CHECK(count(report, Kind::Speckle) == 0);
    CHECK(count(report, Kind::NonNeighbourShade) == 0);
}

} // namespace

TEST_CASE("form tones of a dome are a ladder lit from the light", "[shading][form]") {
    const Ramp r = steel();
    for (const double across : {12.0, 20.0, 30.0, 44.0, 60.0}) {
        const int w = int(across) + 8, h = int(across / 2) + 6;
        const Mask m = disc(w, h, w / 2.0, h / 2.0, across);
        for (const bool rim : {false, true}) {
            for (const bool spec : {false, true}) {
                FormCase o;
                o.rim = rim;
                o.specular = spec;
                const auto tones = formRungs(r, m, o, o.light);
                Canvas c = black(w, h);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const int t = tones[static_cast<size_t>(y) * w + x];
                        CHECK((t >= 0) == m.contains(x, y));
                        if (t >= 0)
                            c.at(x, y) = rungCell(r, t);
                    }
                const auto report = lint::checkLadder(c, r);
                INFO("across " << across << " rim " << rim << " spec " << spec << "\n"
                               << rungMap(c, r) << findings(report));
                CHECK(count(report, Kind::RungSkip) == 0);
                CHECK(count(report, Kind::WideRung) == 0);
                CHECK(count(report, Kind::Speckle) == 0);
                CHECK(count(report, Kind::NonNeighbourShade) == 0);
            }
        }
    }
}

TEST_CASE("form tones follow the light", "[shading][form]") {
    const Ramp r = steel();
    const int w = 38, h = 21;
    const Mask m = disc(w, h, w / 2.0, h / 2.0, 30);
    for (int l = 0; l < 8; ++l) {
        FormCase o;
        o.light = Compass(l);
        const auto tones = formRungs(r, m, o, o.light);
        Canvas c = black(w, h);
        int bx = 0, by = 0, best = -1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (const int t = tones[static_cast<size_t>(y) * w + x]; t >= 0) {
                    c.at(x, y) = rungCell(r, t);
                    if (t > best)
                        best = t, bx = x, by = y;
                }
        const auto report = lint::checkLadder(c, r);
        INFO("light " << l << "\n" << rungMap(c, r) << findings(report));
        CHECK(count(report, Kind::RungSkip) == 0);
        CHECK(count(report, Kind::WideRung) == 0);
        CHECK(count(report, Kind::Speckle) == 0);
        // the brightest cell is on the light's side of the middle
        const auto [lx, ly] = compassVector(Compass(l));
        CHECK((bx - w / 2.0) * lx + (by - h / 2.0) * 2 * ly > 0);
        CHECK(best == r.topRung());
    }
    // the same shape and options always give the same tones
    CHECK(formRungs(r, m) == formRungs(r, m));
}

TEST_CASE("form tones of a cylinder and a plane", "[shading][form]") {
    const Ramp r = steel();
    const int w = 40, h = 20;
    Mask m(w, h);
    for (int y = 3; y < 17; ++y)
        for (int x = 14; x < 26; ++x)
            m.set(x, y);
    for (const auto shape : {FormShape::Cylinder, FormShape::Plane}) {
        FormCase o;
        o.shape = shape;
        o.light = Compass::W;
        const auto tones = formRungs(r, m, o, o.light);
        Canvas c = black(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (const int t = tones[static_cast<size_t>(y) * w + x]; t >= 0)
                    c.at(x, y) = rungCell(r, t);
        const auto report = lint::checkLadder(c, r);
        INFO("shape " << int(shape) << "\n" << rungMap(c, r) << findings(report));
        CHECK(count(report, Kind::RungSkip) == 0);
        CHECK(count(report, Kind::Speckle) == 0);
        // lit from the west: the left side is lighter than the right
        CHECK(tones[static_cast<size_t>(10) * w + 16] > tones[static_cast<size_t>(10) * w + 24]);
    }
}

TEST_CASE("form brush shades the selection's shape", "[shading][form]") {
    const Ramp r = steel();
    const int w = 40, h = 20;
    const Mask m = disc(w, h, 20, 10, 24);
    const auto tones = formRungs(r, m);
    FormShadeBrush brush(r);
    BrushSettings s;
    s.size = 5;
    s.tip = Tip::Round;
    StrokeOptions o;
    o.selection = shared(m);

    SECTION("full strength lays the tones under the stroke, nothing else") {
        Canvas c = black(w, h);
        const Canvas before = c;
        paintStroke(c, brush, s, {{14, 10}, {26, 10}}, o).apply(c);
        int painted = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!m.contains(x, y)) {
                    CHECK(c.at(x, y) == before.at(x, y));
                    continue;
                }
                if (c.at(x, y) == before.at(x, y))
                    continue;
                ++painted;
                CHECK(rungOf(r, c.at(x, y)) == tones[static_cast<size_t>(y) * w + x]);
            }
        // the stroke runs across the turn: its cells in the shadow are
        // black, as the canvas is
        CHECK(painted > 30);
    }
    SECTION("a light press moves a cell part of the way") {
        // two shading glyphs off the shape already: a light press on black
        // alone shades with one glyph, the lint's one-glyph finding
        Canvas c = black(w, h);
        c.at(0, 0) = rungCell(r, 1);
        c.at(1, 0) = rungCell(r, 2);
        s.strength = 0.25;
        paintStroke(c, brush, s, {{20, 10}}, o).apply(c);
        const int t = tones[static_cast<size_t>(10) * w + 20];
        REQUIRE(t > 3);
        CHECK(rungOf(r, c.at(20, 10)) == 3);
    }
    SECTION("the alpha lock keeps transparent cells") {
        Canvas c = black(w, h);
        c.at(20, 10).transparent = true;
        const Cell kept = c.at(20, 10);
        o.lockTransparent = true;
        // a stroke across the form: a lone dab round the hole is a patch
        // of specks on the black, which the brush won't lay
        paintStroke(c, brush, s, {{14, 10}, {26, 10}}, o).apply(c);
        CHECK(c.at(20, 10) == kept);
        CHECK(c.at(21, 10) != black(1, 1).at(0, 0));
    }
    SECTION("with iCE off no cell needs a bright background") {
        Canvas c = black(w, h);
        o.ice = false;
        for (int y = 0; y < h; ++y)
            paintStroke(c, brush, s, {{6, y}, {34, y}}, o).apply(c);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                CHECK(c.at(x, y).bg.paletteIndex() < 8);
    }
}

TEST_CASE("form brush with nothing selected shades what the stroke covers", "[shading][form]") {
    const Ramp r = steel();
    const int w = 40, h = 20;
    FormShadeBrush brush(r);
    BrushSettings s;
    s.size = 9;
    s.tip = Tip::Round;
    const std::vector<Dab> dabs{{12, 10}, {28, 10}};
    Canvas a = black(w, h), b = black(w, h);
    paintStroke(a, brush, s, dabs).apply(a);
    paintStroke(b, FormShadeBrush(r), s, dabs).apply(b);
    CHECK(a == b);  // determinism
    // the covered cells are the form: a lit top left, all on the ladder
    // (its shadow black, like the ground)
    Mask covered(w, h);
    for (int dx = 12; dx <= 28; ++dx)  // a dab every column along the stroke
        for (const auto& [x, y] : footprint(dx, 10, s.size, s.tip, w, h, false))
            covered.set(x, y);
    const auto tones = formRungs(r, covered);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (covered.contains(x, y))
                CHECK(rungOf(r, a.at(x, y)) == tones[static_cast<size_t>(y) * w + x]);
    const auto report = lint::checkLadder(a, r);
    INFO(rungMap(a, r) << findings(report));
    CHECK(count(report, Kind::RungSkip) == 0);
    CHECK(count(report, Kind::Speckle) == 0);
}

TEST_CASE("soft falloff bands rungs from the edge in", "[shading][soft]") {
    const auto at = [](const auto& f, int x, int y) {
        for (const auto& [xy, n] : f)
            if (xy == std::pair{x, y})
                return n;
        return -1;
    };
    const auto f = softFalloff({20, 10}, 15, Tip::Round, 40, 20, 1.0, 0.0);
    CHECK(at(f, 20, 10) == 4);  // the middle moves a whole colour
    // along a row: 1, 2, 3, 4 going in, a band a column wide (two where the
    // band above it turns a corner)
    int prev = 0, run = 0;
    for (int x = 13; x <= 20; ++x) {
        const int n = at(f, x, 10);
        REQUIRE(n >= 1);
        CHECK(n - prev <= 1);
        if (n == prev) {
            ++run;
        } else {
            if (prev > 0 && prev < 4)
                CHECK(run <= 2);
            run = 1;
        }
        prev = n;
    }
    // a hard tip moves its cells evenly, the whole way
    for (const auto& [xy, n] : softFalloff({20, 10}, 15, Tip::Round, 40, 20, 1.0, 1.0))
        CHECK(n == 4);
    // a light press moves the middle less
    CHECK(at(softFalloff({20, 10}, 15, Tip::Round, 40, 20, 0.5, 0.0), 20, 10) == 2);
    // a flipped tip is the mirror image
    const auto m = softFalloff({20, 10, 1.0, 0, true}, 6, Tip::Round, 40, 20, 1.0, 0.0);
    const auto n = softFalloff({20, 10}, 6, Tip::Round, 40, 20, 1.0, 0.0);
    for (const auto& [xy, k] : n)
        CHECK(at(m, 40 - xy.first, xy.second) == k);
}

TEST_CASE("soft brush lightens a flat field into a clean ladder", "[shading][soft]") {
    const Ramp r = steel();
    const int w = 40, h = 20;
    BrushSettings s;
    s.size = 15;
    s.tip = Tip::Round;
    Canvas c = filled(w, h, rungCell(r, 4));
    const Canvas before = c;
    SoftShadeBrush brush(r, ShadeDirection::Lighten);
    // a stroke along a row: overlapping dabs move a cell once
    paintStroke(c, brush, s, {{14, 10}, {26, 10}}).apply(c);
    CHECK(rungOf(r, c.at(20, 10)) == 8);
    CHECK(rungOf(r, c.at(0, 0)) == 4);
    const auto report = lint::checkLadder(c, r);
    INFO(rungMap(c, r) << findings(report));
    CHECK(count(report, Kind::RungSkip) == 0);
    CHECK(count(report, Kind::WideRung) == 0);
    CHECK(count(report, Kind::Speckle) == 0);
    CHECK(count(report, Kind::NonNeighbourShade) == 0);

    Canvas again = before;
    paintStroke(again, SoftShadeBrush(r, ShadeDirection::Lighten), s, {{14, 10}, {26, 10}}).apply(again);
    CHECK(again == c);  // determinism

    SECTION("darkening, the selection and the alpha lock") {
        Canvas d = filled(w, h, rungCell(r, 8));
        d.at(20, 10).transparent = true;
        const Cell kept = d.at(20, 10);
        StrokeOptions o;
        Mask left(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < 21; ++x)
                left.set(x, y);
        o.selection = shared(left);
        o.lockTransparent = true;
        paintStroke(d, SoftShadeBrush(r, ShadeDirection::Darken), s, {{20, 10}}, o).apply(d);
        CHECK(d.at(20, 10) == kept);
        CHECK(rungOf(r, d.at(19, 10)) < 8);
        CHECK(rungOf(r, d.at(22, 10)) == 8);
    }
    SECTION("with iCE off no cell needs a bright background") {
        StrokeOptions o;
        o.ice = false;
        Canvas d = filled(w, h, rungCell(r, 4));
        paintStroke(d, SoftShadeBrush(r, ShadeDirection::Lighten), s, {{20, 10}}, o).apply(d);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                CHECK(d.at(x, y).bg.paletteIndex() < 8);
    }
}

TEST_CASE("despeckle clears the specks under it and nothing else", "[shading][despeckle]") {
    const Ramp r = steel();
    const int w = 30, h = 14;
    // a field of grey with a gradient band, and specks in and out of it
    Canvas c = filled(w, h, rungCell(r, 8));
    for (int y = 0; y < h; ++y)
        for (int x = 20; x < w; ++x)
            c.at(x, y) = rungCell(r, x < 22 ? 7 : x < 24 ? 6 : 5);
    const std::vector<std::pair<std::pair<int, int>, int>> specks{
        {{5, 5}, 4}, {{9, 6}, 11}, {{12, 3}, 6}, {{13, 3}, 6}, {{21, 9}, 9}, {{26, 2}, 2}};
    for (const auto& [xy, g] : specks)
        c.at(xy.first, xy.second) = rungCell(r, g);
    const lint::Box under{0, 0, 24, h - 1};
    REQUIRE(count(lint::checkLadder(c, r, under), Kind::Speckle) == 3);  // a pair isn't a speck
    const Canvas before = c;

    BrushSettings s;
    s.size = 3;
    // a pass of dabs over columns 0-24: the speck at 26, 2 is outside it
    std::vector<Dab> pass;
    for (int y = 1; y < h; y += 3)
        for (int x = y % 2 ? 1 : 23; y % 2 ? x <= 23 : x >= 1; x += y % 2 ? 2 : -2)
            pass.push_back({x, y});
    const auto edit = paintStroke(c, DespeckleBrush(r), s, pass);
    edit.apply(c);
    INFO(rungMap(c, r));
    const auto report = lint::checkLadder(c, r, under);
    INFO(findings(report));
    CHECK(count(report, Kind::Speckle) == 0);
    for (int y = 0; y < h; ++y)
        for (int x = 25; x < w; ++x)
            CHECK(c.at(x, y) == before.at(x, y));
    CHECK(rungOf(r, c.at(26, 2)) == 2);
    CHECK(rungOf(r, c.at(5, 5)) == 8);
    CHECK(rungOf(r, c.at(21, 9)) == 7);
    // cells that weren't specks are left alone
    CHECK(c.at(22, 4) == before.at(22, 4));

    SECTION("it is deterministic") {
        Canvas again = before;
        paintStroke(again, DespeckleBrush(r), s, pass).apply(again);
        CHECK(again == c);
    }
    SECTION("the selection and the alpha lock keep their cells") {
        Canvas d = before;
        d.at(9, 6).transparent = true;
        const Cell kept = d.at(9, 6);
        Mask m(w, h, true);
        m.set(5, 5, false);
        StrokeOptions o;
        o.selection = shared(m);
        o.lockTransparent = true;
        paintStroke(d, DespeckleBrush(r), s, pass, o).apply(d);
        CHECK(d.at(9, 6) == kept);
        CHECK(rungOf(r, d.at(5, 5)) == 4);
        CHECK(rungOf(r, d.at(21, 9)) == 7);
    }
}

TEST_CASE("Phase 2b done-when: a sphere by hand, then a despeckle pass", "[shading][form][despeckle]") {
    const Ramp r = steel();
    const int w = 52, h = 26;
    const Mask m = disc(w, h, 26, 13, 40);
    Canvas c = black(w, h);
    StrokeOptions o;
    o.selection = shared(m);
    BrushSettings s;
    s.size = 4;
    s.tip = Tip::Round;
    s.pressure = PressureTo::Strength;
    // two coats of hand strokes over the ball, pressure rising and falling,
    // then a firm one: a stroke lays its cells from the canvas as it began,
    // so a light press next to the glint's ladder skips rungs and is taken
    // back, and only a firmer press fills it in
    FormShadeBrush form(r);
    scribble(c, form, s, o, 4, 1, 48, 25);
    scribble(c, form, s, o, 5, 0, 47, 24);
    s.pressure = PressureTo::Nothing;
    scribble(c, form, s, o, 4, 1, 48, 25);
    // then a despeckle pass over it
    BrushSettings ds;
    ds.size = 6;
    ds.tip = Tip::Round;
    scribble(c, DespeckleBrush(r), ds, o, 4, 0, 48, 25);
    const auto report = lint::checkLadder(c, r);
    INFO(rungMap(c, r) << findings(report));
    CHECK(count(report, Kind::RungSkip) == 0);
    CHECK(count(report, Kind::WideRung) == 0);
    CHECK(count(report, Kind::Speckle) == 0);
    CHECK(count(report, Kind::NonNeighbourShade) == 0);
    // and it is a ball: lit at the top left, the top rung's glint on it
    int lit = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!m.contains(x, y))
                CHECK(c.at(x, y) == black(1, 1).at(0, 0));
            lit += rungOf(r, c.at(x, y)) == r.topRung();
        }
    CHECK(lit >= 2);
    CHECK(*rungOf(r, c.at(20, 9)) > *rungOf(r, c.at(34, 18)));
}

TEST_CASE("edge distance is the exact distance to the nearest cell off the shape", "[shading][form]") {
    std::mt19937 rng(49);
    for (int trial = 0; trial < 200; ++trial) {
        const int w = 1 + int(rng() % 24), h = 1 + int(rng() % 14), fill = 30 + int(rng() % 70);
        Mask m(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (int(rng() % 100) < fill)
                    m.set(x, y);
        const auto d = edgeDistance(m);
        REQUIRE(d.size() == size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double best = 0;
                if (m.contains(x, y)) {
                    best = 1e300;
                    for (int by = -1; by <= h; ++by)
                        for (int bx = -1; bx <= w; ++bx)
                            if (!m.contains(bx, by))
                                best = std::min(best, std::hypot(double(bx - x), 2.0 * (by - y)));
                }
                INFO("trial " << trial << " " << w << "x" << h << " cell " << x << "," << y);
                CHECK(std::abs(d[size_t(y) * w + x] - best) < 1e-9);
            }
    }
}

TEST_CASE("form brush's first dab on a big selection is quick", "[shading][form]") {
    // Codex round 1, finding 1: a 160x1000 canvas, everything selected
    // The budgets are CPU time in an optimised build; cpuBudget scales them
    // for a Debug one (#285).
    const int w = 160, h = 1000;
    const Ramp r = steel();
    FormShadeBrush brush(r);
    BrushSettings s;
    // a dab of one cell alone on black is a speck the brush won't lay
    s.size = 9;
    StrokeOptions o;
    o.selection = shared(Mask(w, h, true));
    Canvas c = black(w, h);
    SECTION("everything selected") {
        // on the lit side: the middle of the ball is past the turn, black
        const double t0 = bwtest::cpuSeconds();
        const auto edit = paintStroke(c, brush, s, {{40, 250}}, o);
        const double t = bwtest::cpuSeconds() - t0;
        INFO(t << " s");
        CHECK(t < bwtest::cpuBudget(0.3));
        CHECK(!edit.empty());
    }
    SECTION("every other cell transparent, under the alpha lock") {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                c.at(x, y).transparent = (x + y) % 2 != 0;
        o.lockTransparent = true;
        const double t0 = bwtest::cpuSeconds();
        const auto edit = paintStroke(c, brush, s, {{80, 500}}, o);
        const double t = bwtest::cpuSeconds() - t0;
        INFO(t << " s");
        CHECK(t < bwtest::cpuBudget(0.3));
        // every cell alone among black: no form on it is lint-clean, so
        // none is laid
        CHECK(edit.empty());
    }
    SECTION("a dab off the canvas does no form work") {
        const double t0 = bwtest::cpuSeconds();
        const auto edit = paintStroke(c, brush, s, {{-50, -50}}, o);
        const double t = bwtest::cpuSeconds() - t0;
        INFO(t << " s");
        CHECK(t < bwtest::cpuBudget(0.05));
        CHECK(edit.empty());
    }
}

TEST_CASE("form brush's cache follows the shape, not where it lives", "[shading][form]") {
    const Ramp r = steel();
    const int w = 40, h = 20;
    BrushSettings s;
    s.size = 64;
    FormShadeBrush brush(r);
    auto fresh = [&](const Canvas& c, const StrokeOptions& o) {
        Canvas out = c;
        paintStroke(out, FormShadeBrush(r), s, {{20, 10}}, o).apply(out);
        return out;
    };
    SECTION("the same selection and canvas, a cell made transparent under the lock") {
        Canvas c = black(w, h);
        StrokeOptions o;
        o.selection = shared(disc(w, h, 20, 10, 30));
        o.lockTransparent = true;
        {
            Stroke st(c, brush, s, o);
            st.addDab({20, 10});
        }
        for (int x = 0; x < 20; ++x)
            for (int y = 0; y < h; ++y)
                c.at(x, y).transparent = true;
        Stroke st(c, brush, s, o);
        st.addDab({20, 10});
        CHECK(st.preview() == fresh(c, o));
    }
    SECTION("a new selection where the old one was") {
        Canvas c = black(w, h);
        StrokeOptions o;
        o.selection = shared(disc(w, h, 20, 10, 30));
        {
            Stroke st(c, brush, s, o);
            st.addDab({20, 10});
        }
        o.selection.reset();
        o.selection = shared(disc(w, h, 12, 8, 14));
        Stroke st(c, brush, s, o);
        st.addDab({20, 10});
        CHECK(st.preview() == fresh(c, o));
    }
}

namespace {

// A finding as the lint names it: its kind, its cell and its message (which
// carries the other cell of a skip and the rungs either side).
using FindingKey = std::tuple<int, int, int, std::string>;

std::set<FindingKey> findingKeys(const Canvas& c, const Ramp& r) {
    std::set<FindingKey> out;
    for (const auto& f : lint::checkLadder(c, r).findings)
        if (f.kind != Kind::OneGlyph && f.kind != Kind::Noise)
            out.insert({int(f.kind), f.x, f.y, f.message});
    return out;
}

// The findings `after` adds to `before` by the lint's own rule (the
// maintainer's ruling on PR #59): new by kind and place, a wide rung unless
// `before` had one the same way, on the same line, of the same rung,
// sharing a cell with it; one glyph and noise as the whole canvas reads.
std::vector<std::string> added(const Canvas& before, const Canvas& after, const Ramp& r) {
    std::vector<std::pair<int, int>> changed;
    for (int y = 0; y < after.height(); ++y)
        for (int x = 0; x < after.width(); ++x)
            if (!(before.at(x, y) == after.at(x, y)))
                changed.emplace_back(x, y);
    std::vector<std::string> out;
    for (const auto& f : lint::addedFindingsOnce(before, after, r, changed))
        out.push_back(f.message);
    return out;
}

// Two shading glyphs in the canvas's top left corner, out of the way of the
// strokes a test lays: a canvas that already shades with more than one
// glyph can take a change that shades with one (a stroke on a flat field
// of a solid rung that adds one glyph to it is a finding, the ruling's one
// glyph from false to true).
Canvas twoGlyphs(Canvas c, const Ramp& r) {
    c.at(0, 0) = rungCell(r, 1);
    c.at(1, 0) = rungCell(r, 2);
    return c;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& m : v)
        s += m + "\n";
    return s;
}

// A canvas of rungs from a function of the cell, clamped to the ramp.
template <class F> Canvas field(const Ramp& r, int w, int h, F rung) {
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c.at(x, y) = rungCell(r, std::clamp(int(rung(x, y)), 0, r.topRung()));
    return c;
}

const std::array<const char*, 5> kRamps{"steel", "fire", "ice", "toxic", "sunset"};

// The soft brush, checked after every dab it paints rather than once the
// stroke is done: the canvas so far has no lint finding the start didn't,
// every cell has moved only the brush's way from where the stroke began,
// and nothing it may not paint has changed. The first failure is kept.
class CheckedSoft : public Brush {
public:
    CheckedSoft(const SoftShadeBrush& brush, const Canvas& start, ShadeDirection dir)
        : brush_(brush), start_(start), dir_(dir) {}
    std::string_view name() const override { return brush_.name(); }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override {
        brush_.dab(stroke, d, size, strength);
        ++dabs;
        if (!failure.empty())
            return;
        const Ramp& r = brush_.ramp();
        const Canvas now = stroke.preview();
        std::ostringstream o;
        for (const auto& m : added(start_, now, r))
            o << "added: " << m << "\n";
        const int sign = dir_ == ShadeDirection::Lighten ? 1 : -1;
        for (int y = 0; y < now.height(); ++y)
            for (int x = 0; x < now.width(); ++x) {
                if (now.at(x, y) == start_.at(x, y))
                    continue;
                if (!stroke.paintable(x, y))
                    o << "cell " << x << "," << y << " changed outside what the stroke may paint\n";
                const auto was = rungOf(r, stroke.options().ice ? start_.at(x, y) : iceCell(start_.at(x, y), false));
                const auto is = rungOf(r, stroke.options().ice ? now.at(x, y) : iceCell(now.at(x, y), false));
                if (!was || !is || sign * (*is - *was) < 0)
                    o << "cell " << x << "," << y << " moved against the brush\n";
            }
        if (!o.str().empty())
            failure = "dab " + std::to_string(dabs) + " at " + std::to_string(d.x) + "," + std::to_string(d.y) +
                      " size " + std::to_string(size) + "\n" + rungMap(now, r) + o.str();
    }
    mutable int dabs = 0;
    mutable std::string failure;

private:
    const SoftShadeBrush& brush_;
    const Canvas& start_;
    ShadeDirection dir_;
};


// Codex round 1, findings 2 and 3; Claude round 1, P2-1 and P2-2: one dab
// and one line at every size in [from, to], both tips, five hardnesses and
// three strengths, both ways, low and high on the ramp, clean by the lint.
// Every dab of size 2 or more paints (Codex round 2, finding 1). A single
// size-1 dab is one cell, a speck by the lint's own rule (a lone cell
// lighter or darker than every neighbour) unless it reaches black or the
// top of the ramp: lint-clean wins, so it may leave a flat field as it was
// (PLAN §10). The fields are solid rungs (4) and shades (6); a solid one
// gets two glyphs in a corner, as a dab on it that shades with one glyph
// only would be the lint's one-glyph finding. The random strokes below
// check every dab as it lands.
void softSweep(int from, int to) {
    const Ramp r = steel();
    for (const int field : {4, 6})
        for (const auto dir : {ShadeDirection::Lighten, ShadeDirection::Darken})
            for (const Tip tip : {Tip::Round, Tip::Square})
                for (int size = from; size <= to; ++size)
                    for (const double hard : {0.0, 0.25, 0.5, 0.75, 1.0})
                        for (const double strength : {0.25, 0.5, 1.0}) {
                            const int w = size + 30, h = size / (tip == Tip::Round ? 2 : 1) + 8;
                            BrushSettings s;
                            s.size = size;
                            s.tip = tip;
                            s.strength = strength;
                            SoftShadeBrush brush(r, dir, hard);
                            const Canvas flat = twoGlyphs(filled(w, h, rungCell(r, field)), r);
                            Canvas one = flat;
                            paintStroke(flat, brush, s, {{w / 2, h / 2}}).apply(one);
                            Canvas line = flat;
                            paintStroke(flat, brush, s, {{size / 2 + 2, h / 2}, {w - size / 2 - 3, h / 2 - 1}})
                                .apply(line);
                            bool moved = false, lineMoved = false;
                            for (int y = 0; y < h; ++y)
                                for (int x = 0; x < w; ++x) {
                                    moved = moved || !(one.at(x, y) == flat.at(x, y));
                                    lineMoved = lineMoved || !(line.at(x, y) == flat.at(x, y));
                                }
                            const auto oneFound = added(flat, one, r), lineFound = added(flat, line, r);
                            if ((moved || size == 1) && lineMoved && oneFound.empty() && lineFound.empty())
                                continue;
                            INFO("field " << field << " dir " << int(dir) << " tip " << int(tip) << " size " << size
                                          << " hardness " << hard << " strength " << strength << "\n"
                                          << rungMap(one, r) << findings(lint::checkLadder(one, r)) << "line\n"
                                          << rungMap(line, r) << findings(lint::checkLadder(line, r)));
                            CHECK((moved || size == 1));
                            CHECK(lineMoved);
                            CHECK(oneFound.empty());
                            CHECK(lineFound.empty());
                        }
}

} // namespace

TEST_CASE("soft brush leaves a clean ladder at every size, tip, hardness and strength: 1-14", "[shading][soft]") {
    softSweep(1, 14);
}

TEST_CASE("soft brush leaves a clean ladder at every size, tip, hardness and strength: 15-21", "[shading][soft]") {
    softSweep(15, 21);
}

TEST_CASE("soft brush leaves a clean ladder at every size, tip, hardness and strength: 22-27", "[shading][soft]") {
    softSweep(22, 27);
}

TEST_CASE("soft brush leaves a clean ladder at every size, tip, hardness and strength: 28-32", "[shading][soft]") {
    softSweep(28, 32);
}

TEST_CASE("soft brush leaves a clean ladder at every size, tip, hardness and strength: 33-36", "[shading][soft]") {
    softSweep(33, 36);
}

TEST_CASE("soft brush leaves a clean ladder at every size, tip, hardness and strength: 37-39", "[shading][soft]") {
    softSweep(37, 39);
}

TEST_CASE("soft brush leaves a clean ladder at every size, tip, hardness and strength: 40-41", "[shading][soft]") {
    softSweep(40, 41);
}

TEST_CASE("soft brush repros from round 1", "[shading][soft]") {
    const Ramp r = steel();
    BrushSettings s;
    SECTION("a square tip falls off to its own edge") {
        Canvas c = filled(15, 11, rungCell(r, 4));
        s.size = 5;
        s.tip = Tip::Square;
        paintStroke(c, SoftShadeBrush(r, ShadeDirection::Lighten), s, {{7, 5}}).apply(c);
        const auto report = lint::checkLadder(c, r);
        INFO(rungMap(c, r) << findings(report));
        checkClean(report);
        CHECK(rungOf(r, c.at(7, 5)) > 4);
        CHECK(rungOf(r, c.at(5, 5)) == 5);  // the tip's edge moves a rung
    }
    SECTION("a big round tip has no wide rung, a mid one no speck") {
        for (const auto& [w, h, size] : {std::tuple{40, 24, 20}, std::tuple{40, 20, 15}}) {
            Canvas c = filled(w, h, rungCell(r, 4));
            s.size = size;
            s.tip = Tip::Round;
            paintStroke(c, SoftShadeBrush(r, ShadeDirection::Lighten), s, {{w / 2, h / 2}}).apply(c);
            const auto report = lint::checkLadder(c, r);
            INFO(rungMap(c, r) << findings(report));
            checkClean(report);
        }
    }
    SECTION("a small tip moves every cell it covers") {
        // the corner cut leaves a tip three cells or fewer across whole, and
        // a two by two square (all corners) keeps them
        for (const auto& [size, tip] : {std::pair{1, Tip::Round}, {2, Tip::Round}, {3, Tip::Round}, {1, Tip::Square},
                                        {2, Tip::Square}}) {
            const auto f = softFalloff({20, 10}, size, tip, 40, 20, 1.0, 0.0);
            CHECK(f.size() == footprint(20, 10, size, tip).size());
        }
    }
    SECTION("a big tip leaves its corners out") {
        const auto f = softFalloff({20, 10}, 8, Tip::Square, 40, 20, 1.0, 0.0);
        CHECK(f.size() == footprint(20, 10, 8, Tip::Square).size() - 4);
    }
    SECTION("a hard tip moves every cell its strength's share of a colour") {
        s.size = 9;
        for (const double strength : {0.25, 0.5, 1.0}) {
            const int wanted = std::max(1, int(std::lround(4 * strength)));
            for (const auto& [xy, n] : softFalloff({20, 10}, 9, Tip::Round, 40, 20, strength, 1.0))
                CHECK(n == wanted);
        }
    }
}

namespace {

} // namespace

namespace {

// The widest a rung of the turn runs along the gradient, as the lint
// measures a step (the shorter of its run and its longest run the other
// way), over every rung between black and the lit plane: "no
// rung wider than about 4 cells on a form" judges solid rungs too.
int widestTurn(const Canvas& c, const Ramp& r, const Mask& m, int plane) {
    auto g = [&](int x, int y) { return m.contains(x, y) ? rungOf(r, c.at(x, y)).value_or(-1) : -1; };
    auto runLen = [&](int x, int y, int dx, int dy) {
        const int v = g(x, y);
        int a = 0, b = 0;
        while (g(x - (a + 1) * dx, y - (a + 1) * dy) == v)
            ++a;
        while (g(x + (b + 1) * dx, y + (b + 1) * dy) == v)
            ++b;
        return a + b + 1;
    };
    int widest = 0;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{0, 1}}) {
                const int v = g(x, y);
                if (v <= 0 || v >= plane || g(x - dx, y - dy) == v)
                    continue;
                int len = 1;
                while (g(x + len * dx, y + len * dy) == v)
                    ++len;
                const int lo = g(x - dx, y - dy), hi = g(x + len * dx, y + len * dy);
                if (lo < 0 || hi < 0 || (lo < v) == (hi < v))
                    continue;
                int across = 0;
                for (int i = 0; i < len; ++i)
                    across = std::max(across, runLen(x + i * dx, y + i * dy, dy, dx));
                widest = std::max(widest, std::min(len, across));
            }
    return widest;
}

// The form's cells cut to its box, for the detail: what
// `--box <subject>` reads.
lint::Detail formDetail(const Canvas& c, const Mask& m) {
    int x0 = m.width(), y0 = m.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.contains(x, y))
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    Canvas box(x1 - x0 + 1, y1 - y0 + 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            box.at(x - x0, y - y0) = c.at(x, y);
    return lint::detail(box);
}

// The shade glyphs the form uses.
std::set<char32_t> shadeGlyphs(const Canvas& c, const Mask& m) {
    std::set<char32_t> out;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.contains(x, y) && (c.at(x, y).ch == kShadeLight || c.at(x, y).ch == kShadeMedium ||
                                     c.at(x, y).ch == kShadeDark))
                out.insert(c.at(x, y).ch.codePoint().value());
    return out;
}

// The rows (dx 1) or columns (dy 1) of the form with eight or more cells
// short of its black shadow, and how many of them are one rung from end to
// end: a stripe.
std::pair<int, int> stripes(const Canvas& c, const Ramp& r, const Mask& m, bool rows) {
    int lines = 0, flat = 0;
    for (int l = 0; l < (rows ? m.height() : m.width()); ++l) {
        std::set<int> seen;
        int n = 0;
        for (int i = 0; i < (rows ? m.width() : m.height()); ++i) {
            const int x = rows ? i : l, y = rows ? l : i;
            if (m.contains(x, y) && !(c.at(x, y) == rungCell(r, 0) && r.colours[0] == Black)) {
                ++n;
                seen.insert(rungOf(r, c.at(x, y)).value_or(-1));
            }
        }
        if (n >= 8) {
            ++lines;
            flat += seen.size() == 1;
        }
    }
    return {lines, flat};
}

} // namespace

TEST_CASE("form tones spread over the whole form under every light", "[shading][form]") {
    // Claude round 1, P2-4: the shadow side was a third flat rung 1, a plane
    // reached only twenty columns, and the second ladder pass fanned a seam
    // out of the glint.
    const Ramp r = steel();
    struct Case {
        const char* name;
        Mask m;
        FormShape shape;
    };
    Mask plane(57, 20);
    for (int y = 2; y < 18; ++y)
        for (int x = 4; x < 53; ++x)
            plane.set(x, y);
    const std::vector<Case> cases{{"disc", disc(38, 21, 19, 10.5, 30), FormShape::Dome},
                                  {"plane", plane, FormShape::Plane}};
    int discBar = 0;
    for (const auto& k : cases)
        for (int l = 0; l < 8; ++l)
            for (const bool spec : {true, false}) {
                FormCase o;
                o.shape = k.shape;
                o.light = Compass(l);
                o.specular = spec;
                const Canvas c = toneCanvas(r, k.m, o);
                const auto report = lint::checkLadder(c, r);
                INFO(k.name << " light " << l << " spec " << spec << "\n" << rungMap(c, r) << findings(report));
                checkClean(report);
                // the bar (detail, under 15% flat, no
                // rung of the turn over four cells), read at the box's
                // corner as for a piece: with its black held to the
                // light's shadow and ten points and its lit plane to the
                // light's lit share, a ball meets it under a few lights and
                // comes within about a point of detail under the rest, its
                // flat share at the bar's edge; a flat plane's straight
                // bands give a 4x2 window at most three rungs, and its lit
                // share is a broad flat field, so it is held to the flat
                // share and the rung width only
                const auto d = formDetail(c, k.m);
                INFO("detail " << d.mean << ", " << d.flatPercent << "% flat");
                CHECK(d.flatPercent < (k.shape == FormShape::Dome ? 16 : 21));
                CHECK(widestTurn(c, r, k.m, spec && k.shape != FormShape::Plane ? 8 : r.topRung()) <= 4);
                CHECK(seams(c, r, k.m) == 0);
                if (k.shape == FormShape::Dome) {
                    CHECK(d.mean >= 2.5);
                    discBar += d.mean >= 3.6;
                }
            }
    CHECK(discBar >= 4);  // of 16
}

TEST_CASE("form glint makes no wide rung and sits mid-cylinder", "[shading][form]") {
    const Ramp r = steel();
    SECTION("Codex round 1, finding 4: a 10x7 cylinder lit from the north") {
        const int w = 10, h = 7;
        FormCase fo;
        fo.shape = FormShape::Cylinder;
        fo.light = Compass::N;
        fo.rim = false;
        fo.specular = true;
        StrokeOptions o;
        o.selection = shared(Mask(w, h, true));
        BrushSettings s;
        s.size = 32;
        Canvas c = black(w, h);
        s.lightDirection = fo.light;
        paintStroke(c, FormShadeBrush(r, fo), s, {{5, 3}}, o).apply(c);
        const auto report = lint::checkLadder(c, r);
        INFO(rungMap(c, r) << findings(report));
        checkClean(report);
    }
    SECTION("Claude round 1, P3-1: the glint is in the middle of the long axis") {
        // a 10-row log and a 12-column pillar have no room for a glint that
        // stays on the lit plane (Claude round 5, P3-A): their top rung is
        // the plane; a 14-row log and a 16-column pillar do
        for (const auto& [w, h, wide, room] : {std::tuple{49, 10, true, false}, std::tuple{12, 20, false, false},
                                               std::tuple{49, 14, true, true}, std::tuple{16, 20, false, true}}) {
            Mask m(w + 6, h + 4);
            for (int y = 2; y < h + 2; ++y)
                for (int x = 3; x < w + 3; ++x)
                    m.set(x, y);
            FormCase fo;
            fo.shape = FormShape::Cylinder;
            const FormResult f = formShade(r, m, fo, fo.light);
            const auto& tones = f.tones;
            INFO(w << "x" << h << "\n" << rungMap(toneCanvas(r, m, fo), r));
            if (!room) {
                CHECK(f.reading.plane == r.topRung());
                continue;
            }
            std::vector<std::pair<int, int>> glint;
            for (int y = 0; y < m.height(); ++y)
                for (int x = 0; x < m.width(); ++x)
                    if (tones[size_t(y) * m.width() + x] == r.topRung())
                        glint.emplace_back(x, y);
            INFO(w << "x" << h << "\n" << rungMap(toneCanvas(r, m, fo), r));
            REQUIRE(glint.size() == 2);
            const double along = wide ? glint[0].first - 3 - (w - 1) / 2.0 : glint[0].second - 2 - (h - 1) / 2.0;
            CHECK(std::abs(along) <= 1.0);
        }
    }
}

TEST_CASE("form brush with iCE off writes a clean ladder", "[shading][form]") {
    // Codex round 1, finding 5; Claude round 1, P2-3
    SECTION("a fire plane, 17x3") {
        const Ramp r = *findRamp("fire");
        const int w = 17, h = 3;
        FormCase fo;
        fo.shape = FormShape::Plane;
        fo.light = Compass::W;
        fo.rim = false;
        fo.specular = false;
        StrokeOptions o;
        o.selection = shared(Mask(w, h, true));
        o.ice = false;
        BrushSettings s;
        s.size = 32;
        Canvas c = black(w, h);
        s.lightDirection = fo.light;
        paintStroke(c, FormShadeBrush(r, fo), s, {{8, 1}}, o).apply(c);
        const auto report = lint::checkLadder(c, r);
        INFO(rungMap(c, r) << findings(report));
        checkClean(report);
    }
    SECTION("domes on every ramp with bright neighbours") {
        for (const char* name : {"fire", "ice", "toxic", "sunset"}) {
            const Ramp r = *findRamp(name);
            const int w = 80, h = 25;
            for (const bool spec : {true, false}) {
                FormCase fo;
                fo.specular = spec;
                StrokeOptions o;
                o.selection = shared(disc(w, h, 40, 12, 34));
                o.ice = false;
                BrushSettings s;
                s.size = 5;
                s.tip = Tip::Round;
                Canvas c = black(w, h);
                for (int y = 0; y < h; ++y)
                    paintStroke(c, FormShadeBrush(r, fo), s, {{18, y}, {62, y}}, o).apply(c);
                const auto report = lint::checkLadder(c, r);
                INFO(name << " spec " << spec << "\n" << rungMap(c, r) << findings(report));
                checkClean(report);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        CHECK(c.at(x, y).bg.paletteIndex() < 8);
            }
        }
    }
}

TEST_CASE("soft brush with iCE off writes a clean ladder", "[shading][soft]") {
    for (const char* name : {"fire", "ice", "toxic", "sunset", "steel"}) {
        const Ramp r = *findRamp(name);
        for (const auto dir : {ShadeDirection::Lighten, ShadeDirection::Darken})
            for (const int size : {5, 15, 25}) {
                StrokeOptions o;
                o.ice = false;
                BrushSettings s;
                s.size = size;
                s.tip = Tip::Round;
                Canvas c = filled(60, 24, rungCell(r, 6));
                paintStroke(c, SoftShadeBrush(r, dir), s, {{30, 12}}, o).apply(c);
                const auto report = lint::checkLadder(c, r);
                INFO(name << " dir " << int(dir) << " size " << size << "\n" << rungMap(c, r) << findings(report));
                checkClean(report);
                for (int y = 0; y < c.height(); ++y)
                    for (int x = 0; x < c.width(); ++x)
                        CHECK(c.at(x, y).bg.paletteIndex() < 8);
            }
    }
}

TEST_CASE("a growing form keeps each cell's strongest dab and replays the same", "[shading][form]") {
    // Codex round 1, finding 6, and PLAN §10's one exception to "a cell
    // changes once": the form brush with nothing selected lays the whole
    // form again as the stroke grows, so a cell's tone follows the shape,
    // but its strength stays the strongest dab's and nothing compounds.
    const Ramp r = steel();
    FormCase fo;
    fo.shape = FormShape::Plane;
    fo.light = Compass::W;
    fo.specular = false;
    FormShadeBrush brush(r, fo);
    BrushSettings s;
    s.lightDirection = fo.light;
    s.pressure = PressureTo::Strength;
    // dabs of three by three: one cell alone on black is a speck the brush
    // won't lay
    s.size = 3;
    s.tip = Tip::Square;
    const Canvas c = black(12, 5);
    auto box = [](int x0, int x1) {
        Mask m(12, 5);
        for (int y = 1; y <= 3; ++y)
            for (int x = x0; x <= x1; ++x)
                m.set(x, y);
        return m;
    };
    Stroke st(c, brush, s);
    st.addDab({6, 2, 1.0});
    const auto first = formRungs(r, box(5, 7), fo, fo.light);
    CHECK(rungOf(r, st.preview().at(5, 2)) == first[size_t(2) * 12 + 5]);
    st.addDab({5, 2, 0.1});
    CHECK(st.paintedStrength(6, 2) == 1.0);
    CHECK(st.paintedStrength(4, 2) == 0.1);
    // its tone is the wider form's, at full strength
    const auto tones = formRungs(r, box(4, 7), fo, fo.light);
    CHECK(rungOf(r, st.preview().at(6, 2)) == tones[size_t(2) * 12 + 6]);
    CHECK(first[size_t(2) * 12 + 6] != tones[size_t(2) * 12 + 6]);

    Stroke twice(c, brush, s);
    for (int k = 0; k < 2; ++k) {
        twice.addDab({6, 2, 1.0}, false);
        twice.addDab({5, 2, 0.1});
    }
    CHECK(twice.preview() == st.preview());
}

TEST_CASE("despeckle keeps the cell's own transparency", "[shading][despeckle]") {
    // Codex round 1, finding 7
    const Ramp r = steel();
    Canvas c = filled(3, 3, rungCell(r, 8));
    c.at(1, 1) = rungCell(r, 4);
    c.at(1, 2).transparent = true;
    StrokeOptions o;
    o.lockTransparent = true;
    BrushSettings s;
    paintStroke(c, DespeckleBrush(r), s, {{1, 1}}, o).apply(c);
    CHECK_FALSE(c.at(1, 1).transparent);
    CHECK(rungOf(r, c.at(1, 1)) == 8);
    CHECK(c.at(1, 2).transparent);
}

TEST_CASE("despeckle round 8, P3-B: a tie that adds a finding gives way to the other rung", "[shading][despeckle]") {
    // #727 P3-B: a speck at rung 4 on the steel sphere, 7 above and to the
    // left, 6 below and to the right. The tie went to 6, the nearer, which
    // makes a rung four cells wide, and the fix was dropped; 7 is clean.
    const Ramp r = steel();
    const Mask m = disc(44, 22, 22, 11, 36);
    const FormResult f = formShade(r, m, FormOptions{}, Compass::NW);
    Canvas sphere = black(44, 22);
    for (int y = 0; y < 22; ++y)
        for (int x = 0; x < 44; ++x)
            if (const int t = f.tones[size_t(y) * 44 + x]; t >= 0)
                sphere.at(x, y) = rungCell(r, t);
    sphere.at(26, 8) = rungCell(r, 4);
    REQUIRE(rungOf(r, sphere.at(26, 7)) == 7);
    REQUIRE(rungOf(r, sphere.at(25, 8)) == 7);
    REQUIRE(rungOf(r, sphere.at(26, 9)) == 6);
    REQUIRE(rungOf(r, sphere.at(27, 8)) == 6);
    for (const int size : {1, 3, 6}) {
        BrushSettings s;
        s.size = size;
        s.tip = Tip::Square;
        Canvas c = sphere;
        paintStroke(sphere, DespeckleBrush(r), s, {{26, 8}}).apply(c);
        INFO("size " << size << "\n" << rungMap(c, r));
        CHECK(rungOf(r, c.at(26, 8)) == 7);
    }
}

TEST_CASE("despeckle round 9, Claude #742 N1: a fallback doesn't take an outlying neighbour's rung",
          "[shading][despeckle]") {
    // A near-white speck (rung 19) with 17 below, 3 above, 7 left and 0
    // right. The guard refuses 17, 7 and 3; round 8 took the 0, the far end
    // of its neighbours, which reads as a hole. The far end of theirs, when
    // one neighbour alone holds it, is no fallback, so the speck stays.
    const Ramp r = *findRamp("sunset");
    const std::array<std::string, 11> rows{"hfjbhihhhh17h6h7h", "c39a2h56hhigd2h2c", "7i6d1jgkhhhcchekb",
                                           "h2hgjjihh1k52hd47", "hh5h9bhd4h2hihh9i", "hhjfbcfhhhh8ijgb1",
                                           "fhighhhd3hd553k2h", "hihhh9hhch527j0h1", "jhi7hab442jhjhhe2",
                                           "jhb31h33bjhc6h44h", "jbhf56g9hhhcekgh0"};
    const Canvas start = field(r, 17, 11, [&](int x, int y) {
        const char c = rows[size_t(y)][size_t(x)];
        return c <= '9' ? c - '0' : c - 'a' + 10;
    });
    REQUIRE(rungOf(r, start.at(13, 7)) == 19);
    REQUIRE(rungOf(r, start.at(13, 8)) == 17);
    REQUIRE(rungOf(r, start.at(13, 6)) == 3);
    REQUIRE(rungOf(r, start.at(12, 7)) == 7);
    REQUIRE(rungOf(r, start.at(14, 7)) == 0);
    BrushSettings s;
    s.size = 1;
    Canvas c = start;
    paintStroke(start, DespeckleBrush(r), s, {{13, 7}}).apply(c);
    INFO(rungMap(c, r));
    CHECK(rungOf(r, c.at(13, 7)) == 19);
    CHECK(added(start, c, r).empty());
}

TEST_CASE("despeckle round 10, Claude #747 P2-1: a fallback still takes the rung nearest the speck",
          "[shading][despeckle]") {
    // A dark speck (2) with 8 above and below, 7 left and 3 right. The
    // guard refuses 8 (a new skip from 8 to the 3) and 7 (7 to 3). The 3,
    // one rung from the speck and the near end of its neighbours, clears it
    // and adds nothing; round 9 dropped it with the far end and left the 2.
    const Ramp r = steel();
    Canvas start = filled(5, 5, rungCell(r, 8));
    start.at(2, 2) = rungCell(r, 2);
    start.at(1, 2) = rungCell(r, 7);
    start.at(3, 2) = rungCell(r, 3);
    BrushSettings s;
    s.size = 1;
    Canvas c = start;
    paintStroke(start, DespeckleBrush(r), s, {{2, 2}}).apply(c);
    INFO(rungMap(c, r));
    CHECK(rungOf(r, c.at(2, 2)) == 3);
    CHECK(added(start, c, r).empty());
}

TEST_CASE("despeckle round 10, Codex #746: a clean fallback at the near end isn't dropped", "[shading][despeckle]") {
    // A dark speck (1) with 2 below, 3 above and left, 6 right. The 3 makes
    // the 2 below a speck, so the guard refuses it; round 9 dropped the 2
    // (the neighbours' lowest, one holds it) and the 6, and left the 1. The
    // 2 is clean. The 6, the far end, is still no fallback.
    const Ramp r = steel();
    const std::array<std::array<int, 3>, 3> rows{{{6, 3, 10}, {3, 1, 6}, {11, 2, 5}}};
    const Canvas start = field(r, 3, 3, [&](int x, int y) { return rows[size_t(y)][size_t(x)]; });
    StrokeOptions o;
    o.ice = true;
    BrushSettings s;
    s.size = 1;
    CHECK(speckFixes(r, 1, 1, [&](int x, int y) -> std::optional<int> {
                if (x < 0 || y < 0 || x > 2 || y > 2)
                    return std::nullopt;
                return rows[size_t(y)][size_t(x)];
            }) == std::vector<int>{3, 2});
    Canvas c = start;
    paintStroke(start, DespeckleBrush(r), s, {{1, 1}}, o).apply(c);
    INFO(rungMap(c, r));
    CHECK(rungOf(r, c.at(1, 1)) == 2);
    CHECK(added(start, c, r).empty());
}

TEST_CASE("soft brush repros from round 2", "[shading][soft]") {
    const Ramp r = steel();
    SECTION("Codex round 2, finding 2: a bent stroke's overlapping dabs") {
        BrushSettings s;
        s.size = 15;
        s.tip = Tip::Round;
        Canvas c = filled(35, 29, rungCell(r, 4));
        paintStroke(c, SoftShadeBrush(r, ShadeDirection::Lighten, 0.0), s, {{23, 19}, {25, 9}, {9, 12}}).apply(c);
        const auto report = lint::checkLadder(c, r);
        INFO(rungMap(c, r) << findings(report));
        checkClean(report);
        CHECK(rungOf(r, c.at(24, 14)) == 8);
    }
    SECTION("Codex round 2, finding 3: a hard tip's pressure change ramps") {
        BrushSettings s;
        s.size = 5;
        s.tip = Tip::Square;
        s.strength = 1.0;
        s.pressure = PressureTo::Strength;
        Canvas c = filled(20, 12, rungCell(r, 4));
        paintStroke(c, SoftShadeBrush(r, ShadeDirection::Lighten, 1.0), s, {{6, 6, 0.25}, {10, 6, 1.0}}).apply(c);
        const auto report = lint::checkLadder(c, r);
        INFO(rungMap(c, r) << findings(report));
        checkClean(report);
        // neighbours on the stroke a rung apart at most, and the full press
        // still reaches its colour
        for (int y = 4; y <= 8; ++y)
            for (int x = 4; x < 12; ++x)
                CHECK(std::abs(*rungOf(r, c.at(x + 1, y)) - *rungOf(r, c.at(x, y))) <= 1);
        CHECK(rungOf(r, c.at(10, 6)) == 8);
    }
}

// Strokes [from, to) of one seeded run, in two cases to keep each short.
void randomJoined(int from, int to) {
    // Codex round 2, findings 2 and 3: the stroke, not each tip, is clean.
    // Seeded strokes of 2-6 points that bend and cross themselves, every
    // size from 1 to 41, both tips, three hardnesses, constant and changing
    // pressure to strength and to size, both ways, on flat fields low, in
    // the middle and high on the ramp.
    const Ramp r = steel();
    std::mt19937 rng(20261022);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int w = 40, h = 22, strokes = to - from;
    int dirty = 0, blank = 0;
    std::string first;
    const double start = bwtest::cpuSeconds();
    for (int k = 0; k < to; ++k) {
        const int field = std::array{2, 6, 10}[size_t(pick(0, 2))];
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        BrushSettings s;
        s.size = pick(1, 41);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        s.pressure = std::array{PressureTo::Nothing, PressureTo::Strength, PressureTo::Size}[size_t(pick(0, 2))];
        const bool varying = pick(0, 1);
        const double constant = 0.1 * pick(1, 10);
        std::vector<Dab> dabs;
        for (int i = 0, n = pick(2, 6); i < n; ++i)
            dabs.push_back({pick(0, w - 1), pick(0, h - 1), varying ? 0.05 * pick(1, 20) : constant});
        if (k < from)
            continue;
        const Canvas flat = filled(w, h, rungCell(r, field));
        Canvas c = flat;
        paintStroke(c, SoftShadeBrush(r, dir, hard), s, dabs).apply(c);
        const auto report = lint::checkLadder(c, r);
        int changed = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                changed += !(c.at(x, y) == flat.at(x, y));
        // Claude round 3, P3-1: no speck exemption, a lone cell is a finding
        blank += changed == 0;
        if (count(report, Kind::RungSkip) + count(report, Kind::WideRung) + count(report, Kind::Speckle) == 0)
            continue;
        if (!dirty++) {
            std::ostringstream o;
            o << "stroke " << k << ": field " << field << " dir " << int(dir) << " hardness " << hard << " size "
              << s.size << " tip " << int(s.tip) << " strength " << s.strength << " pressure " << int(s.pressure)
              << "\n";
            for (const Dab& d : dabs)
                o << "(" << d.x << "," << d.y << "," << d.pressure << ") ";
            o << "\n" << rungMap(c, r) << findings(report);
            first = o.str();
        }
    }
    const double took = bwtest::cpuSeconds() - start;
    INFO(dirty << " of " << strokes << " strokes with findings; " << blank << " left the canvas as it was; " << took
               << " s\n"
               << first);
    CHECK(dirty == 0);
    CHECK(blank == 0);
}

TEST_CASE("soft strokes are lint-clean: random joined strokes 0-999", "[shading][soft]") {
    randomJoined(0, 1000);
}

TEST_CASE("soft strokes are lint-clean: random joined strokes 1000-1999", "[shading][soft]") {
    randomJoined(1000, 2000);
}

TEST_CASE("form brush repros from round 2", "[shading][form]") {
    SECTION("Codex round 2, finding 4: the iCE-off ceiling is no glint") {
        for (const char* name : {"fire", "ice", "toxic", "sunset"}) {
            const Ramp r = *findRamp(name);
            FormCase fo;
            fo.shape = FormShape::Dome;
            fo.light = Compass::E;
            fo.rim = false;
            fo.specular = true;
            StrokeOptions o;
            o.selection = shared(Mask(3, 3, true));
            o.ice = false;
            BrushSettings s;
            s.size = 8;
            s.tip = Tip::Square;
            Canvas c = black(3, 3);
            s.lightDirection = fo.light;
            paintStroke(c, FormShadeBrush(r, fo), s, {{1, 1}}, o).apply(c);
            const auto report = lint::checkLadder(c, r);
            INFO(name << "\n" << rungMap(c, r) << findings(report));
            checkClean(report);
        }
    }
    SECTION("Codex round 2, finding 5: a 25x13 dome lit from the north-east") {
        const Ramp r = steel();
        FormCase fo;
        fo.shape = FormShape::Dome;
        fo.light = Compass::NE;
        fo.rim = false;
        fo.specular = false;
        StrokeOptions o;
        o.selection = shared(Mask(25, 13, true));
        BrushSettings s;
        s.size = 32;
        s.tip = Tip::Square;
        Canvas c = black(25, 13);
        s.lightDirection = fo.light;
        paintStroke(c, FormShadeBrush(r, fo), s, {{12, 6}}, o).apply(c);
        const auto report = lint::checkLadder(c, r);
        INFO(rungMap(c, r) << findings(report));
        checkClean(report);
    }
}

TEST_CASE("form brush is lint-clean: random shapes", "[shading][form]") {
    // Codex round 2, finding 5: bands bounded by the runs the lint measures.
    // Domes, cylinders and planes over rectangles, ellipses and L shapes
    // 3-40 by 3-20, under every light, with and without the glint and iCE,
    // on five ramps.
    std::mt19937 rng(20261023);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const std::array<const char*, 5> ramps{"steel", "fire", "ice", "toxic", "sunset"};
    const int cases = 1200;
    int dirty = 0;
    std::string first;
    for (int k = 0; k < cases; ++k) {
        const Ramp r = *findRamp(ramps[size_t(pick(0, 4))]);
        const int sw = pick(3, 40), sh = pick(3, 20), kind = pick(0, 2);
        const int w = sw + 4, h = sh + 4;
        Mask m(w, h);
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) {
                const double dx = (x + 0.5) / sw - 0.5, dy = (y + 0.5) / sh - 0.5;
                const bool in = kind == 0   ? true
                                : kind == 1 ? dx * dx + dy * dy <= 0.25
                                            : x < (sw + 1) / 2 || y >= sh / 2;
                if (in)
                    m.set(x + 2, y + 2);
            }
        FormCase fo;
        fo.shape = std::array{FormShape::Dome, FormShape::Cylinder, FormShape::Plane}[size_t(pick(0, 2))];
        fo.light = Compass(pick(0, 7));
        fo.rim = false;
        fo.specular = pick(0, 1);
        StrokeOptions o;
        o.selection = shared(m);
        o.ice = pick(0, 1);
        BrushSettings s;
        s.size = 2 * std::max(w, h) + 2;
        s.tip = Tip::Square;
        Canvas c = black(w, h);
        s.lightDirection = fo.light;
        paintStroke(c, FormShadeBrush(r, fo), s, {{w / 2, h / 2}}, o).apply(c);
        const auto report = lint::checkLadder(c, r);
        if (count(report, Kind::RungSkip) + count(report, Kind::WideRung) + count(report, Kind::Speckle) == 0)
            continue;
        if (!dirty++) {
            std::ostringstream out;
            out << "case " << k << ": " << r.name << " " << sw << "x" << sh << " kind " << kind << " shape "
                << int(fo.shape) << " light " << int(fo.light) << " glint " << fo.specular << " ice " << o.ice
                << "\n"
                << rungMap(c, r) << findings(report);
            first = out.str();
        }
    }
    INFO(dirty << " of " << cases << " forms with findings\n" << first);
    CHECK(dirty == 0);
}

TEST_CASE("soft brush repros from round 3: a clean gradient stays clean", "[shading][soft]") {
    BrushSettings s;
    s.size = 2;
    s.tip = Tip::Round;
    for (const char* name : kRamps)
        for (const bool ice : {true, false}) {
            const Ramp r = *findRamp(name);
            StrokeOptions o;
            o.ice = ice;
            SECTION(std::string("Codex round 3, finding 1: a lone lighter cell on a two-column gradient, ") + name +
                    (ice ? "" : ", iCE off")) {
                const Canvas start = field(r, 10, 7, [](int x, int) { return 1 + x / 2; });
                REQUIRE(findingKeys(start, r).empty());
                Canvas c = start;
                paintStroke(c, SoftShadeBrush(r, ShadeDirection::Lighten, 0.0), s, {{6, 3}}, o).apply(c);
                const auto more = added(start, c, r);
                INFO(rungMap(c, r) << joined(more));
                CHECK(more.empty());
            }
            SECTION(std::string("Codex round 3, finding 2: a one-column gradient has no clean change, ") + name +
                    (ice ? "" : ", iCE off")) {
                // Two cells a rung up each on 1 2 3 ... 10 skip their
                // neighbours whichever way they go: lint-clean wins, so the
                // dab may leave the canvas as it was (PLAN §10).
                const Canvas start = field(r, 10, 7, [](int x, int) { return 1 + x; });
                REQUIRE(findingKeys(start, r).empty());
                Canvas c = start;
                paintStroke(c, SoftShadeBrush(r, ShadeDirection::Lighten, 0.0), s, {{6, 3}}, o).apply(c);
                const auto more = added(start, c, r);
                INFO(rungMap(c, r) << joined(more));
                CHECK(more.empty());
            }
        }
}

TEST_CASE("soft brush repros from round 3: Claude's gradient and selection edge", "[shading][soft]") {
    const Ramp r = steel();
    SECTION("Claude round 3, P3-1: size 2 and size 5 on a two-row gradient") {
        const Canvas start = field(r, 30, 24, [](int, int y) { return std::min(12, 1 + y / 2); });
        REQUIRE(findingKeys(start, r).empty());
        for (const auto& [size, tip, strength, x, y] :
             {std::tuple{2, Tip::Square, 0.5, 15, 4}, std::tuple{5, Tip::Round, 1.0, 15, 10}}) {
            BrushSettings s;
            s.size = size;
            s.tip = tip;
            s.strength = strength;
            Canvas c = start;
            paintStroke(c, SoftShadeBrush(r, ShadeDirection::Lighten, 0.0), s, {{x, y}}).apply(c);
            const auto more = added(start, c, r);
            INFO("size " << size << "\n" << rungMap(c, r) << joined(more));
            CHECK(more.empty());
        }
    }
    SECTION("Claude round 3, P3-2: a footprint that meets the selection at two cells paints them") {
        const Canvas start = filled(40, 22, rungCell(r, 6));
        StrokeOptions o;
        o.selection = shared(Mask::rect(40, 22, 15, 2, 35, 12));
        BrushSettings s;
        s.size = 34;
        s.tip = Tip::Round;
        s.strength = 0.25;
        const CellEdit e = paintStroke(start, SoftShadeBrush(r, ShadeDirection::Darken, 0.0), s, {{11, 20}}, o);
        Canvas c = start;
        e.apply(c);
        const auto more = added(start, c, r);
        INFO(rungMap(c, r) << joined(more));
        CHECK_FALSE(e.changes.empty());
        CHECK(more.empty());
    }
}

TEST_CASE("soft brush's tip depth doesn't depend on earlier strokes", "[shading][soft]") {
    // Codex round 3, finding 5: a dab far off the canvas clipped the tip the
    // depth was cached from.
    const Ramp r = steel();
    BrushSettings s;
    s.size = 5;
    s.tip = Tip::Square;
    const SoftShadeBrush brush(r, ShadeDirection::Lighten, 0.0);
    auto middle = [&](bool offFirst) {
        int rung = -1;
        std::thread t([&] {
            const Canvas c = filled(20, 12, rungCell(r, 4));
            if (offFirst) {
                Stroke off(c, brush, s);
                off.addDab({std::numeric_limits<int>::min(), std::numeric_limits<int>::min()});
            }
            Stroke st(c, brush, s);
            st.addDab({10, 6});
            rung = rungOf(r, st.preview().at(10, 6)).value_or(-1);
        });
        t.join();
        return rung;
    };
    CHECK(middle(false) == 6);
    CHECK(middle(true) == 6);
}

namespace {

// An L like the random-shapes test's: sw by sh at (2, 2) on a canvas two
// cells bigger all round, the left half full height and the bottom half
// full width.
Mask ell(int sw, int sh) {
    Mask m(sw + 4, sh + 4);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x)
            if (x < (sw + 1) / 2 || y >= sh / 2)
                m.set(x + 2, y + 2);
    return m;
}

// The form brush laid over the whole of a selected shape in one dab.
Canvas formDab(const Ramp& r, const Mask& m, const FormCase& fo, bool ice = true) {
    StrokeOptions o;
    o.selection = shared(m);
    o.ice = ice;
    BrushSettings s;
    s.size = 2 * std::max(m.width(), m.height()) + 2;
    s.tip = Tip::Square;
    Canvas c = black(m.width(), m.height());
    s.lightDirection = fo.light;
    paintStroke(c, FormShadeBrush(r, fo), s, {{m.width() / 2, m.height() / 2}}, o).apply(c);
    return c;
}


} // namespace

TEST_CASE("form brush repros from round 3", "[shading][form]") {
    const Ramp r = steel();
    SECTION("Codex round 3, finding 3: a 22x19 dome lit from the north-east keeps under 15% a rung") {
        const Mask m = Mask::rect(26, 23, 2, 2, 23, 20);
        FormCase fo;
        fo.shape = FormShape::Dome;
        fo.light = Compass::NE;
        fo.rim = false;
        fo.specular = false;
        const Canvas c = formDab(r, m, fo);
        const auto report = lint::checkLadder(c, r);
        const auto d = formDetail(c, m);
        INFO(rungMap(c, r) << findings(report) << "detail " << d.mean << ", " << d.flatPercent << "% flat");
        checkClean(report);
        // the bar, in place of a 15% cap on each rung
        CHECK(d.mean >= 3.6);
        CHECK(d.flatPercent < 15);
        CHECK(widestTurn(c, r, m, r.topRung()) <= 4);
    }
    SECTION("Codex round 3, finding 4: a holed cylinder lit from the south-east") {
        Mask m = Mask::rect(43, 30, 2, 2, 40, 27);
        for (int y = 10; y <= 18; ++y)
            for (int x = 15; x <= 27; ++x)
                m.set(x, y, false);
        FormCase fo;
        fo.shape = FormShape::Cylinder;
        fo.light = Compass::SE;
        fo.rim = true;
        fo.specular = false;
        const Canvas c = formDab(r, m, fo);
        const auto report = lint::checkLadder(c, r);
        INFO(rungMap(c, r) << findings(report));
        checkClean(report);
    }
    SECTION("Claude round 3, P3-3: an ice L cylinder 29x30 lit from the north") {
        const Ramp ice = *findRamp("ice");
        FormCase fo;
        fo.shape = FormShape::Cylinder;
        fo.light = Compass::N;
        fo.rim = false;
        fo.specular = false;
        const Mask m = ell(29, 30);
        const Canvas c = formDab(ice, m, fo);
        const auto report = lint::checkLadder(c, ice);
        INFO(rungMap(c, ice) << findings(report));
        checkClean(report);
    }
    SECTION("Claude round 3, P3-3: a sunset L cylinder 27x14 lit from the south with rim light") {
        const Ramp sunset = *findRamp("sunset");
        for (const bool spec : {true, false}) {
            FormCase fo;
            fo.shape = FormShape::Cylinder;
            fo.light = Compass::S;
            fo.rim = true;
            fo.specular = spec;
            const Mask m = ell(27, 14);
            const Canvas c = formDab(sunset, m, fo);
            const auto report = lint::checkLadder(c, sunset);
            INFO("glint " << spec << "\n" << rungMap(c, sunset) << findings(report));
            checkClean(report);
        }
    }
    SECTION("Claude round 3, P2-2: a 40x20 L dome lit from the north-west uses every rung, bands no wider "
            "than four, the shadow black on the far side") {
        FormCase fo;
        fo.shape = FormShape::Dome;
        fo.light = Compass::NW;
        fo.rim = false;
        fo.specular = true;
        const Mask m = ell(40, 20);
        const Canvas c = formDab(r, m, fo);
        const auto report = lint::checkLadder(c, r);
        INFO(rungMap(c, r) << findings(report));
        checkClean(report);
        std::set<int> used;
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x)
                if (m.contains(x, y))
                    used.insert(*rungOf(r, c.at(x, y)));
        for (int g = 0; g <= r.topRung(); ++g)
            CHECK(used.count(g));
        CHECK(widestTurn(c, r, m, r.topRung() - kRungsPerStep) <= 4);
        // the shadow is black, and it reaches the edge facing away from the
        // light (south and east)
        bool farEdge = false;
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x)
                if (m.contains(x, y) && rungOf(r, c.at(x, y)) == 0 &&
                    (!m.contains(x + 1, y) || !m.contains(x, y + 1)))
                    farEdge = true;
        CHECK(farEdge);
    }
}

TEST_CASE("form brush repros from round 4", "[shading][form]") {
    SECTION("Codex round 4, finding 2: a lone selected cell gets no speck") {
        const Ramp fire = *findRamp("fire");
        Mask m = Mask::rect(22, 18, 2, 2, 17, 10);
        m.set(8, 14);
        FormCase fo;
        fo.shape = FormShape::Plane;
        fo.light = Compass::W;
        fo.rim = false;
        fo.specular = false;
        StrokeOptions o;
        o.selection = shared(m);
        o.ice = false;
        BrushSettings s;
        s.size = 44;
        s.tip = Tip::Square;
        const Canvas start = black(22, 18);
        Canvas c = start;
        s.lightDirection = fo.light;
        paintStroke(start, FormShadeBrush(fire, fo), s, {{11, 9}}, o).apply(c);
        const auto more = added(start, c, fire);
        INFO(rungMap(c, fire) << joined(more));
        CHECK(more.empty());
    }
    SECTION("Codex round 4, finding 3: a one-cell dab is judged as painted") {
        const Ramp fire = *findRamp("fire");
        FormCase fo;
        fo.shape = FormShape::Plane;
        fo.light = Compass::W;
        fo.rim = false;
        fo.specular = false;
        StrokeOptions o;
        o.selection = shared(Mask::rect(22, 18, 2, 2, 17, 10));
        o.ice = false;
        BrushSettings s;
        s.size = 1;
        s.tip = Tip::Square;
        const Canvas start = black(22, 18);
        Canvas c = start;
        s.lightDirection = fo.light;
        paintStroke(start, FormShadeBrush(fire, fo), s, {{8, 6}}, o).apply(c);
        const auto more = added(start, c, fire);
        INFO(rungMap(c, fire) << joined(more));
        CHECK(more.empty());
    }
    SECTION("Claude round 4, P2-1: a 36-across disc is a ball, not stripes, under the north and north-west light") {
        const Ramp r = steel();
        Mask m(42, 24);
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 42; ++x)
                if (std::pow((x - 20.5) / 18, 2) + std::pow((y - 11.5) / 9, 2) <= 1)
                    m.set(x, y);
        for (const auto light : {Compass::N, Compass::NW})
            for (const bool spec : {true, false}) {
                FormCase fo;
                fo.shape = FormShape::Dome;
                fo.light = light;
                fo.specular = spec;
                fo.rim = false;
                const Canvas c = formDab(r, m, fo);
                const auto report = lint::checkLadder(c, r);
                const auto d = formDetail(c, m);
                const auto [rows, flatRows] = stripes(c, r, m, true);
                const auto [cols, flatCols] = stripes(c, r, m, false);
                // with a glint the lit plane is the solid rung below the top
                const int plane = spec ? r.topRung() - kRungsPerStep : r.topRung();
                INFO("light " << int(light) << " glint " << spec << "\n"
                              << rungMap(c, r) << findings(report) << "detail " << d.mean << ", " << d.flatPercent
                              << "% flat; " << flatRows << " of " << rows << " rows one rung, " << flatCols << " of "
                              << cols << " columns");
                checkClean(report);
                // the bar is missed under the bounds (Claude round 5,
                // P2-A): the black no more than the shadow and ten points,
                // the lit plane no less than the light's lit share. With a
                // glint it is missed by more: since round 6 the glint is a
                // spot, not the rings that broke up the lit plane
                CHECK(d.mean >= (spec ? 2.3 : 2.5));
                CHECK(d.flatPercent < (spec ? 25 : 15));
                CHECK(widestTurn(c, r, m, plane) <= 4);
                CHECK(shadeGlyphs(c, m).size() >= 2);
                // both of the light's components: no stripes either way
                CHECK(flatRows * 4 < rows);
                CHECK(flatCols * 4 < cols);
                // the glint kept, the lit side the broad plane, the shadow
                // side down to black
                int top = 0, litAbove = 0, dark = 0, cells = 0;
                for (int y = 0; y < m.height(); ++y)
                    for (int x = 0; x < m.width(); ++x)
                        if (m.contains(x, y)) {
                            const int g = *rungOf(r, c.at(x, y));
                            ++cells;
                            top += g == r.topRung();
                            litAbove += g >= plane;
                            dark += g == 0;
                        }
                if (spec)
                    CHECK(top > 0);
                CHECK(dark > 0);
                // Claude round 5, P2-A: the lit side the broad plane, the
                // black no more than the light's shadow and ten points
                const FormLight shares = formLight(r, m, fo, light, plane);
                INFO("black " << double(dark) / cells << " shadow " << shares.shadow << "; lit "
                              << double(litAbove) / cells << " of at least " << shares.lit);
                CHECK(double(dark) / cells <= shares.shadow + 0.10 + 1e-9);
                CHECK(double(litAbove) / cells >= shares.lit - 1e-9);
                CHECK(shares.lit >= 0.03);
            }
    }
    SECTION("Claude round 4, P3-2: a 40x20 L dome lit from the north-west has no flat dark rung") {
        const Ramp r = steel();
        FormCase fo;
        fo.shape = FormShape::Dome;
        fo.light = Compass::NW;
        fo.rim = false;
        fo.specular = true;
        const Mask m = ell(40, 20);
        const Canvas c = formDab(r, m, fo);
        const auto report = lint::checkLadder(c, r);
        const auto d = formDetail(c, m);
        std::map<int, int> n;
        int cells = 0;
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x)
                if (m.contains(x, y)) {
                    ++cells;
                    ++n[*rungOf(r, c.at(x, y))];
                }
        // the dark rungs of the ladder, between black and the lit plane
        int darkMost = 0;
        const int plane = r.topRung() - kRungsPerStep;
        for (int g = 1; g < plane; ++g)
            darkMost = std::max(darkMost, n[g]);
        INFO(rungMap(c, r) << findings(report) << "detail " << d.mean << ", " << d.flatPercent << "% flat; "
                           << "widest dark rung " << darkMost << " of " << cells);
        checkClean(report);
        CHECK(darkMost * 100 < 15 * cells);
        CHECK(widestTurn(c, r, m, plane) <= 4);
        // The L's upper bar faces the light along its whole length, so its
        // lit plane is a flat run no try at the turn breaks up: the detail
        // bar is out of reach here (PLAN §10), and the brush lays the clean
        // try that comes nearest. It still beats round 3's 76% dark field;
        // under round 5's bounds on the black and the lit plane, and round
        // 6's glint a spot rather than rings, its detail is lower.
        CHECK(d.mean >= 2.1);
        CHECK(d.flatPercent < 40);
    }
}

namespace {

// The seeded harnesses below make each random draw in a statement or a
// declarator of its own, never two in one call's arguments: the order those
// are evaluated in is unspecified, so another compiler could build other
// strokes from the same seed and miss the pinned cells. Where a call took
// several draws, they are made last argument first, the order GCC made
// them in when the strokes, the pins and PLAN §10's figures were measured.

// What a soft stroke shades in the property tests: a flat field, a gradient
// running one of eight ways at one to three cells a rung, or a form brush
// dome over the canvas.
enum class Start { Flat, Gradient, Dome };

Canvas startCanvas(Start kind, const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int top = r.topRung();
    switch (kind) {
    case Start::Flat:
        return filled(w, h, rungCell(r, pick(1, top - 1)));
    case Start::Gradient: {
        constexpr int ways[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        const auto& way = ways[pick(0, 7)];
        const int every = pick(1, 3), from = pick(1, top / 2);
        return field(r, w, h, [&](int x, int y) {
            const int along = way[0] * (way[0] < 0 ? x - (w - 1) : x) + way[1] * (way[1] < 0 ? y - (h - 1) : y);
            return std::clamp(from + std::abs(along) / every, 1, top);
        });
    }
    case Start::Dome: {
        FormCase fo;
        fo.light = Compass(pick(0, 7));
        fo.specular = pick(0, 1);
        return toneCanvas(r, Mask(w, h, true), fo);
    }
    }
    return Canvas(w, h);
}

// Specks a size-1 despeckle dab leaves, over random fields of every named
// ramp: gradients and domes with 3-12 stray cells, near-noise (a gradient
// with a third of its cells moved up to two rungs) and uniform random
// rungs, 6-40 by 4-20, iCE on. Every speck the lint finds on a field gets
// its own dab on that field. A speck left is `held` when the only
// neighbour rungs that would clear it and add nothing are the far end of
// its neighbours that one of them alone holds (the #742 hole), `missed`
// when another neighbour rung would.
struct SpeckTally {
    int fields = 0, specks = 0, cleaned = 0, held = 0, missed = 0;
    std::string first;
};

SpeckTally speckCorpus(int fields, unsigned seed) {
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const auto& ramps = namedRamps();
    SpeckTally t;
    auto speckAt = [](const Canvas& c, const Ramp& r, int x, int y) {
        for (const auto& f : lint::checkLadder(c, r).findings)
            if (f.kind == Kind::Speckle && f.x == x && f.y == y)
                return true;
        return false;
    };
    for (int n = 0; n < fields; ++n) {
        const Ramp& r = ramps[size_t(pick(0, int(ramps.size()) - 1))];
        const int w = pick(6, 40), h = pick(4, 20), top = r.topRung(), kind = pick(0, 3);
        Canvas start = kind == 3 ? field(r, w, h, [&](int, int) { return pick(0, top); })
                                 : startCanvas(kind == 1 ? Start::Dome : Start::Gradient, r, w, h, rng);
        if (kind == 2) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (pick(0, 2) == 0)
                        if (const auto g = rungOf(r, start.at(x, y)))
                            start.at(x, y) = rungCell(r, std::clamp(*g + pick(-2, 2), 0, top));
        } else if (kind < 2) {
            for (int i = 0, k = pick(3, 12); i < k; ++i) {
                const int g = pick(0, top), y = pick(0, h - 1), x = pick(0, w - 1);
                start.at(x, y) = rungCell(r, g);
            }
        }
        ++t.fields;
        BrushSettings s;
        s.size = 1;
        for (const auto& f : lint::checkLadder(start, r).findings) {
            if (f.kind != Kind::Speckle)
                continue;
            ++t.specks;
            Canvas c = start;
            paintStroke(start, DespeckleBrush(r), s, {{f.x, f.y}}).apply(c);
            if (!speckAt(c, r, f.x, f.y)) {
                ++t.cleaned;
                continue;
            }
            const int g = *rungOf(r, start.at(f.x, f.y));
            std::map<int, int> votes;
            for (const auto& [dx, dy] : {std::pair{0, 1}, {0, -1}, {-1, 0}, {1, 0}})
                if (start.contains(f.x + dx, f.y + dy))
                    if (const auto v = rungOf(r, start.at(f.x + dx, f.y + dy)))
                        ++votes[*v];
            const bool dark = votes.begin()->first > g;
            const int far = dark ? votes.rbegin()->first : votes.begin()->first;
            bool clean = false, farClean = false;
            for (const auto& [v, count] : votes) {
                Canvas trial = start;
                trial.at(f.x, f.y) = rungCell(r, v);
                if (speckAt(trial, r, f.x, f.y) || !added(start, trial, r).empty())
                    continue;
                (v == far && count == 1 ? farClean : clean) = true;
            }
            t.held += farClean && !clean;
            if (clean && !t.missed++) {
                std::ostringstream out;
                out << r.name << " " << w << "x" << h << " speck " << g << " at (" << f.x << "," << f.y << ")\n"
                    << rungMap(start, r);
                t.first = out.str();
            }
        }
    }
    return t;
}

TEST_CASE("despeckle round 10: a speck is left only when no neighbour rung but the far end would clear it",
          "[shading][despeckle]") {
    const SpeckTally t = speckCorpus(200, 2026102310);
    INFO(t.fields << " fields, " << t.specks << " specks, " << t.cleaned << " cleaned, " << t.held
                  << " left as holes, " << t.missed << " left with a clean rung\n"
                  << t.first);
    CHECK(t.specks > 2000);
    CHECK(t.missed == 0);
    CHECK(t.cleaned * 2 > t.specks);
}

TEST_CASE("despeckle speck corpus counts", "[.measure]") {
    const SpeckTally t = speckCorpus(4000, 2026102311);
    WARN(t.fields << " fields, " << t.specks << " specks, " << t.cleaned << " cleaned, " << t.held
                  << " left as holes, " << t.missed << " left with a clean rung\n"
                  << t.first);
}

struct Tally {
    int strokes = 0, dabs = 0, painted = 0, failed = 0;
    std::string first;
};

// Random soft strokes on `kind` starts, each checked after every dab by
// CheckedSoft: every ramp, iCE on and off, both ways, three hardnesses,
// sizes 1-16 on both tips, pressure to nothing, strength and size, under no
// selection, a rectangle or a disc, some under the alpha lock with a fifth
// of the cells transparent.
Tally softStrokes(Start kind, int strokes, unsigned seed) {
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int w = 28, h = 14;
    Tally t;
    for (int k = 0; k < strokes; ++k) {
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        Canvas start = startCanvas(kind, r, w, h, rng);
        StrokeOptions o;
        o.ice = pick(0, 1);
        switch (pick(0, 2)) {
        case 1: {
            const int x0 = pick(0, w - 2), y0 = pick(0, h - 2);
            const int y1 = pick(y0 + 1, h - 1), x1 = pick(x0 + 1, w - 1);
            o.selection = shared(Mask::rect(w, h, x0, y0, x1, y1));
            break;
        }
        case 2: {
            const int across = pick(4, 30), cy = pick(0, h - 1), cx = pick(0, w - 1);
            o.selection = shared(disc(w, h, cx, cy, across));
            break;
        }
        default:
            break;
        }
        if (pick(0, 3) == 0) {
            o.lockTransparent = true;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    start.at(x, y).transparent = pick(0, 4) == 0;
        }
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        BrushSettings s;
        s.size = pick(1, 16);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        s.pressure = std::array{PressureTo::Nothing, PressureTo::Strength, PressureTo::Size}[size_t(pick(0, 2))];
        const SoftShadeBrush brush(r, dir, std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))]);
        std::vector<Dab> dabs;
        for (int i = 0, n = pick(1, 4); i < n; ++i)
            dabs.push_back({pick(-2, w + 1), pick(-2, h + 1), 0.05 * pick(1, 20)});
        CheckedSoft checked(brush, start, dir);
        t.painted += !paintStroke(start, checked, s, dabs, o).changes.empty();
        ++t.strokes;
        t.dabs += checked.dabs;
        if (checked.failure.empty())
            continue;
        if (!t.failed++) {
            std::ostringstream out;
            out << "stroke " << k << ": " << r.name << " ice " << o.ice << " dir " << int(dir) << " hardness "
                << brush.hardness() << " size " << s.size << " tip " << int(s.tip) << " strength " << s.strength
                << " pressure " << int(s.pressure) << " lock " << o.lockTransparent << " selection "
                << bool(o.selection) << "\n";
            for (const Dab& d : dabs)
                out << "(" << d.x << "," << d.y << "," << d.pressure << ") ";
            out << "\nstart\n" << rungMap(start, r) << checked.failure;
            t.first = out.str();
        }
    }
    return t;
}

} // namespace

TEST_CASE("soft strokes add no lint finding, dab by dab: flat fields", "[shading][soft]") {
    // Codex round 3, findings 1 and 2; Claude round 3, P3-1 and P3-2.
    const Tally t = softStrokes(Start::Flat, 1000, 2026102301);
    INFO(t.failed << " of " << t.strokes << " strokes (" << t.dabs << " dabs, " << t.painted << " painted) failed\n"
                   << t.first);
    CHECK(t.failed == 0);
    CHECK(t.painted * 2 > t.strokes);
}

TEST_CASE("soft strokes add no lint finding, dab by dab: gradients", "[shading][soft]") {
    const Tally t = softStrokes(Start::Gradient, 1000, 2026102302);
    INFO(t.failed << " of " << t.strokes << " strokes (" << t.dabs << " dabs, " << t.painted << " painted) failed\n"
                   << t.first);
    CHECK(t.failed == 0);
    // a gradient a cell a rung has no room for a change: any is a skip
    CHECK(t.painted * 5 > t.strokes * 2);
}

TEST_CASE("soft strokes add no lint finding, dab by dab: a form dome", "[shading][soft]") {
    const Tally t = softStrokes(Start::Dome, 1000, 2026102303);
    INFO(t.failed << " of " << t.strokes << " strokes (" << t.dabs << " dabs, " << t.painted << " painted) failed\n"
                   << t.first);
    CHECK(t.failed == 0);
    // the dome's shadow is black and its lit plane the top rung: a dab
    // darkening the one or lightening the other has nothing to move
    CHECK(t.painted * 3 > t.strokes);
}

// Claude P3-2, generalised: lint-clean may win over painting, but on a flat
// field of a shade a dab that has two 4-adjacent paintable cells, and a
// next rung to move them to, always finds a clean change. (A field of a
// solid rung has no shade: a change that shades it with one glyph is the
// lint's one-glyph finding, which the brush refuses.)
TEST_CASE("soft dab on a flat field paints when its footprint can take a clean change", "[shading][soft]") {
    std::mt19937 rng(77);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int w = 28, h = 14;
    int can = 0, blank = 0;
    std::string first;
    for (int k = 0; k < 3000; ++k) {
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        int g = pick(1, r.topRung() - 1);
        if (g % kRungsPerStep == 0)
            g += pick(0, 1) ? 1 : -1;
        Canvas start = filled(w, h, rungCell(r, g));
        StrokeOptions o;
        o.ice = pick(0, 1);
        switch (pick(0, 2)) {
        case 1: { const int x0 = pick(0, w - 2), y0 = pick(0, h - 2);
            const int y1 = pick(y0 + 1, h - 1), x1 = pick(x0 + 1, w - 1);
            o.selection = shared(Mask::rect(w, h, x0, y0, x1, y1)); break; }
        case 2: { const int across = pick(4, 30), cy = pick(0, h - 1), cx = pick(0, w - 1);
            o.selection = shared(disc(w, h, cx, cy, across)); break; }
        default: break;
        }
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        BrushSettings s; s.size = pick(1, 16); s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        const Dab d{pick(-2, w + 1), pick(-2, h + 1)};
        const SoftShadeBrush brush(r, dir, hard);
        Stroke probe(start, brush, s, o);
        std::set<std::pair<int,int>> fp;
        for (const auto& [x, y] : footprint(d.x, d.y, s.size, s.tip, w, h, false)) if (probe.paintable(x, y)) fp.insert({x, y});
        bool pair = false;
        for (const auto& [x, y] : fp) pair = pair || fp.count({x + 1, y}) || fp.count({x, y + 1});
        const int g0 = *rungOf(r, start.at(0, 0)) + (dir == ShadeDirection::Lighten ? 1 : -1);
        if (!o.ice && (rungCell(r, g0).bg.paletteIndex() >= 8 || start.at(0, 0).bg.paletteIndex() >= 8)) continue;
        if (!pair) continue;
        ++can;
        const auto e = paintStroke(start, brush, s, {d}, o);
        if (e.changes.empty() && !blank++) {
            std::ostringstream out; out << r.name << " rung " << *rungOf(r, start.at(0, 0)) << " ice " << o.ice << " dir " << int(dir) << " hard " << hard << " size " << s.size << " tip " << int(s.tip) << " str " << s.strength << " at " << d.x << "," << d.y << "\n";
            for (int y = 0; y < h; ++y) { for (int x = 0; x < w; ++x) out << (fp.count({x, y}) ? '#' : probe.paintable(x, y) ? '.' : ' '); out << "\n"; }
            first = out.str();
        }
    }
    INFO(blank << " of " << can << " single dabs with a paintable pair painted nothing\n" << first);
    CHECK(can > 1000);
    CHECK(blank == 0);
}

// Claude round 3, P2-1: a dab re-shades the cells near it, not the stroke so
// far. A long stroke of size-41 dabs back and forth along a gradient: the
// cells a stamp's solve visits stay within a constant times its window (the
// dab's size plus a fixed margin, read a few dozen times while it gives
// way) and the same on a canvas ten times taller. Only the 47 stamps of the
// first pass solve; a dab back over cells that already want its depth is
// skipped and reads nothing, so the bound is on the most any stamp reads,
// late in the stroke as early (re-shading the whole stroke reads 287
// windows at most, and 9.5 times as much on the taller canvas). The lint's
// check of a dab isn't counted:
// it reads the rows and columns through the cells that change to their
// ends (PLAN §10 times it). The start's findings the brush reads are (Codex
// round 6, finding 2): a column of them reads down the canvas, so the
// taller canvas costs a little more. Round 4 painted with a brush that was
// gone before the first dab. Round 7's guard reads one glyph, noise and wide
// rungs that drift along their line, so a dab here gives way more rungs, a
// pass over the window each: 159 windows a stamp at most (round 6 read
// under 100, and 113 once the lint's rule for a change came in from main),
// the same on both heights. Round 8 (P3-A) solves a dab that lays nothing
// once more with the earlier wants that laid nothing set aside, so a stamp
// may take two solves. Round 8 read 212 windows, but it solved nearly every
// dab twice (Codex round 8, P2); since round 9 only the 51 stamps a height
// that lay nothing are, and the most is 134 windows. The bound is about
// 1.2 times that.
TEST_CASE("soft brush solve per dab is bounded by the dab, not the stroke or canvas", "[shading][soft]") {
    const Ramp r = steel();
    const int size = 41;
    std::array<long long, 2> worst{};
    for (const int H : {100, 1000}) {
        Canvas c(160, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < 160; ++x)
                c.at(x, y) = rungCell(r, std::min(12, 1 + x * 12 / 160));
        BrushSettings s;
        s.size = size;
        s.tip = Tip::Round;
        s.strength = 1.0;
        const SoftShadeBrush brush(r, ShadeDirection::Lighten, 0.0);
        Stroke st(c, brush, s);
        std::vector<double> perStamp;
        int x = 10, dx = 3;
        const int y = 50;
        for (int i = 0; i < 200; ++i) {
            const long long v0 = SoftShadeBrush::cellsVisited();
            const long long s0 = st.stamps();
            st.addDab({x, y});
            const long long stamps = st.stamps() - s0;
            if (stamps > 0)
                perStamp.push_back(double(SoftShadeBrush::cellsVisited() - v0) / double(stamps));
            x += dx;
            if (x > 150 || x < 10) {
                dx = -dx;
                x += 2 * dx;
            }
        }
        REQUIRE(perStamp.size() >= 150);
        const double most = *std::max_element(perStamp.begin(), perStamp.end());
        INFO("160x" << H << ": most " << most << " cells a stamp");
        // the window: the dab and 9 + 4 + 13 cells each side
        const double window = double(size + 2 * 26) * (size + 2 * 26);
        CHECK(most < 170.0 * window);
        worst[H == 1000] = (long long)most;
    }
    INFO("most cells a stamp: " << worst[0] << " on 160x100, " << worst[1] << " on 160x1000");
    CHECK(worst[1] * 100 <= worst[0] * 102);
}

// Claude round 3, P2-1: re-shading only near the new dab gives the stroke
// re-shading all of it would. Random strokes over flat fields, gradients
// and a dome, with selections and the alpha lock, painted both ways, over
// 400 strokes a seed. Rarely a cell at the edge of the cells the near solve
// settles moves where the whole solve, trying its neighbours further out
// too, holds it back, or the other way: 2 of 2,400 strokes over seeds
// 2026102304-09 (Claude #747). Both add nothing. They are pinned by stroke,
// cell and rung; any other difference fails.
struct NearWholeCell {
    int stroke, x, y, near, whole;
    friend bool operator==(const NearWholeCell&, const NearWholeCell&) = default;
};

struct NearWholeRun {
    std::vector<NearWholeCell> cells;
    int painted = 0;
    std::string first;
};

// The seed's 400 strokes, each painted near and whole when `only` is empty
// or holds it.
NearWholeRun nearWhole(unsigned seed, const std::set<int>& only = {}) {
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int w = 40, h = 24;
    NearWholeRun run;
    for (int k = 0; k < 400; ++k) {
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        Canvas start = startCanvas(Start(pick(0, 2)), r, w, h, rng);
        StrokeOptions o;
        o.ice = pick(0, 1);
        if (pick(0, 2) == 0) {
            const int across = pick(4, 30), cy = pick(0, h - 1), cx = pick(0, w - 1);
            o.selection = shared(disc(w, h, cx, cy, across));
        }
        if (pick(0, 3) == 0) {
            o.lockTransparent = true;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    start.at(x, y).transparent = pick(0, 4) == 0;
        }
        const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
        const double hard = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
        BrushSettings s;
        s.size = pick(1, 21);
        s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
        s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
        std::vector<Dab> dabs;
        for (int i = 0, n = pick(1, 5); i < n; ++i)
            dabs.push_back({pick(-2, w + 1), pick(-2, h + 1)});
        if (!only.empty() && !only.count(k))
            continue;
        SoftShadeBrush near(r, dir, hard), whole(r, dir, hard);
        whole.reshadeWhole(true);
        Canvas a = start, b = start;
        paintStroke(start, near, s, dabs, o).apply(a);
        paintStroke(start, whole, s, dabs, o).apply(b);
        run.painted += !(a == start);
        if (a == b)
            continue;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (!(a.at(x, y) == b.at(x, y)))
                    run.cells.push_back(
                        {k, x, y, rungOf(r, a.at(x, y)).value_or(-1), rungOf(r, b.at(x, y)).value_or(-1)});
        if (run.first.empty()) {
            std::ostringstream out;
            out << "stroke " << k << ": " << r.name << " ice " << o.ice << " dir " << int(dir) << " hardness " << hard
                << " size " << s.size << " tip " << int(s.tip) << " strength " << s.strength << "\n";
            for (const Dab& d : dabs)
                out << "(" << d.x << "," << d.y << ") ";
            out << "\nnear\n" << rungMap(a, r) << "whole\n" << rungMap(b, r);
            run.first = out.str();
        }
    }
    return run;
}

std::string cellList(const std::vector<NearWholeCell>& cells) {
    std::ostringstream out;
    for (const auto& c : cells)
        out << "stroke " << c.stroke << " (" << c.x << "," << c.y << ") near " << c.near << " whole " << c.whole
            << "\n";
    return out.str();
}

// Seed 2026102304 stroke 297: sunset, iCE on, Darken, hardness 1, a size-3
// square at 0.75. The cell starts at 20; near takes it the dab's three
// rungs, whole two. Seed 2026102309 stroke 205: steel, iCE on, Lighten,
// hardness 0, a size-7 square at 0.75 under the alpha lock.
const std::vector<NearWholeCell> kNearWhole304{{297, 23, 18, 17, 18}};
const std::vector<NearWholeCell> kNearWhole309{
    {205, 10, 6, 2, 3}, {205, 11, 6, 2, 3}, {205, 19, 8, 3, 2}, {205, 20, 8, 4, 3}};

TEST_CASE("soft brush: re-shading near the dab equals re-shading the whole stroke", "[shading][soft]") {
    const NearWholeRun run = nearWhole(2026102304);
    INFO(cellList(run.cells) << run.painted << " of 400 painted\n" << run.first);
    CHECK(run.cells == kNearWhole304);
    // the seed's strokes themselves: a compiler that drew them in another
    // order would paint another count, the pins above aside
    CHECK(run.painted == 239);
    const NearWholeRun other = nearWhole(2026102309, {205});
    INFO(cellList(other.cells));
    CHECK(other.cells == kNearWhole309);
}

TEST_CASE("soft brush: near and whole over six seeds", "[.measure]") {
    std::string all;
    int strokes = 0;
    for (unsigned seed = 2026102304; seed <= 2026102309; ++seed) {
        const NearWholeRun run = nearWhole(seed);
        std::set<int> differ;
        for (const auto& c : run.cells)
            differ.insert(c.stroke);
        strokes += int(differ.size());
        all += "seed " + std::to_string(seed) + ": " + std::to_string(differ.size()) + " of 400\n" +
               cellList(run.cells);
        if (seed == 2026102304)
            CHECK(run.cells == kNearWhole304);
        else if (seed == 2026102309)
            CHECK(run.cells == kNearWhole309);
        else
            CHECK(run.cells.empty());
    }
    WARN(strokes << " of 2400 strokes differ\n" << all);
}

namespace {

// The lightest rung a form brush stroke uses with iCE off: the last one up
// from the darkest without a bright background (FormShadeBrush::highest).
int iceOffHighest(const Ramp& r) {
    int g = r.colours[0] == Black ? 1 : 0;
    while (g < r.topRung() && rungCell(r, g + 1).bg.paletteIndex() < 8)
        ++g;
    return g;
}

struct FormTally {
    int forms = 0, windowed = 0, bar = 0, failed = 0, unclean = 0, partial = 0;
    // domes, and of them those of 900 cells or more: how many get a form
    // at all, and how many clear the bar
    int domes[2] = {}, domesLaid[2] = {}, domesBar[2] = {};
    std::vector<double> means, flats;  // of the forms with windows
    std::string first;
};

// `count` seeded forms of every shape kind (rect, ellipse, L, holed) from
// 3x3 to 60x40, as a dome, a cylinder or a plane, under the eight lights,
// with and without rim light, glint and iCE, on the five ramps, each laid
// by the brush over its whole selected shape. Every one is clean by the
// ladder lint, and a dab over part of the selection adds no finding. The
// bar is read on each and tallied, not required: PLAN §10 says
// which forms it is out of reach for.
std::map<std::string, FormTally> formSweep(int count, unsigned seed) {
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    std::map<std::string, FormTally> out;
    for (int k = 0; k < count; ++k) {
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        const int sw = pick(3, 60), sh = pick(3, 40);
        const char* kinds[] = {"rect", "ellipse", "L", "holed"};
        const std::string kind = kinds[pick(0, 3)];
        Mask m(sw + 4, sh + 4);
        if (kind == "rect") {
            m = Mask::rect(sw + 4, sh + 4, 2, 2, sw + 1, sh + 1);
        } else if (kind == "ellipse") {
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x) {
                    const double dx = (x + 0.5) / sw - 0.5, dy = (y + 0.5) / sh - 0.5;
                    if (dx * dx + dy * dy <= 0.25)
                        m.set(x + 2, y + 2);
                }
        } else if (kind == "L") {
            m = ell(sw, sh);
        } else {
            m = Mask::rect(sw + 4, sh + 4, 2, 2, sw + 1, sh + 1);
            const int hw = std::max(1, sw / 3), hh = std::max(1, sh / 3);
            const int hx = 2 + pick(1, std::max(1, sw - hw - 1)), hy = 2 + pick(1, std::max(1, sh - hh - 1));
            for (int y = hy; y < std::min(hy + hh, sh + 1); ++y)
                for (int x = hx; x < std::min(hx + hw, sw + 1); ++x)
                    m.set(x, y, false);
        }
        FormCase fo;
        fo.shape = FormShape(pick(0, 2));
        fo.light = Compass(pick(0, 7));
        fo.rim = pick(0, 1);
        fo.specular = pick(0, 1);
        const bool ice = pick(0, 1);
        const int high = ice ? r.topRung() : iceOffHighest(r);
        const Canvas c = formDab(r, m, fo, ice);
        FormTally& t = out[kind];
        ++t.forms;
        std::string why;
        const auto keys = findingKeys(c, r);
        if (!keys.empty()) {
            why = std::get<3>(*keys.begin());
            ++t.unclean;
        }
        // the bar, read independently: the detail over the
        // form's box, the widest rung of its turn and its shade glyphs
        const int plane = fo.specular && fo.shape != FormShape::Plane && high > kRungsPerStep
                              ? (high - 1) / kRungsPerStep * kRungsPerStep
                              : high;
        const auto d = formDetail(c, m);
        const bool bar = keys.empty() && widestTurn(c, r, m, plane) <= 4 && shadeGlyphs(c, m).size() >= 2 &&
                         (d.windows == 0 || (d.mean >= 3.6 && d.flatPercent < 15));
        t.windowed += d.windows > 0;
        t.bar += bar;
        if (d.windows > 0) {
            t.means.push_back(d.mean);
            t.flats.push_back(d.flatPercent);
        }
        if (fo.shape == FormShape::Dome) {
            int cells = 0;
            bool laid = false;
            for (int y = 0; y < m.height(); ++y)
                for (int x = 0; x < m.width(); ++x)
                    if (m.contains(x, y)) {
                        ++cells;
                        laid = laid || c.at(x, y) != black(1, 1).at(0, 0);
                    }
            const int big = cells >= 900;
            ++t.domes[big];
            t.domesLaid[big] += laid;
            t.domesBar[big] += bar;
        }
        // a dab over part of the selection paints only what keeps the
        // canvas as clean as it was
        {
            StrokeOptions o;
            o.selection = shared(m);
            o.ice = ice;
            BrushSettings s;
            s.size = pick(1, std::max(sw, sh));
            s.tip = Tip(pick(0, 1));
            const Canvas start = black(m.width(), m.height());
            Canvas part = start;
            s.lightDirection = fo.light;
            paintStroke(start, FormShadeBrush(r, fo), s, {{pick(0, m.width() - 1), pick(0, m.height() - 1)}}, o)
                .apply(part);
            if (const auto more = added(start, part, r); why.empty() && !more.empty()) {
                why = "a part dab adds " + more.front();
                ++t.partial;
            }
        }
        if (why.empty())
            continue;
        if (!t.failed++) {
            std::ostringstream o;
            o << "form " << k << ": " << kind << " " << sw << "x" << sh << " " << r.name << " shape "
              << int(fo.shape) << " light " << int(fo.light) << " rim " << fo.rim << " glint " << fo.specular
              << " ice " << ice << ": " << why << "\n"
              << rungMap(c, r);
            t.first = o.str();
        }
    }
    return out;
}

std::string quartiles(std::vector<double> v) {
    if (v.empty())
        return "-";
    std::sort(v.begin(), v.end());
    std::ostringstream o;
    for (const double q : {0.1, 0.25, 0.5, 0.75, 0.9})
        o << (q > 0.1 ? "/" : "") << v[size_t(q * (v.size() - 1))];
    return o.str();
}

std::string formReport(const std::map<std::string, FormTally>& tally) {
    std::ostringstream o;
    for (const auto& [kind, t] : tally)
        o << kind << ": " << t.forms << " forms, " << t.failed << " failed (" << t.unclean << " unclean, "
          << t.partial << " part dabs adding a finding); " << t.windowed << " with windows, " << t.bar
          << " clear the bar; detail p10/25/50/75/90 " << quartiles(t.means) << ", flat % " << quartiles(t.flats)
          << "; domes under 900 cells " << t.domesLaid[0] << " of " << t.domes[0] << " laid, " << t.domesBar[0]
          << " clear the bar; 900 or more " << t.domesLaid[1] << " of " << t.domes[1] << " laid, " << t.domesBar[1]
          << " clear the bar\n";
    for (const auto& [kind, t] : tally)
        o << t.first;
    return o.str();
}

void checkForms(int count, unsigned seed) {
    const auto tally = formSweep(count, seed);
    int forms = 0, failed = 0;
    for (const auto& [kind, t] : tally) {
        forms += t.forms;
        failed += t.failed;
    }
    INFO(formReport(tally));
    CHECK(forms == count);
    CHECK(failed == 0);
}

} // namespace

// Codex round 3, findings 3 and 4; Claude round 3, P2-2 and P3-3; Codex
// round 4, findings 2 and 3: the form brush never lays a form the lint
// fails, whole or in part.
TEST_CASE("form brush property: 1000 forms seed 1", "[shading][form]") {
    checkForms(1000, 2026102311);
}

TEST_CASE("form brush property: 1000 forms seed 2", "[shading][form]") {
    checkForms(1000, 2026102312);
}

// The numbers behind PLAN §10's form bullet: run with "[.survey]".
TEST_CASE("form brush survey", "[.survey]") {
    for (const unsigned seed : {2026102311u, 2026102312u})
        WARN(formReport(formSweep(1000, seed)));
}

namespace {

// A stroke under mirror X against its images painted one at a time: the
// stroke alone, then the stroke of its mirrored dabs (flipped, so an even
// tip lands as the image would), each on the canvas as the stroke began.
// The stroke keeps to the left of the canvas so the two never meet.
void checkMirrorsAlone(const Canvas& start, const Brush& brush, const BrushSettings& s, const std::vector<Dab>& dabs) {
    const int w = start.width();
    std::vector<Dab> images;
    for (const Dab& d : dabs)
        images.push_back({w - 1 - d.x, d.y, d.pressure, 0, true});
    StrokeOptions o;
    o.symmetry = {SymmetryMode::MirrorX};
    Canvas both = start;
    paintStroke(start, brush, s, dabs, o).apply(both);
    const CellEdit alone = paintStroke(start, brush, s, dabs);
    const CellEdit image = paintStroke(start, brush, s, images);
    REQUIRE_FALSE(alone.empty());
    REQUIRE_FALSE(image.empty());
    for (const auto& a : alone.changes)
        REQUIRE(a.x < w / 2);
    for (const auto& b : image.changes)
        REQUIRE(b.x >= w / 2);
    Canvas apart = start;
    alone.apply(apart);
    image.apply(apart);
    INFO("symmetric\n" << rungMap(both, steel()) << "one at a time\n" << rungMap(apart, steel()));
    CHECK(both == apart);
}

} // namespace

TEST_CASE("form brush: a symmetric stroke shades each image as its own form", "[shading][form][symmetry]") {
    const Ramp r = steel();
    FormShadeBrush brush(r);
    BrushSettings s;
    s.size = 4;  // even, so the image's tip is the flipped one
    s.tip = Tip::Square;
    // a light that isn't symmetric about the mirror: the image is lit as
    // the stroke is, not as its mirror
    s.lightDirection = Compass::NE;
    checkMirrorsAlone(black(160, 20), brush, s, {{12, 8}, {30, 10}, {44, 12}});
}

TEST_CASE("soft brush: a symmetric stroke bands as its images one at a time", "[shading][soft][symmetry]") {
    const Ramp r = steel();
    SoftShadeBrush brush(r, ShadeDirection::Lighten);
    BrushSettings s;
    s.size = 10;
    s.tip = Tip::Round;
    checkMirrorsAlone(filled(160, 20, rungCell(r, 4)), brush, s, {{14, 9}, {40, 11}});
}

TEST_CASE("despeckle brush: a symmetric stroke cleans its images one at a time", "[shading][despeckle][symmetry]") {
    const Ramp r = steel();
    Canvas c = filled(160, 14, rungCell(r, 8));
    // specks on both sides, not mirror images of each other
    for (const auto& [x, y, g] : std::vector<std::tuple<int, int, int>>{
             {6, 5, 4}, {15, 7, 11}, {22, 3, 6}, {150, 6, 10}, {141, 4, 5}, {133, 8, 2}})
        c.at(x, y) = rungCell(r, g);
    DespeckleBrush brush(r);
    BrushSettings s;
    s.size = 6;
    s.tip = Tip::Square;
    checkMirrorsAlone(c, brush, s, {{4, 5}, {28, 5}});
}

// Codex round 5, finding 3: under a mirror a speck and its image are
// judged together, from the same cells, so a cluster on a symmetric canvas
// clears as its mirror does.
TEST_CASE("despeckle brush: Codex round 5, finding 3: a mirror stroke on a symmetric canvas stays symmetric",
          "[shading][despeckle][symmetry]") {
    const Ramp r = steel();
    const int rows[3][8] = {{0, 1, 9, 0, 0, 9, 1, 0}, {1, 9, 1, 9, 9, 1, 9, 1}, {0, 1, 9, 0, 0, 9, 1, 0}};
    Canvas c(8, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 8; ++x)
            c.at(x, y) = rungCell(r, rows[y][x]);
    const DespeckleBrush brush(r);
    BrushSettings s;
    s.size = 2;
    s.tip = Tip::Square;
    StrokeOptions o;
    o.symmetry = {SymmetryMode::MirrorX};
    Canvas out = c;
    paintStroke(c, brush, s, {{2, 2}}, o).apply(out);
    INFO(rungMap(out, r));
    CHECK(out.at(1, 1) == out.at(6, 1));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 8; ++x)
            CHECK(out.at(x, y) == out.at(7 - x, y));
}

TEST_CASE("despeckle brush: mirror strokes on symmetric canvases stay symmetric", "[shading][despeckle][symmetry]") {
    const Ramp r = steel();
    std::mt19937 rng(2026102503);
    auto pick = [&](int a, int b) { return bwtest::uniformInt(rng, a, b); };
    const SymmetryMode modes[3] = {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY};
    int failed = 0, cleaned = 0;
    std::string first;
    for (int i = 0; i < 400; ++i) {
        const SymmetryMode mode = modes[i % 3];
        // odd and even sizes: the axis on a cell and between two
        const int w = pick(5, 24), h = pick(3, 14);
        const bool mx = mode != SymmetryMode::MirrorY, my = mode != SymmetryMode::MirrorX;
        std::vector<int> g(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                g[size_t(y) * w + x] = pick(0, 2) ? 4 + (x + y) / 4 % 5 : pick(0, r.topRung());
        Canvas start(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int sx = mx ? std::min(x, w - 1 - x) : x, sy = my ? std::min(y, h - 1 - y) : y;
                start.at(x, y) = rungCell(r, g[size_t(sy) * w + sx]);
            }
        const DespeckleBrush brush(r);
        BrushSettings s;
        s.size = pick(1, 6);
        s.tip = pick(0, 1) ? Tip::Square : Tip::Round;
        StrokeOptions o;
        o.symmetry = {mode};
        std::vector<Dab> dabs;
        for (int k = pick(1, 4); k > 0; --k)
            dabs.push_back({pick(-1, w), pick(-1, h)});
        Stroke st(start, brush, s, o);
        const bool join = pick(0, 1);
        bool broke = false;
        for (const Dab& d : dabs) {
            st.addDab(d, join);
            const Canvas& out = st.preview();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    broke = broke || !(out.at(x, y) == out.at(mx ? w - 1 - x : x, my ? h - 1 - y : y));
        }
        cleaned += !(st.preview() == start);
        if (broke && failed++ == 0) {
            std::ostringstream m;
            m << "stroke " << i << ": mode " << int(mode) << " " << w << "x" << h << " size " << s.size << " tip "
              << int(s.tip) << " join " << join << "\n";
            for (const Dab& d : dabs)
                m << "(" << d.x << "," << d.y << ") ";
            m << "\nstart\n" << rungMap(start, r) << "after\n" << rungMap(st.preview(), r);
            first = m.str();
        }
    }
    INFO(failed << " of 400 strokes broke symmetry; " << cleaned << " changed\n" << first);
    CHECK(failed == 0);
    CHECK(cleaned > 150);
}

TEST_CASE("shading brushes: random strokes add nothing to the start, checked after every dab",
          "[shading][soft][form][despeckle][symmetry]") {
    // Round 7: the soft, form and despeckle brushes each keep one
    // StrokeLint over the stroke. 80 random strokes a brush (every ramp, iCE
    // on and off, no symmetry or a mirror, joined walks and new segments,
    // sizes 2-12 on both tips) on a flat field, a gradient or a form dome
    // with a few speckled cells, and after every dab the whole canvas is
    // read against the stroke's start by the lint's own rule: nothing added,
    // the one glyph and noise readings included.
    std::mt19937 rng(2026102307);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int w = 36, h = 18;
    const SymmetryMode modes[4] = {SymmetryMode::None, SymmetryMode::MirrorX, SymmetryMode::MirrorY,
                                   SymmetryMode::MirrorXY};
    const char* names[3] = {"soft", "form", "despeckle"};
    for (int which = 0; which < 3; ++which) {
        int strokes = 0, dabs = 0, painted = 0, failed = 0, mirrored = 0, iced = 0;
        std::string first;
        for (int k = 0; k < 80; ++k) {
            const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
            Canvas start = startCanvas(Start(pick(0, 2)), r, w, h, rng);
            for (int i = 0, n = pick(0, 12); i < n; ++i) {
                const int g = pick(0, r.topRung()), y = pick(0, h - 1), x = pick(0, w - 1);
                start.at(x, y) = rungCell(r, g);
            }
            StrokeOptions o;
            o.ice = pick(0, 1);
            o.symmetry = {modes[pick(0, 3)]};
            mirrored += o.symmetry.mode != SymmetryMode::None;
            iced += o.ice;
            BrushSettings s;
            s.size = pick(2, 12);
            s.tip = pick(0, 1) ? Tip::Round : Tip::Square;
            s.strength = std::array{0.25, 0.5, 0.75, 1.0}[size_t(pick(0, 3))];
            std::unique_ptr<Brush> brush;
            if (which == 0) {
                const double hardness = std::array{0.0, 0.5, 1.0}[size_t(pick(0, 2))];
                const auto dir = pick(0, 1) ? ShadeDirection::Lighten : ShadeDirection::Darken;
                brush = std::make_unique<SoftShadeBrush>(r, dir, hardness);
            } else if (which == 1) {
                FormOptions fo;
                fo.shape = FormShape(pick(0, 2));
                fo.rim = pick(0, 1);
                fo.specular = pick(0, 1);
                brush = std::make_unique<FormShadeBrush>(r, fo);
            } else {
                brush = std::make_unique<DespeckleBrush>(r);
            }
            Stroke st(start, *brush, s, o);
            int x = pick(0, w - 1), y = pick(0, h - 1);
            std::ostringstream trail;
            std::string failure;
            for (int i = 0, n = pick(1, 8); i < n && failure.empty(); ++i) {
                const bool join = i == 0 || pick(0, 4) != 0;
                if (!join) {
                    x = pick(0, w - 1);
                    y = pick(0, h - 1);
                }
                st.addDab({x, y}, join);
                ++dabs;
                trail << "(" << x << "," << y << (join ? "" : " new") << ") ";
                const auto found = added(start, st.preview(), r);
                if (!found.empty())
                    failure = "dab " + std::to_string(i) + " added:\n" + joined(found) + rungMap(st.preview(), r);
                x = std::clamp(x + pick(-4, 4), 0, w - 1);
                y = std::clamp(y + pick(-3, 3), 0, h - 1);
            }
            ++strokes;
            painted += !(st.preview() == start);
            if (failure.empty())
                continue;
            if (!failed++) {
                std::ostringstream out;
                out << "stroke " << k << ": " << r.name << " ice " << o.ice << " symmetry "
                    << int(o.symmetry.mode) << " size " << s.size << " tip " << int(s.tip) << " strength "
                    << s.strength << "\n"
                    << trail.str() << "\nstart\n"
                    << rungMap(start, r) << failure;
                first = out.str();
            }
        }
        INFO(names[which] << ": " << failed << " of " << strokes << " strokes (" << dabs << " dabs, " << painted
                          << " painted, " << mirrored << " mirrored, " << iced << " iCE) failed\n"
                          << first);
        CHECK(failed == 0);
        CHECK(strokes == 80);
        CHECK(mirrored > 40);
        CHECK(iced > 20);
        CHECK(painted * 4 > strokes);
    }
}

TEST_CASE("despeckle leaves the gradient's dither alone and clears a speck by hand in it", "[shading][despeckle][dither]") {
    const Ramp r = steel();
    const int w = 60, h = 8;
    const Canvas blank(w, h);
    Gradient g;
    g.x1 = w - 1, g.toRung = r.topRung(), g.banding = GradientBanding::Bayer;
    Canvas dithered = blank;
    rampGradient(blank, r, g, StrokeOptions{}).apply(dithered);
    int marks = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            marks += dithered.at(x, y).dithered;
    REQUIRE(marks > 20);
    REQUIRE(rungOf(r, dithered.at(38, 1)) == 8);
    Canvas c = dithered;
    c.at(38, 1) = rungCell(r, 9);  // a speck by hand

    BrushSettings s;
    s.size = 3;
    std::vector<Dab> pass;  // over every cell
    for (int y = 1; y < h; y += 3)
        for (int x = 1; x < w; x += 2)
            pass.push_back({x, y});
    paintStroke(c, DespeckleBrush(r), s, pass).apply(c);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            INFO(x << "," << y);
            CHECK(identical(c.at(x, y), dithered.at(x, y)));
        }
    CHECK(lint::checkLadder(c, r).clean());

    // the same cells unmarked are specks to it like any other
    Canvas bare = dithered;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            bare.at(x, y).dithered = false;
    CHECK_FALSE(paintStroke(bare, DespeckleBrush(r), s, pass).empty());
}
