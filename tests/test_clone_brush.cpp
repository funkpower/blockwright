// The clone stamp (PLAN §10, "Detail"): what it copies and from where,
// aligned and not, the canvas it reads, the edges, the selection and the
// alpha lock, and symmetry.

#include "blockwright/brush.h"
#include "blockwright/brushes/clone.h"
#include "blockwright/palette.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace bw;

namespace {

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg = Black, std::uint8_t attrs = 0) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    c.attrs = attrs;
    return c;
}

// A canvas whose top row reads `text`, a letter a cell, in red.
Canvas row(const std::u32string& text, int height = 1) {
    Canvas c(int(text.size()), height);
    for (size_t x = 0; x < text.size(); ++x)
        c.at(int(x), 0) = cell(text[x], Red);
    return c;
}

std::u32string rowText(const Canvas& c, int y = 0) {
    std::u32string out;
    for (int x = 0; x < c.width(); ++x)
        out += c.at(x, y).ch.codePoint().value();
    return out;
}

std::vector<Dab> dabs(std::initializer_list<std::pair<int, int>> at) {
    std::vector<Dab> out;
    for (const auto& [x, y] : at)
        out.push_back(Dab{x, y});
    return out;
}

// Paints a stroke and applies it.
CellEdit stamp(Canvas& c, const Brush& b, const std::vector<Dab>& ds, const StrokeOptions& o = {},
               BrushSettings s = {}) {
    CellEdit e = paintStroke(c, b, s, ds, o);
    e.apply(c);
    return e;
}

std::shared_ptr<const Mask> shared(Mask m) { return std::make_shared<const Mask>(std::move(m)); }

// The glyph a mirror shows: the half blocks swap across the mirror's axis.
char32_t mirrorGlyph(char32_t ch, bool x) {
    if (x)
        return ch == U'▌' ? U'▐' : ch == U'▐' ? U'▌' : ch;
    return ch == U'▀' ? U'▄' : ch == U'▄' ? U'▀' : ch;
}

// The mirror axes a canvas is symmetric about: columns (X), rows (Y) or both.
enum class Axes { X, Y, XY };

// A canvas symmetric about `axes`: random cells of half blocks, shades and
// letters in random colours in one half (or quarter), their mirror images in
// the others. A cell on an axis is its own mirror there, so it takes a glyph
// that mirror leaves as it is.
Canvas symmetric(int w, int h, Axes axes, unsigned seed) {
    static const char32_t glyphs[] = {U'▌', U'▐', U'▀', U'▄', U'█', U'░', U' ', U'o'};
    const bool mx = axes != Axes::Y, my = axes != Axes::X;
    std::mt19937 rng(seed);
    Canvas c(w, h);
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const int ox = mx ? w - 1 - xx : xx, oy = my ? h - 1 - yy : yy;
            if (xx > ox || yy > oy)
                continue;
            Cell a = cell(glyphs[rng() % 8], std::uint8_t(rng() % 16), std::uint8_t(rng() % 8));
            if ((xx == ox && mx && mirrorGlyph(a.ch.codePoint().value(), true) != a.ch) ||
                (yy == oy && my && mirrorGlyph(a.ch.codePoint().value(), false) != a.ch))
                a.ch = U'█';
            Cell m = a;
            c.at(xx, yy) = a;
            if (mx) {
                m.ch = mirrorGlyph(a.ch.codePoint().value(), true);
                c.at(ox, yy) = m;
            }
            if (my) {
                Cell n = a;
                n.ch = mirrorGlyph(a.ch.codePoint().value(), false);
                c.at(xx, oy) = n;
                if (mx) {
                    n.ch = mirrorGlyph(m.ch.codePoint().value(), false);
                    c.at(ox, oy) = n;
                }
            }
        }
    return c;
}

// Whether each cell is the mirror of its mirror cell about `axes`. A cell
// on an axis is its own mirror cell; it may show a half block the mirror
// would turn (it copies a cell off the axis as it is), so its glyph is
// taken as it is there.
bool isSymmetric(const Canvas& c, Axes axes) {
    for (const bool x : {true, false}) {
        if (axes == (x ? Axes::Y : Axes::X))
            continue;
        for (int yy = 0; yy < c.height(); ++yy)
            for (int xx = 0; xx < c.width(); ++xx) {
                const int mx = x ? c.width() - 1 - xx : xx, my = x ? yy : c.height() - 1 - yy;
                Cell m = c.at(xx, yy);
                if (mx != xx || my != yy)
                    m.ch = mirrorGlyph(m.ch.codePoint().value(), x);
                if (!(c.at(mx, my) == m))
                    return false;
            }
    }
    return true;
}

} // namespace

TEST_CASE("clone stamp: copies whole cells from the source at the stroke's distance") {
    Canvas c(12, 4);
    const Cell a = cell(U'▓', Yellow, Blue, attr::Blink);
    Cell b = cell(U'x', LightCyan, Magenta);
    b.transparent = true;
    c.at(1, 1) = a;
    c.at(2, 1) = b;
    c.at(1, 2) = cell(U'▀', Green, Red);
    const Canvas before = c;
    const CloneBrush brush({1, 1}, {6, 1});
    CHECK(brush.name() == "clone");
    stamp(c, brush, dabs({{6, 1}, {7, 1}}));
    CHECK(c.at(6, 1) == a);
    CHECK(c.at(6, 1).attrs == attr::Blink);
    CHECK(c.at(7, 1) == b);  // transparency and all
    CHECK(c.at(7, 1).transparent);
    CHECK(c.at(5, 1) == before.at(5, 1));
    CHECK(c.at(8, 1) == before.at(8, 1));

    // the tip: a 2x2 square copies the 2x2 block beside the source too
    Canvas d = before;
    BrushSettings s;
    s.size = 2;
    stamp(d, CloneBrush({2, 2}, {8, 2}), dabs({{8, 2}}), {}, s);
    // an even square reaches a column left and a row up: (7..8, 1..2)
    CHECK(d.at(7, 1) == a);
    CHECK(d.at(8, 1) == b);
    CHECK(d.at(7, 2) == before.at(1, 2));

    // strength doesn't matter: a weak dab copies the cell whole
    Canvas e = before;
    s = {};
    s.strength = 0.1;
    stamp(e, CloneBrush({1, 1}, {9, 3}), dabs({{9, 3}}), {}, s);
    CHECK(e.at(9, 3) == a);
}

TEST_CASE("clone stamp: aligned strokes keep the distance, others start over at the source") {
    Canvas start = row(U"ABCDEFGHIJKLMNOP", 3);
    for (int x = 0; x < 16; ++x)
        start.at(x, 1) = cell(char32_t(U'a' + x), Green);

    SECTION("aligned") {
        Canvas c = start;
        CloneSource src;
        CHECK(src.aligned);
        CHECK_FALSE(src.beginStroke(0, 0));  // no source, no brush
        src.setSource(0, 0);
        CHECK(src.sampleFor(10, 2) == std::pair<long long, long long>{0, 0});
        auto b = src.beginStroke(10, 2);
        REQUIRE(b);
        stamp(c, *b, dabs({{10, 2}, {11, 2}}));
        CHECK(c.at(10, 2).ch == U'A');
        CHECK(c.at(11, 2).ch == U'B');
        // a second stroke two cells on copies two cells on: the distance stays
        CHECK(src.sampleFor(12, 2) == std::pair<long long, long long>{2, 0});
        b = src.beginStroke(12, 2);
        REQUIRE(b);
        stamp(c, *b, dabs({{12, 2}, {13, 2}}));
        CHECK(rowText(c, 2).substr(10, 4) == U"ABCD");
        // and a stroke on another row reads that row's distance away
        b = src.beginStroke(10, 0);
        stamp(c, *b, dabs({{10, 0}}));
        CHECK(c.at(10, 0).ch == U'K');  // (10, 0) - (10, 2) + (0, 0) is off the canvas: left
        b = src.beginStroke(5, 1);
        CHECK(b->anchor() == std::pair{10, 2});
    }
    SECTION("not aligned") {
        Canvas c = start;
        CloneSource src;
        src.aligned = false;
        src.setSource(0, 1);
        auto b = src.beginStroke(10, 2);
        stamp(c, *b, dabs({{10, 2}, {11, 2}}));
        b = src.beginStroke(13, 2);
        CHECK(src.sampleFor(13, 2) == std::pair<long long, long long>{0, 1});
        // along the stroke it reads on from there
        CHECK(b->sampleAt(13, 2) == std::pair<long long, long long>{0, 1});
        CHECK(b->sampleAt(14, 2) == std::pair<long long, long long>{1, 1});
        CHECK(b->sampleAt(10, 0) == std::pair<long long, long long>{-3, -1});
        stamp(c, *b, dabs({{13, 2}, {14, 2}}));
        CHECK(rowText(c, 2).substr(10, 5) == U"ab ab");
    }
    SECTION("a new source starts over") {
        CloneSource src;
        src.setSource(0, 0);
        src.beginStroke(10, 2);
        REQUIRE(src.anchor);
        src.setSource(3, 1);
        CHECK_FALSE(src.anchor);
        const auto b = src.beginStroke(7, 0);
        CHECK(b->source() == std::pair{3, 1});
        CHECK(b->anchor() == std::pair{7, 0});
    }
}

TEST_CASE("clone stamp: a source overlapping the stroke copies the canvas as the stroke began") {
    Canvas c = row(U"ABCDEFGH");
    // a cell to the right of the source: each cell copies its left neighbour
    // as it was, not what the stroke just laid there
    stamp(c, CloneBrush({0, 0}, {1, 0}), dabs({{1, 0}, {6, 0}}));
    CHECK(rowText(c) == U"AABCDEFH");
    // the other way the same
    c = row(U"ABCDEFGH");
    stamp(c, CloneBrush({7, 0}, {6, 0}), dabs({{6, 0}, {1, 0}}));
    CHECK(rowText(c) == U"ACDEFGHH");
}

TEST_CASE("clone stamp: a source cell off the canvas copies nothing") {
    Canvas c = row(U"ABCDEFGH", 2);
    const Canvas before = c;
    // three cells left of the stroke: the cells nearer the left edge than
    // three read off the canvas and stay
    BrushSettings s;
    s.size = 5;
    stamp(c, CloneBrush({0, 0}, {3, 0}), dabs({{3, 0}}), {}, s);
    CHECK(rowText(c) == U"ABCABCGH");
    // a row up reads above the canvas, a row down below it: nothing changes
    c = before;
    CHECK(paintStroke(c, CloneBrush({0, 0}, {0, 1}), {}, dabs({{0, 0}, {7, 0}})).empty());
    CHECK(paintStroke(c, CloneBrush({0, 1}, {0, 0}), {}, dabs({{0, 1}, {7, 1}})).empty());
    CHECK(paintStroke(c, CloneBrush({0, 2}, {0, 0}), {}, dabs({{0, 0}, {7, 1}})).empty());
    // on the canvas it copies: the second row's blanks over the first
    stamp(c, CloneBrush({0, 1}, {0, 0}), dabs({{0, 0}, {7, 0}}));
    CHECK(rowText(c) == U"        ");
}

TEST_CASE("clone stamp: the selection and the alpha lock limit where it lands, not what it reads") {
    Canvas c = row(U"ABCDEFGH", 2);
    for (int x = 0; x < 8; ++x)
        c.at(x, 1).transparent = (x % 2 == 0);
    const Canvas before = c;

    // the selection: only (2..4, 1) may change; it reads from row 0, outside it
    StrokeOptions o;
    o.selection = shared(Mask::rect(8, 2, 2, 1, 4, 1));
    stamp(c, CloneBrush({0, 0}, {0, 1}), dabs({{0, 1}, {7, 1}}), o);
    for (int x = 0; x < 8; ++x) {
        INFO(x);
        CHECK(c.at(x, 1) == (x >= 2 && x <= 4 ? before.at(x, 0) : before.at(x, 1)));
    }

    // the alpha lock: the transparent cells stay
    c = before;
    o = {};
    o.lockTransparent = true;
    stamp(c, CloneBrush({0, 0}, {0, 1}), dabs({{0, 1}, {7, 1}}), o);
    for (int x = 0; x < 8; ++x) {
        INFO(x);
        CHECK(c.at(x, 1) == (x % 2 ? before.at(x, 0) : before.at(x, 1)));
    }

    // nor does the lock let a transparent source cell clear an opaque one
    c = before;
    stamp(c, CloneBrush({0, 1}, {0, 0}), dabs({{0, 0}, {7, 0}}), o);
    for (int x = 0; x < 8; ++x) {
        INFO(x);
        CHECK(c.at(x, 0) == (x % 2 ? before.at(x, 1) : before.at(x, 0)));
    }
}

TEST_CASE("clone stamp: under a mirror each image copies from the source's image") {
    SECTION("mirror X: the mirror image copies the mirror of the source") {
        Canvas c(10, 3);
        c.at(1, 0) = cell(U'▌', Red);
        c.at(8, 0) = cell(U'▐', Blue);  // not the mirror of (1, 0): a canvas that isn't symmetric
        StrokeOptions o;
        o.symmetry.mode = SymmetryMode::MirrorX;
        const CloneBrush b({1, 0}, {3, 2});
        CHECK(b.offset(o.symmetry, 0, 10, 3) == std::pair<long long, long long>{-2, -2});
        CHECK(b.offset(o.symmetry, 1, 10, 3) == std::pair<long long, long long>{2, -2});
        stamp(c, b, dabs({{3, 2}}), o);
        CHECK(c.at(3, 2) == cell(U'▌', Red));
        CHECK(c.at(6, 2) == cell(U'▐', Blue));  // copied as it is, not flipped
    }
    SECTION("mirror Y") {
        Canvas c(4, 6);
        c.at(0, 0) = cell(U'▀', Red);
        c.at(0, 5) = cell(U'▄', Green);
        StrokeOptions o;
        o.symmetry.mode = SymmetryMode::MirrorY;
        stamp(c, CloneBrush({0, 0}, {2, 1}), dabs({{2, 1}}), o);
        CHECK(c.at(2, 1) == cell(U'▀', Red));
        CHECK(c.at(2, 4) == cell(U'▄', Green));
    }
    SECTION("a stroke on a symmetric canvas leaves it symmetric") {
        for (const bool x : {true, false}) {
            for (unsigned seed = 1; seed <= 4; ++seed) {
                INFO("mirror " << (x ? "X" : "Y") << ", seed " << seed);
                const Axes axes = x ? Axes::X : Axes::Y;
                Canvas c = symmetric(20, 12, axes, seed);
                REQUIRE(isSymmetric(c, axes));
                StrokeOptions o;
                o.symmetry.mode = x ? SymmetryMode::MirrorX : SymmetryMode::MirrorY;
                BrushSettings s;
                s.size = int(seed) + 1;
                s.tip = seed % 2 ? Tip::Round : Tip::Square;
                const CellEdit e =
                    stamp(c, CloneBrush({int(seed) + 1, 2}, {4, 7}), dabs({{4, 7}, {12, 9}, {16, 4}}), o, s);
                CHECK_FALSE(e.empty());
                CHECK(isSymmetric(c, axes));
            }
        }
    }
    SECTION("a dab on an odd canvas's axis: both images' centres meet, and the cells off it still mirror") {
        // ABCBA, a 3-wide square at the middle copying a cell to the left:
        // the left half copies leftwards, the right half its mirror, rightwards
        Canvas c = row(U"ABCBA");
        StrokeOptions o;
        o.symmetry.mode = SymmetryMode::MirrorX;
        BrushSettings s;
        s.size = 3;
        s.tip = Tip::Square;
        stamp(c, CloneBrush({1, 0}, {2, 0}), dabs({{2, 0}}), o, s);
        CHECK(c.at(1, 0) == c.at(3, 0));
        CHECK(rowText(c) == U"AABAA");
        // mirror Y the same, down a column
        Canvas d(1, 5);
        for (int y = 0; y < 5; ++y)
            d.at(0, y) = cell(U"ABCBA"[y], Red);
        o.symmetry.mode = SymmetryMode::MirrorY;
        stamp(d, CloneBrush({0, 1}, {0, 2}), dabs({{0, 2}}), o, s);
        CHECK(d.at(0, 1) == d.at(0, 3));
        CHECK(d.at(0, 1).ch == U'A');
    }
    SECTION("odd canvases stay symmetric under every mirror, dabs on and beside the axes") {
        for (const auto& [w, h] : {std::pair{81, 41}, std::pair{9, 5}})
            for (const Axes axes : {Axes::X, Axes::Y, Axes::XY})
                for (unsigned seed = 1; seed <= 6; ++seed) {
                    INFO(w << "x" << h << ", axes " << int(axes) << ", seed " << seed);
                    Canvas c = symmetric(w, h, axes, seed * 7 + unsigned(w));
                    REQUIRE(isSymmetric(c, axes));
                    StrokeOptions o;
                    o.symmetry.mode = axes == Axes::X   ? SymmetryMode::MirrorX
                                      : axes == Axes::Y ? SymmetryMode::MirrorY
                                                        : SymmetryMode::MirrorXY;
                    BrushSettings s;
                    s.size = int(seed);
                    s.tip = seed % 2 ? Tip::Round : Tip::Square;
                    const int cx = w / 2, cy = h / 2;
                    const std::pair<int, int> source{(cx + 2 * int(seed)) % w, (cy + int(seed)) % h};
                    // on the middle, beside it and on each axis
                    const CellEdit e = stamp(c, CloneBrush(source, {cx, cy}),
                                             dabs({{cx, cy}, {cx + 1, cy}, {cx - 1, cy + 1}, {cx, cy - 1}}), o, s);
                    CHECK_FALSE(e.empty());
                    CHECK(isSymmetric(c, axes));
                    // a stroke starting on an axis, on a fresh canvas (the stroke
                    // before may have left a half block on an axis as it copied it)
                    c = symmetric(w, h, axes, seed * 13 + unsigned(h));
                    REQUIRE(isSymmetric(c, axes));
                    CHECK_FALSE(stamp(c, CloneBrush({source.second % w, source.first % h}, {cx, 1}),
                                      dabs({{cx, 1}, {1, cy}}), o, s)
                                    .empty());
                    CHECK(isSymmetric(c, axes));
                }
    }
    SECTION("mirror XY: four images") {
        Canvas c(8, 6);
        c.at(0, 0) = cell(U'1', Red);
        c.at(7, 0) = cell(U'2', Red);
        c.at(0, 5) = cell(U'3', Red);
        c.at(7, 5) = cell(U'4', Red);
        StrokeOptions o;
        o.symmetry.mode = SymmetryMode::MirrorXY;
        stamp(c, CloneBrush({0, 0}, {2, 1}), dabs({{2, 1}}), o);
        CHECK(c.at(2, 1).ch == U'1');
        CHECK(c.at(5, 1).ch == U'2');
        CHECK(c.at(2, 4).ch == U'3');
        CHECK(c.at(5, 4).ch == U'4');
    }
    SECTION("radial, a half turn: the turned image copies the turned source") {
        Canvas c(9, 5);
        c.at(0, 0) = cell(U'1', Red);
        c.at(8, 4) = cell(U'2', Red);
        StrokeOptions o;
        o.symmetry.mode = SymmetryMode::Radial;
        o.symmetry.folds = 2;
        stamp(c, CloneBrush({0, 0}, {2, 1}), dabs({{2, 1}}), o);
        CHECK(c.at(2, 1).ch == U'1');
        CHECK(c.at(6, 3).ch == U'2');
    }
}

TEST_CASE("clone stamp: a stroke is one edit that reverts whole") {
    Canvas c = row(U"ABCDEFGH", 3);
    const Canvas before = c;
    CloneSource src;
    src.setSource(0, 0);
    auto b = src.beginStroke(0, 2);
    const CellEdit e = stamp(c, *b, dabs({{0, 2}, {7, 2}, {7, 1}}));
    CHECK(rowText(c, 2) == U"ABCDEFGH");
    CHECK(c.at(7, 1) == before.at(7, 1));  // (7, 1) copies (7, -1): off the canvas
    CHECK(e.changes.size() == 8);
    e.revert(c);
    CHECK(c == before);
}
