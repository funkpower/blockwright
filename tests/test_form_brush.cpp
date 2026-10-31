// The form brush after round 5 (PLAN §10, "Form"): black no more than the
// form's shadow, the lit plane no less than the light gives it, a replayed
// stroke painting what it did once, the glint kept to the lit side, a
// cylinder lit along its axis still turning, and the bar read at every
// window offset; and after round 8, a stamp's work bounded by its size and
// a dab that can't lay anything keeping what the stroke has laid.

#include "blockwright/ansi.h"
#include "portable_random.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/guard.h"
#include "blockwright/lint.h"
#include "blockwright/palette.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace bw;

namespace {

Ramp steel() { return *findRamp("steel"); }  // k K w W

const std::array<const char*, 5> kRamps{"steel", "fire", "ice", "toxic", "sunset"};

Canvas black(int w, int h) {
    Canvas c(w, h);
    Cell k;
    k.ch = U' ';
    k.fg = Color::indexed(Black);
    k.bg = Color::indexed(Black);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c.at(x, y) = k;
    return c;
}

// A canvas of rungs, a row a string: 0-9 then a-z.
Canvas rungs(const Ramp& r, const std::vector<std::string>& rows) {
    Canvas c(int(rows[0].size()), int(rows.size()));
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const char ch = rows[size_t(y)][size_t(x)];
            c.at(x, y) = rungCell(r, ch <= '9' ? ch - '0' : ch - 'a' + 10);
        }
    return c;
}

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

std::string row(const Canvas& c, const Ramp& r, int y) {
    std::string s = rungMap(c, r);
    std::istringstream in(s);
    std::string line;
    for (int k = 0; k <= y; ++k)
        std::getline(in, line);
    return line;
}

// An ellipse `across` columns by `tall` rows about (cx, cy) on a w × h mask.
Mask ellipse(int w, int h, double cx, double cy, double rx, double ry) {
    Mask m(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (std::pow((x - cx) / rx, 2) + std::pow((y - cy) / ry, 2) <= 1)
                m.set(x, y);
    return m;
}

// The disc of Claude's round-5 survey: `across` columns, half as many rows,
// on a canvas three columns and three rows bigger all round.
Mask disc(int across) {
    const int w = across + 6, h = across / 2 + 6;
    return ellipse(w, h, (w - 1) / 2.0, (h - 1) / 2.0, across / 2.0, across / 4.0);
}

// An L sw by sh with arms `arm` thick, two cells in from the canvas edge:
// the left arm full height, the bottom arm full width.
Mask ell(int sw, int sh, int arm) {
    Mask m(sw + 4, sh + 4);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x)
            if (x < arm || y >= sh - arm)
                m.set(x + 2, y + 2);
    return m;
}

Canvas toneCanvas(const Ramp& r, const Mask& m, const std::vector<int>& tones) {
    Canvas c = black(m.width(), m.height());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (const int t = tones[size_t(y) * m.width() + x]; t >= 0)
                c.at(x, y) = rungCell(r, t);
    return c;
}

// The form's box, cells off it black: what `--box` reads.
Canvas boxOf(const Canvas& c, const Mask& m) {
    int x0 = m.width(), y0 = m.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.contains(x, y))
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    Canvas box = black(x1 - x0 + 1, y1 - y0 + 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (m.contains(x, y))
                box.at(x - x0, y - y0) = c.at(x, y);
    return box;
}

// The detail over the box at each of the 8 alignments of its 4x2
// windows (the box cropped dx columns and dy rows in), averaged over those
// with windows.
std::pair<double, double> alignedDetail(const Canvas& box) {
    double mean = 0, flat = 0;
    int n = 0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 4; ++dx) {
            const auto d = lint::detail(box, lint::Box{dx, dy, box.width() - 1, box.height() - 1});
            if (d.windows == 0)
                continue;
            mean += d.mean;
            flat += d.flatPercent;
            ++n;
        }
    return n ? std::pair{mean / n, flat / n} : std::pair{0.0, 0.0};
}

// Shares of the form's cells: black, and at the lit plane's rung or above.
std::pair<double, double> blackAndLit(const Canvas& c, const Ramp& r, const Mask& m, int plane) {
    int cells = 0, k = 0, lit = 0;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.contains(x, y)) {
                ++cells;
                const int g = rungOf(r, c.at(x, y)).value_or(-1);
                k += g == 0;
                lit += g >= plane;
            }
    return {double(k) / cells, double(lit) / cells};
}

} // namespace

TEST_CASE("form round 5, P2-A: black no more than the shadow, the lit plane no less than the light gives it",
          "[shading][form]") {
    // Claude round 5, P2-A: a 36-across disc lit from the north-west was 61%
    // black against a 24% shadow, its lit plane 5%; 64 across, 74%.
    const Ramp r = steel();
    for (const int across : {12, 20, 36, 50, 64})
        for (int l = 0; l < 8; ++l)
            for (const auto shape : {FormShape::Dome, FormShape::Cylinder})
                for (const bool spec : {true, false}) {
                    const Mask m = disc(across);
                    FormOptions fo;
                    fo.shape = shape;
                    fo.rim = false;
                    fo.specular = spec;
                    const FormResult f = formShade(r, m, fo, Compass(l));
                    const Canvas c = toneCanvas(r, m, f.tones);
                    // the lit plane: the top rung, or below the glint's two
                    // cells the solid rung under it
                    const int plane = f.reading.plane;
                    int atTop = 0;
                    for (const int t : f.tones)
                        atTop += t == r.topRung();
                    CHECK((plane == r.topRung() || (plane == r.topRung() - kRungsPerStep && atTop == 2)));
                    const FormLight light = formLight(r, m, fo, Compass(l), plane);
                    const auto [k, lit] = blackAndLit(c, r, m, plane);
                    INFO("disc " << across << " light " << l << " shape " << int(shape) << " specular " << spec << "\n"
                                 << rungMap(c, r) << "black " << k << " shadow " << light.shadow << ", lit " << lit
                                 << " floor " << light.lit);
                    REQUIRE(f.reading.laid);
                    CHECK(light.shadow > 0.0);
                    CHECK(light.shadow < 0.6);
                    CHECK(k <= light.shadow + 0.10 + 1e-9);
                    CHECK(lit >= light.lit - 1e-9);
                    // the reading the brush gives is the canvas's
                    CHECK(std::abs(f.reading.black - k) < 1e-9);
                    CHECK(std::abs(f.reading.lit - lit) < 1e-9);
                    CHECK(std::abs(f.reading.shadow - light.shadow) < 1e-9);
                }
}

TEST_CASE("form round 5, P2-B: replaying a stroke's dabs paints what playing them once does", "[shading][form]") {
    const Ramp r = steel();
    SECTION("Claude: a 4x2 canvas stroked back and forth") {
        const Canvas start = rungs(r, {"2223", "2223"});
        FormShadeBrush brush(r, FormOptions{});
        BrushSettings s;
        s.size = 1;
        s.tip = Tip::Round;
        s.strength = 1.0;
        s.lightDirection = Compass::SW;
        auto play = [&](std::vector<std::pair<int, int>> dabs) {
            Stroke st(start, brush, s);
            for (const auto& [x, y] : dabs)
                st.addDab({x, y});
            return st.preview();
        };
        const Canvas back = play({{2, 0}, {0, 0}}), forth = play({{0, 0}, {2, 0}}),
                     twice = play({{2, 0}, {0, 0}, {2, 0}, {0, 0}});
        // round 8: a dab that takes cells back leaves them as the last dab
        // did, so the order can tell (back 2663, forth 7223 over 2223)
        INFO("back\n" << rungMap(back, r) << "forth\n" << rungMap(forth, r) << "twice\n" << rungMap(twice, r));
        CHECK(twice == back);
        CHECK(!(back == start));
        CHECK(!(forth == start));
    }
    SECTION("Codex finding 2: a mirrored stroke replayed in the same stroke") {
        const Canvas start = black(5, 1);
        FormOptions fo;
        fo.shape = FormShape::Plane;
        fo.rim = false;
        fo.specular = false;
        FormShadeBrush brush(r, fo);
        BrushSettings s;
        s.size = 2;
        s.tip = Tip::Square;
        s.pressure = PressureTo::Strength;
        s.lightDirection = Compass::W;
        StrokeOptions o;
        o.symmetry = {SymmetryMode::MirrorX};
        Stroke st(start, brush, s, o);
        st.addDab({3, 0, 1.0}, false);
        st.addDab({0, 0, 0.5}, false);
        const Canvas once = st.preview();
        st.addDab({3, 0, 1.0}, false);
        st.addDab({0, 0, 0.5}, false);
        INFO("once " << rungMap(once, r) << "replayed " << rungMap(st.preview(), r));
        CHECK(st.preview() == once);
    }
    SECTION("Claude: one mirrored dab stamped twice") {
        const Canvas start = rungs(r, {"999a", "999a"});
        FormShadeBrush brush(r, FormOptions{});
        BrushSettings s;
        s.size = 3;
        s.tip = Tip::Round;
        s.lightDirection = Compass::SE;
        StrokeOptions o;
        o.symmetry = {SymmetryMode::MirrorX};
        Stroke once(start, brush, s, o), twice(start, brush, s, o);
        once.addDab({2, 0});
        twice.addDab({2, 0});
        twice.addDab({2, 0});
        INFO("once\n" << rungMap(once.preview(), r) << "twice\n" << rungMap(twice.preview(), r));
        CHECK(twice.preview() == once.preview());
    }
}

namespace {

// `strokes` random strokes on 4-20 by 2-8 canvases (flat fields and
// gradients), sizes 1-5, every light, a few unjoined dabs at random
// pressures: replaying the dabs in the same stroke changes nothing, and
// played in either order they add no finding to the canvas they began on.
// Since round 8 a dab's take-backs keep what the last dab left, so the
// order can change what is painted: fewer than half the strokes differ.
void checkFootprint(SymmetryMode mode, int strokes, unsigned seed) {
    std::mt19937 rng(seed);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    int replayed = 0, reordered = 0, painted = 0, added = 0;
    std::string first;
    for (int k = 0; k < strokes; ++k) {
        const Ramp r = *findRamp(kRamps[size_t(pick(0, 4))]);
        const int w = pick(4, 20), h = pick(2, 8);
        Canvas start(w, h);
        const bool gradient = pick(0, 1);
        const int base = pick(0, r.topRung());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                start.at(x, y) = rungCell(r, gradient ? std::clamp(base + (x - w / 2) / 2, 0, r.topRung()) : base);
        FormOptions fo;
        fo.shape = FormShape(pick(0, 2));
        fo.rim = pick(0, 1);
        fo.specular = pick(0, 1);
        FormShadeBrush brush(r, fo);
        BrushSettings s;
        s.size = pick(1, 5);
        s.tip = Tip(pick(0, 1));
        s.pressure = pick(0, 1) ? PressureTo::Strength : PressureTo::Nothing;
        s.lightDirection = Compass(pick(0, 7));
        StrokeOptions o;
        o.symmetry = {mode};
        o.ice = pick(0, 1);
        std::vector<Dab> dabs;
        for (int n = pick(2, 5); n > 0; --n)
            dabs.push_back({pick(0, w - 1), pick(0, h - 1), pick(1, 4) / 4.0});
        Stroke st(start, brush, s, o);
        for (const Dab& d : dabs)
            st.addDab(d, false);
        const Canvas once = st.preview();
        painted += !(once == start);
        for (const Dab& d : dabs)
            st.addDab(d, false);
        const bool replay = st.preview() == once;
        std::vector<Dab> shuffled = dabs;
        bwtest::shuffle(shuffled.begin(), shuffled.end(), rng);
        Stroke other(start, brush, s, o);
        for (const Dab& d : shuffled)
            other.addDab(d, false);
        const bool order = other.preview() == once;
        for (const Canvas& after : {once, other.preview()}) {
            std::vector<std::pair<int, int>> changed;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (!(after.at(x, y) == start.at(x, y)))
                        changed.emplace_back(x, y);
            StrokeLint guard(start, r);
            added += !guard.judge(after, changed).empty();
        }
        replayed += !replay;
        reordered += !order;
        if (!replay && first.empty()) {
            std::ostringstream out;
            out << "stroke " << k << ": " << r.name << " " << w << "x" << h << " shape " << int(fo.shape)
                << " rim " << fo.rim << " glint " << fo.specular << " size " << s.size << " tip " << int(s.tip)
                << " light " << int(s.lightDirection) << " ice " << o.ice << " dabs";
            for (const Dab& d : dabs)
                out << " (" << d.x << "," << d.y << "," << d.pressure << ")";
            out << "\nstart\n" << rungMap(start, r) << "once\n" << rungMap(once, r) << "replayed\n"
                << rungMap(st.preview(), r) << "reordered\n" << rungMap(other.preview(), r);
            first = out.str();
        }
    }
    INFO(first);
    INFO(reordered << " of " << strokes << " strokes differ in another order");
    CHECK(painted > strokes / 2);
    CHECK(replayed == 0);
    CHECK(added == 0);
    CHECK(reordered * 2 < strokes);
}

} // namespace

TEST_CASE("form round 5, P2-B: replay and dab order, no symmetry", "[shading][form]") {
    checkFootprint(SymmetryMode::None, 600, 5001);
}

TEST_CASE("form round 5, P2-B: replay and dab order, mirror X", "[shading][form][symmetry]") {
    checkFootprint(SymmetryMode::MirrorX, 400, 5002);
}

TEST_CASE("form round 5, P2-B: replay and dab order, mirror Y", "[shading][form][symmetry]") {
    checkFootprint(SymmetryMode::MirrorY, 400, 5003);
}

TEST_CASE("form round 5, P2-B: replay and dab order, mirror X and Y", "[shading][form][symmetry]") {
    checkFootprint(SymmetryMode::MirrorXY, 400, 5004);
}

TEST_CASE("form round 5, P3-A: the glint is a spot on the lit plane", "[shading][form]") {
    // Claude round 5, P3-A: on a 30x10 log lit from the north-west the
    // glint's ladder ran down to the black bottom row (`12344321`).
    const Ramp r = steel();
    struct Case {
        const char* name;
        Mask m;
    };
    const std::vector<Case> cases{{"log 30x10", Mask::rect(34, 14, 2, 2, 31, 11)},
                                  {"log 40x8", Mask::rect(44, 12, 2, 2, 41, 9)},
                                  {"pillar 8x20", Mask::rect(12, 24, 2, 2, 9, 21)},
                                  {"disc 36", disc(36)}};
    int glinted = 0, discGlints = 0;
    for (const auto& k : cases)
        for (int l = 0; l < 8; ++l)
            for (const auto shape : {FormShape::Cylinder, FormShape::Dome}) {
                FormOptions fo;
                fo.shape = shape;
                fo.rim = false;
                fo.specular = true;
                const FormResult f = formShade(r, k.m, fo, Compass(l));
                const Canvas c = toneCanvas(r, k.m, f.tones);
                INFO(k.name << " light " << l << " shape " << int(shape) << "\n" << rungMap(c, r));
                // without a glint the plane is the top rung
                const int plane = f.reading.plane;
                if (plane == r.topRung())
                    continue;
                ++glinted;
                discGlints += std::string(k.name) == "disc 36";
                // every cell above the lit plane is within the glint's
                // falloff of the glint, and the cells round it out to the
                // falloff's end are on the plane: the spot never reaches the
                // fall or the shadow
                std::vector<std::pair<int, int>> above;
                for (int y = 0; y < k.m.height(); ++y)
                    for (int x = 0; x < k.m.width(); ++x)
                        if (k.m.contains(x, y) && *rungOf(r, c.at(x, y)) > plane)
                            above.emplace_back(x, y);
                if (above.empty())
                    continue;
                int x0 = 1 << 20, x1 = -1, y0 = 1 << 20, y1 = -1;
                for (const auto& [x, y] : above)
                    x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                // a 2-5 px cluster's falloff: no taller than the ramp's
                // steps from the plane to the top would make it twice over
                const int steps = r.topRung() - plane;
                CHECK(y1 - y0 + 1 <= 2 * steps);
                CHECK(x1 - x0 + 1 <= 4 * steps + 2);
                for (const auto& [x, y] : above)
                    for (const auto& [dx, dy] : {std::pair{0, 1}, std::pair{0, -1}, std::pair{1, 0}, std::pair{-1, 0}})
                        if (k.m.contains(x + dx, y + dy))
                            CHECK(*rungOf(r, c.at(x + dx, y + dy)) >= plane);
                // and the row or column of the form farthest from the
                // light carries nothing above the plane
                CHECK(std::none_of(above.begin(), above.end(), [&](const auto& xy) {
                    for (const auto& [dx, dy] : {std::pair{0, 1}, std::pair{0, -1}, std::pair{1, 0}, std::pair{-1, 0}})
                        if (!k.m.contains(xy.first + dx, xy.second + dy))
                            return true;
                    return false;
                }));
            }
    // the disc has room for a glint as a dome and as a cylinder under
    // every light; the thin logs and the pillar mostly have none, and go
    // without
    CHECK(discGlints == 16);
    CHECK(glinted >= 16);
    // the log of the repro: nothing above the plane on its bottom row
    FormOptions fo;
    fo.shape = FormShape::Cylinder;
    fo.rim = false;
    fo.specular = true;
    const Mask log = Mask::rect(34, 14, 2, 2, 31, 11);
    const FormResult f = formShade(r, log, fo, Compass::NW);
    const Canvas c = toneCanvas(r, log, f.tones);
    INFO(rungMap(c, r));
    const std::string bottom = row(c, r, 11).substr(2, 30);
    CHECK(std::all_of(bottom.begin(), bottom.end(), [&](char t) { return (t <= '9' ? t - '0' : t - 'a' + 10) <= f.reading.plane; }));
}

TEST_CASE("form round 5, P3-B: a cylinder lit along its axis still turns", "[shading][form]") {
    // Claude round 5, P3-B: an oval pillar lit from the north was 82% one
    // rung, an L log lit from the west 87%.
    const Ramp r = steel();
    struct Case {
        const char* name;
        Mask m;
        std::vector<Compass> lights;
    };
    const std::vector<Case> cases{
        {"oval pillar", ellipse(30, 40, 14.5, 19.5, 10, 17), {Compass::N, Compass::S}},
        {"L 40x20 log", ell(40, 20, 12), {Compass::W, Compass::E}},
        {"log 40x8", Mask::rect(44, 12, 2, 2, 41, 9), {Compass::W, Compass::E}},
        {"pillar 10x24", Mask::rect(14, 28, 2, 2, 11, 25), {Compass::N, Compass::S}}};
    for (const auto& k : cases)
        for (const Compass light : k.lights)
            for (const bool spec : {true, false}) {
                FormOptions fo;
                fo.shape = FormShape::Cylinder;
                fo.rim = false;
                fo.specular = spec;
                const auto tones = formRungs(r, k.m, fo, light);
                const Canvas c = toneCanvas(r, k.m, tones);
                std::map<int, int> n;
                int cells = 0;
                for (const int t : tones)
                    if (t >= 0) {
                        ++cells;
                        ++n[t];
                    }
                int most = 0;
                for (const auto& [t, count] : n)
                    most = std::max(most, count);
                INFO(k.name << " light " << int(light) << " glint " << spec << "\n"
                            << rungMap(c, r) << "largest rung " << most << " of " << cells);
                REQUIRE(cells > 0);
                CHECK(most * 2 <= cells);
            }
}

TEST_CASE("form round 5, P3-C: the bar is read at every window offset", "[shading][form]") {
    // Claude round 5, P3-C: a 64-across dome lit from the south-east cleared
    // the bar at the box's corner and at 2 of the 8 offsets.
    const Ramp r = steel();
    for (const auto& [across, light] : {std::pair{64, Compass::SE}, std::pair{36, Compass::NW}}) {
        const Mask m = disc(across);
        FormOptions fo;
        fo.shape = FormShape::Dome;
        fo.rim = false;
        fo.specular = true;
        const FormResult f = formShade(r, m, fo, light);
        const Canvas c = toneCanvas(r, m, f.tones);
        const Canvas box = boxOf(c, m);
        const auto [mean, flat] = alignedDetail(box);
        const auto corner = lint::detail(box);
        INFO("disc " << across << " light " << int(light) << "\n"
                     << rungMap(c, r) << "aligned " << mean << " / " << flat << "%, corner " << corner.mean << " / "
                     << corner.flatPercent << "%; the brush read " << f.reading.detail << " / "
                     << f.reading.flatPercent << "%, corner " << f.reading.corner.mean << " / "
                     << f.reading.corner.flatPercent << "%, clears " << f.reading.clears);
        CHECK(std::abs(f.reading.detail - mean) < 1e-9);
        CHECK(std::abs(f.reading.flatPercent - flat) < 1e-9);
        CHECK(f.reading.corner.mean == corner.mean);
        CHECK(f.reading.corner.flatPercent == corner.flatPercent);
        // it clears only by the mean over the offsets
        if (f.reading.clears) {
            CHECK(mean >= 3.6);
            CHECK(flat < 15);
        }
        if (mean < 3.6 || flat >= 15)
            CHECK_FALSE(f.reading.clears);
    }
}

TEST_CASE("form round 6: the glint is a 2-5 cell cluster at every size", "[shading][form]") {
    // Claude round 6 (#490): on a large dome the try picked was a
    // bull's-eye 13-15 columns across, rungs 9/a/b/c round two W cells;
    // a glint wants a 2-5 px cluster, and wide rings read as a disc.
    // The glint's top rung is a cluster of 2-5 cells, and it falls to the
    // plane at least a rung a cell (a step on the ladder) from that
    // cluster: no falloff wider than the ladder makes it.
    const Ramp r = steel();
    int glinted = 0;
    for (int across = 12; across <= 64; across += 4) {
        const Mask m = disc(across);
        for (int l = 0; l < 8; ++l)
            for (const auto shape : {FormShape::Cylinder, FormShape::Dome}) {
                FormOptions fo;
                fo.shape = shape;
                fo.rim = false;
                fo.specular = true;
                const FormResult f = formShade(r, m, fo, Compass(l));
                const int plane = f.reading.plane;
                if (plane == r.topRung())
                    continue;
                const Canvas c = toneCanvas(r, m, f.tones);
                INFO("disc " << across << " light " << l << " shape " << int(shape) << "\n" << rungMap(c, r));
                std::vector<std::pair<int, int>> core;
                for (int y = 0; y < m.height(); ++y)
                    for (int x = 0; x < m.width(); ++x)
                        if (m.contains(x, y) && *rungOf(r, c.at(x, y)) == r.topRung())
                            core.emplace_back(x, y);
                if (core.empty())
                    continue;
                ++glinted;
                CHECK(core.size() >= 2);
                CHECK(core.size() <= 5);
                // one cluster: every top cell joined to the first through
                // top cells
                std::vector<std::pair<int, int>> reached{core.front()};
                for (size_t k = 0; k < reached.size(); ++k)
                    for (const auto& q : core)
                        if (std::abs(q.first - reached[k].first) + std::abs(q.second - reached[k].second) == 1 &&
                            std::find(reached.begin(), reached.end(), q) == reached.end())
                            reached.push_back(q);
                CHECK(reached.size() == core.size());
                for (int y = 0; y < m.height(); ++y)
                    for (int x = 0; x < m.width(); ++x) {
                        if (!m.contains(x, y))
                            continue;
                        const int t = *rungOf(r, c.at(x, y));
                        if (t <= plane)
                            continue;
                        int d = 1 << 20;
                        for (const auto& [qx, qy] : core)
                            d = std::min(d, std::abs(x - qx) + std::abs(y - qy));
                        INFO("cell " << x << "," << y << " rung " << t << " " << d << " from the top");
                        CHECK(t <= r.topRung() - d);
                    }
            }
    }
    CHECK(glinted >= 100);
}

TEST_CASE("form round 6: a thin log turns to black on its shadow edge", "[shading][form]") {
    // Claude round 6 (#490): the 30x10 log of round 5's repro, lit from
    // the north-west, came out as one-row bands b c b a 9 8 7 6 5 4 with
    // no black: a straight-ramp stripe, not a cylinder. A cylinder too thin
    // for the ladder to come down to black still has it on its shadow
    // edge: at least three rungs across it, the fall between the lit plane
    // and the black, and the black within the light's bound. The same for
    // a log and a pillar four cells across.
    const Ramp r = steel();
    struct Case {
        const char* name;
        Mask m;
        bool log;  // shaded down its columns; else a pillar, across its rows
    };
    const std::vector<Case> cases{{"log 30x10", Mask::rect(34, 14, 2, 2, 31, 11), true},
                                  {"log 30x4", Mask::rect(34, 8, 2, 2, 31, 5), true},
                                  {"pillar 4x20", Mask::rect(8, 24, 2, 2, 5, 21), false}};
    for (const auto& k : cases)
        for (const bool spec : {true, false}) {
            FormOptions fo;
            fo.shape = FormShape::Cylinder;
            fo.rim = false;
            fo.specular = spec;
            const FormResult f = formShade(r, k.m, fo, Compass::NW);
            const Canvas c = toneCanvas(r, k.m, f.tones);
            INFO(k.name << " glint " << spec << "\n"
                        << rungMap(c, r) << "black " << f.reading.black << " of at most " << f.reading.shadow + 0.10);
            CHECK(f.reading.bounded);
            CHECK(f.reading.black <= f.reading.shadow + 0.10 + 1e-9);
            int x0 = k.m.width(), y0 = k.m.height(), x1 = -1, y1 = -1;
            for (int y = 0; y < k.m.height(); ++y)
                for (int x = 0; x < k.m.width(); ++x)
                    if (k.m.contains(x, y))
                        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            const int lines = k.log ? x1 - x0 + 1 : y1 - y0 + 1, len = k.log ? y1 - y0 + 1 : x1 - x0 + 1;
            for (int i = 0; i < lines; ++i) {
                // a column of the log, top to bottom; a row of the pillar,
                // left to right: the light is north-west, so the shadow
                // edge is the last cell
                std::vector<int> across;
                for (int j = 0; j < len; ++j)
                    across.push_back(*rungOf(r, k.log ? c.at(x0 + i, y0 + j) : c.at(x0 + j, y0 + i)));
                INFO("line " << i);
                CHECK(across.back() == 0);
                CHECK(across.front() > 0);
                std::vector<int> rungs = across;
                std::sort(rungs.begin(), rungs.end());
                CHECK(std::unique(rungs.begin(), rungs.end()) - rungs.begin() >= 3);
            }
        }
}

namespace {

// A canvas of rungs that climb across it, `steps` columns a rung.
Canvas gradient(const Ramp& r, int w, int h) {
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c.at(x, y) = rungCell(r, std::clamp(x * (r.topRung() + 1) / w, 0, r.topRung()));
    return c;
}

// The most work (FormShadeBrush::cellsVisited) a stamp takes over a wave
// 40 rows high across a w x h gradient, a size-5 round brush, in the dabs [from, to) of
// the stroke as a share of its dabs.
long long wavePeak(int w, int h, double from, double to) {
    const Ramp r = steel();
    const Canvas start = gradient(r, w, h);
    FormShadeBrush brush(r);
    BrushSettings s;
    s.size = 5;
    s.tip = Tip::Round;
    s.strength = 0.5;
    Stroke st(start, brush, s);
    std::vector<Dab> dabs;
    for (int x = 6; x < w - 6; x += 3)
        dabs.push_back({x, int(std::lround(h / 2 + 20 * std::sin(x / 12.0)))});
    long long peak = 0;
    for (size_t i = 0; i < dabs.size(); ++i) {
        const long long work = FormShadeBrush::cellsVisited();
        const long long stamps = st.stamps();
        st.addDab(dabs[i]);
        const long long per = (FormShadeBrush::cellsVisited() - work) / std::max(1LL, st.stamps() - stamps);
        if (i >= size_t(from * double(dabs.size())) && i < size_t(to * double(dabs.size())))
            peak = std::max(peak, per);
    }
    return peak;
}

// The most work a stamp takes after the first dab, over a wave across a
// w x h gradient with all but a ten-cell border selected.
long long selectedPeak(int w, int h) {
    const Ramp r = steel();
    const Canvas start = gradient(r, w, h);
    Mask m(w, h);
    for (int y = 10; y < h - 10; ++y)
        for (int x = 10; x < w - 10; ++x)
            m.set(x, y);
    FormShadeBrush brush(r);
    BrushSettings s;
    s.size = 5;
    s.tip = Tip::Round;
    StrokeOptions o;
    o.selection = std::make_shared<const Mask>(std::move(m));
    Stroke st(start, brush, s, o);
    long long peak = 0;
    for (int x = 12; x < w - 12; x += 3) {
        const long long work = FormShadeBrush::cellsVisited();
        const long long stamps = st.stamps();
        st.addDab({x, int(std::lround(h / 2 + 20 * std::sin(x / 12.0)))});
        if (x > 12)
            peak = std::max(peak, (FormShadeBrush::cellsVisited() - work) / std::max(1LL, st.stamps() - stamps));
    }
    return peak;
}

// How many cells a stroke has changed after each of its dabs.
std::vector<size_t> changedCounts(const Canvas& start, const Brush& brush, const BrushSettings& s,
                                  const std::vector<Dab>& dabs) {
    Stroke st(start, brush, s);
    std::vector<size_t> counts;
    for (const Dab& d : dabs) {
        st.addDab(d);
        counts.push_back(st.edit().changes.size());
    }
    return counts;
}

} // namespace

TEST_CASE("form round 8, P1: a stamp's work doesn't grow with the stroke", "[shading][form]") {
    // with nothing selected the form is the cells covered so far: a stamp
    // that grows it works out the form near it, not the whole of it again
    const long long mid = wavePeak(160, 100, 0.3, 0.6), late = wavePeak(160, 100, 0.7, 1.0);
    const long long longer = wavePeak(400, 160, 0.7, 1.0);
    INFO("most work a stamp: middle " << mid << ", end " << late << ", end of a longer stroke " << longer);
    CHECK(late <= mid * 5 / 4);
    CHECK(longer <= mid * 5 / 4);
}

TEST_CASE("form round 8, P3-1: with a selection, a stamp's work doesn't grow with the canvas", "[shading][form]") {
    // #723 P3-1: the selection's shape was built over the whole canvas and
    // compared with the cached one at every dab, and every covered cell laid
    // again; now the shape is taken once a stroke and a stamp lays the
    // cells near it
    const long long small = selectedPeak(160, 100), large = selectedPeak(400, 160);
    INFO("most work a stamp after the first dab: " << small << " on 160x100, " << large << " on 400x160");
    CHECK(small > 0);
    CHECK(small < 160 * 100 / 10);
    CHECK(large <= small * 5 / 4);
}

TEST_CASE("form round 8, P2: a dab that can't lay anything keeps what earlier dabs laid", "[shading][form]") {
    // #723 P2-1: with no finding near a changed cell (one glyph), the whole
    // stroke went back to the canvas it began on mid-drag
    struct Case {
        const char* ramp;
        int rung;
    };
    for (const Case& k : {Case{"toxic", 4}, Case{"steel", 8}}) {
        const Ramp r = *findRamp(k.ramp);
        Canvas start(40, 16);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 40; ++x)
                start.at(x, y) = rungCell(r, k.rung);
        FormShadeBrush brush(r, FormOptions{});
        BrushSettings s;
        s.size = 14;
        s.tip = Tip::Square;
        std::vector<Dab> dabs;
        for (int x = 2; x <= 38; x += 2)
            dabs.push_back({x, 8});
        const auto counts = changedCounts(start, brush, s, dabs);
        std::ostringstream out;
        for (size_t n : counts)
            out << n << " ";
        INFO(k.ramp << " at rung " << k.rung << ": changed cells after each dab " << out.str());
        size_t most = 0;
        for (size_t n : counts) {
            CHECK(n * 2 >= most);
            most = std::max(most, n);
        }
        CHECK(most > 0);
    }
}
