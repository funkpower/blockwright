// Symmetry and the stroke join: a drag's mirrored images are the exact
// images of the stroke's own cells (#125), for the dab brushes and the
// half-block pencil, on odd and even canvases.

#include "blockwright/brush.h"
#include "portable_random.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/ramp.h"
#include "blockwright/shapes.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace bw;

namespace {

using Point = std::pair<int, int>;
using Cells = std::set<Point>;

Cell hash() {
    Cell c;
    c.ch = U'#';
    c.fg = Color::indexed(White);
    return c;
}

struct Step {
    Dab dab;
    bool join = true;
};

Cells paintedBy(const Canvas& c, const Brush& b, const BrushSettings& s, const std::vector<Step>& steps,
                const Symmetry& sym) {
    StrokeOptions o;
    o.symmetry = sym;
    Stroke st(c, b, s, o);
    for (const Step& p : steps)
        st.addDab(p.dab, p.join);
    const auto v = st.paintedCells();
    return Cells(v.begin(), v.end());
}

// The cells the stroke's own image covers, unclipped: each joining line from
// its lesser end (by row, then column), every point of it stamped.
Cells ownCells(const BrushSettings& s, const std::vector<Step>& steps) {
    Cells out;
    std::optional<Point> last;
    for (const Step& p : steps) {
        const Point at{p.dab.x, p.dab.y};
        std::vector<Point> centres{at};
        if (last && p.join)
            centres = std::pair{last->second, last->first} < std::pair{at.second, at.first}
                          ? line4(last->first, last->second, at.first, at.second)
                          : line4(at.first, at.second, last->first, last->second);
        for (const auto& [x, y] : centres)
            for (const Point q : footprint(x, y, s.size, s.tip))
                out.insert(q);
        last = at;
    }
    return out;
}

// The reflections a symmetry makes (itself first), as doubled centres:
// x maps to c2x - x when flipped across.
struct Reflection {
    bool flipX, flipY;
};

std::vector<Reflection> reflections(SymmetryMode m) {
    switch (m) {
    case SymmetryMode::MirrorX: return {{false, false}, {true, false}};
    case SymmetryMode::MirrorY: return {{false, false}, {false, true}};
    case SymmetryMode::MirrorXY: return {{false, false}, {true, false}, {false, true}, {true, true}};
    case SymmetryMode::Radial: return {{false, false}, {true, true}};  // the half turn
    case SymmetryMode::None: break;
    }
    return {{false, false}};
}

// The own cells and their reflections about doubled centre c2x, c2y, on a
// w x h canvas.
Cells reflected(const Cells& own, const std::vector<Reflection>& rs, long long c2x, long long c2y, int w, int h) {
    Cells out;
    for (const auto& [x, y] : own)
        for (const Reflection r : rs) {
            const long long ix = r.flipX ? c2x - x : x, iy = r.flipY ? c2y - y : y;
            if (ix >= 0 && iy >= 0 && ix < w && iy < h)
                out.insert({int(ix), int(iy)});
        }
    return out;
}

std::string describe(int w, int h, const Symmetry& sym, const BrushSettings& s, const std::vector<Step>& steps) {
    std::ostringstream o;
    o << w << "x" << h << " mode " << int(sym.mode) << " folds " << sym.folds;
    if (sym.centre)
        o << " centre " << sym.centre->first << "," << sym.centre->second;
    o << " size " << s.size << (s.tip == Tip::Round ? " round:" : " square:");
    for (const Step& p : steps)
        o << " " << p.dab.x << "," << p.dab.y << (p.join ? "" : " new");
    return o.str();
}

// The pixels of colour `colour` on the canvas.
std::set<std::pair<int, int>> pixelsOf(const Canvas& c, std::uint8_t colour) {
    std::set<std::pair<int, int>> out;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const LayerPixels p = layerPixels(c.at(x, y));
            if (p.top == colour)
                out.insert({x, 2 * y});
            if (p.bottom == colour)
                out.insert({x, 2 * y + 1});
        }
    return out;
}

} // namespace

TEST_CASE("a mirrored drag's images take the stroke's own cells (#125 repro)", "[symmetry][brush]") {
    // 81 x 41, mirror Y, a size-21 square from (34,16) to (38,23): the
    // image's own line walked a different staircase, so (45,6) was painted
    // and its mirror (45,34) wasn't
    const Canvas c(81, 41);
    const GlyphBrush pen(hash());
    const BrushSettings s{21, Tip::Square};
    const std::vector<Step> steps{{{34, 16}, false}, {{38, 23}}};
    const Cells got = paintedBy(c, pen, s, steps, {SymmetryMode::MirrorY});
    CHECK(got.count({45, 6}) == got.count({45, 34}));
    CHECK(got == reflected(ownCells(s, steps), reflections(SymmetryMode::MirrorY), 80, 40, 81, 41));
}

TEST_CASE("every mirror's drag is cell-exact on odd and even canvases", "[symmetry][brush]") {
    // Short and long drags on every slope, each way round, so a reflection
    // turns which end of the line is the lesser one, at several sizes and
    // both tips
    const GlyphBrush pen(hash());
    const std::vector<std::vector<Step>> drags{
        {{{34, 16}, false}, {{38, 23}}},
        {{{38, 23}, false}, {{34, 16}}},
        {{{5, 3}, false}, {{30, 9}}, {{12, 30}}},
        {{{70, 2}, false}, {{61, 38}}, {{66, 4}}},
        {{{10, 35}, false}, {{75, 31}}, {{40, 7}, false}, {{43, 19}}},
        {{{20, 20}, false}, {{23, 21}}, {{26, 24}}, {{25, 29}}, {{19, 31}}},
    };
    int checked = 0;
    for (const auto& [w, h] : {std::pair{81, 41}, std::pair{80, 40}, std::pair{81, 40}, std::pair{80, 41}})
        for (const SymmetryMode m : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY})
            for (const int size : {1, 2, 3, 4, 7, 12, 21})
                for (const Tip tip : {Tip::Square, Tip::Round})
                    for (const auto& steps : drags) {
                        const Canvas c(w, h);
                        const BrushSettings s{size, tip};
                        const Symmetry sym{m};
                        INFO(describe(w, h, sym, s, steps));
                        REQUIRE(paintedBy(c, pen, s, steps, sym) ==
                                reflected(ownCells(s, steps), reflections(m), w - 1, h - 1, w, h));
                        ++checked;
                    }
    CHECK(checked == 4 * 3 * 7 * 2 * 6);
}

TEST_CASE("a half turn's drag is cell-exact about the canvas centre or a set cell", "[symmetry][brush]") {
    const GlyphBrush pen(hash());
    const std::vector<Step> steps{{{34, 16}, false}, {{38, 23}}, {{50, 20}}};
    for (const auto& [w, h] : {std::pair{81, 41}, std::pair{80, 40}})
        for (const std::optional<Point> centre : {std::optional<Point>{}, std::optional<Point>{Point{30, 12}},
                                                  std::optional<Point>{Point{70, 35}}})
            for (const int size : {1, 4, 9}) {
                const Canvas c(w, h);
                const BrushSettings s{size, Tip::Round};
                const Symmetry sym{SymmetryMode::Radial, 2, centre};
                INFO(describe(w, h, sym, s, steps));
                const long long c2x = centre ? 2LL * centre->first : w - 1, c2y = centre ? 2LL * centre->second : h - 1;
                CHECK(paintedBy(c, pen, s, steps, sym) ==
                      reflected(ownCells(s, steps), reflections(SymmetryMode::Radial), c2x, c2y, w, h));
            }
}

TEST_CASE("random drags under every symmetry are cell-exact where the images are reflections",
          "[symmetry][brush][property]") {
    // Random drags (new segments too, dabs off the canvas and back), sizes
    // 1-24 on both tips, on canvases of every parity. Mirrors and the half
    // turn: the stroke paints exactly its own cells and their reflections.
    // Other radial turns are rounded, so they walk lines of their own; the
    // stroke's own cells and their half turn (even folds) are still all
    // painted. The ramp shade brush paints the same cells as the glyph
    // brush, and the same whole cell (glyph and both colours) on a cell and
    // its reflections.
    std::mt19937 rng(20261024);
    auto in = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const GlyphBrush pen(hash());
    const RampShadeBrush shade(*findRamp("steel"), ShadeDirection::Lighten);
    int exact = 0, turned = 0, parity[4] = {};
    for (int n = 0; n < 3000; ++n) {
        const int w = in(1, 40), h = in(1, 24);
        ++parity[(w % 2) * 2 + h % 2];
        const SymmetryMode modes[5] = {SymmetryMode::None, SymmetryMode::MirrorX, SymmetryMode::MirrorY,
                                       SymmetryMode::MirrorXY, SymmetryMode::Radial};
        Symmetry sym{modes[in(0, 4)]};
        if (sym.mode == SymmetryMode::Radial) {
            sym.folds = in(kMinFolds, kMaxFolds);
            if (in(0, 1))
                sym.centre = Point{in(0, w - 1), in(0, h - 1)};
        }
        BrushSettings s;
        s.size = in(1, 24);
        s.tip = in(0, 1) ? Tip::Round : Tip::Square;
        std::vector<Step> steps(size_t(in(1, 6)));
        for (size_t i = 0; i < steps.size(); ++i)
            steps[i] = {{in(-8, w + 7), in(-8, h + 7)}, i > 0 && in(0, 4) != 0};
        const Canvas c(w, h);
        INFO(describe(w, h, sym, s, steps));
        const Cells got = paintedBy(c, pen, s, steps, sym);
        const long long c2x = sym.centre ? 2LL * sym.centre->first : w - 1,
                        c2y = sym.centre ? 2LL * sym.centre->second : h - 1;
        const Cells own = ownCells(s, steps);
        if (sym.mode != SymmetryMode::Radial || sym.folds == 2) {
            REQUIRE(got == reflected(own, reflections(sym.mode), c2x, c2y, w, h));
            ++exact;
            // a shade stroke paints the same cells, a cell and its
            // reflections alike
            StrokeOptions o;
            o.symmetry = sym;
            Canvas shaded(w, h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    shaded.at(x, y).bg = Color::indexed(Black);
            Stroke st(shaded, shade, s, o);
            for (const Step& p : steps)
                st.addDab(p.dab, p.join);
            const auto cells = st.paintedCells();
            REQUIRE(Cells(cells.begin(), cells.end()) == got);
            const Canvas out = st.preview();
            for (const auto& [x, y] : got)
                for (const Reflection r : reflections(sym.mode)) {
                    const long long ix = r.flipX ? c2x - x : x, iy = r.flipY ? c2y - y : y;
                    if (ix >= 0 && iy >= 0 && ix < w && iy < h)
                        REQUIRE(out.at(int(ix), int(iy)) == out.at(x, y));
                }
        } else {
            const std::vector<Reflection> rs = sym.folds % 2 == 0 ? reflections(SymmetryMode::Radial)
                                                                  : std::vector<Reflection>{{false, false}};
            for (const Point p : reflected(own, rs, c2x, c2y, w, h))
                REQUIRE(got.count(p) == 1);
            ++turned;
        }
    }
    INFO(exact << " exact, " << turned << " turned");
    CHECK(exact > 2000);
    CHECK(turned > 300);
    for (const int k : parity)
        CHECK(k > 500);
}

TEST_CASE("a mirrored half-block drag paints the exact image of its pixels", "[symmetry][halfblock][property]") {
    // The pencil joins each image to its own last point, from that end, so
    // a mirror walks the reflection of the stroke's own line: 4- and
    // 8-connected, on odd and even canvases
    std::mt19937 rng(20261025);
    auto in = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const SymmetryMode modes[3] = {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY};
    for (int n = 0; n < 1500; ++n) {
        const int w = in(1, 30), h = in(1, 15);
        const SymmetryMode m = modes[n % 3];
        const Connectivity conn = in(0, 1) ? Connectivity::Four : Connectivity::Eight;
        std::vector<std::pair<std::pair<int, int>, bool>> points(size_t(in(1, 6)));
        for (size_t i = 0; i < points.size(); ++i)
            points[i] = {{in(-6, w + 5), in(-6, 2 * h + 5)}, i > 0 && in(0, 4) != 0};
        StrokeOptions o, plainO;
        o.symmetry = {m};
        const Canvas c(w, h);
        HalfBlockStroke mirrored(c, 12, o, false, conn), plain(c, 12, plainO, false, conn);
        std::ostringstream what;
        what << w << "x" << h << " mode " << int(m) << (conn == Connectivity::Four ? " 4:" : " 8:");
        for (const auto& [p, join] : points) {
            mirrored.addPoint(p.first, p.second, join);
            plain.addPoint(p.first, p.second, join);
            what << " " << p.first << "," << p.second << (join ? "" : " new");
        }
        INFO(what.str());
        std::set<std::pair<int, int>> want;
        for (const auto& [x, py] : pixelsOf(plain.preview(), 12))
            for (const Reflection r : reflections(m))
                want.insert({r.flipX ? w - 1 - x : x, r.flipY ? 2 * h - 1 - py : py});
        REQUIRE(pixelsOf(mirrored.preview(), 12) == want);
    }
}
