// Glyph spray (PLAN §10, "Detail"): stars, haze and smoke, each a seeded
// field over the canvas that the brush uncovers.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/spray.h"
#include "blockwright/cp437.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace bw;

namespace {

using Point = std::pair<int, int>;

Ramp steel() { return *findRamp("steel"); }  // k K w W

const Color kInk = Color::indexed(15);

std::map<Point, Cell> written(const CellEdit& e) {
    std::map<Point, Cell> out;
    for (const CellChange& c : e.changes)
        out[{c.x, c.y}] = c.after;
    return out;
}

Canvas applied(const Canvas& c, const CellEdit& e) {
    Canvas out = c;
    e.apply(out);
    return out;
}

CellEdit drag(const Canvas& c, const SprayBrush& b, std::vector<Dab> dabs, int size, const StrokeOptions& o = {}) {
    return paintStroke(c, b, BrushSettings{size, Tip::Square}, std::move(dabs), o);
}

CellEdit everywhere(const Canvas& c, const SprayBrush& b, const StrokeOptions& o = {}) {
    return paintRegion(c, b, {}, o);
}

Canvas field(const Ramp& r, int w, int h, auto rung) {
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c.at(x, y) = rungCell(r, rung(x, y));
    return c;
}

std::string rungMap(const Canvas& c, const Ramp& r) {
    std::string s;
    for (int y = 0; y < c.height(); ++y) {
        for (int x = 0; x < c.width(); ++x) {
            const auto v = rungOf(r, c.at(x, y));
            s += v ? char(v < 10 ? '0' + *v : 'a' + *v - 10) : '.';
        }
        s += '\n';
    }
    return s;
}

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

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& m : v)
        s += m + "\n";
    return s;
}

// A start for the lint sweeps: flat, a gradient one of four ways at one to
// five cells a rung, or a gradient with cells nudged a rung at random.
Canvas startCanvas(int kind, const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const int top = r.topRung();
    if (kind == 0) {
        const int v = pick(0, top);
        return field(r, w, h, [&](int, int) { return v; });
    }
    const int way = pick(0, 3), every = pick(1, 5), from = pick(0, top / 2);
    Canvas c = field(r, w, h, [&](int x, int y) {
        const int along = way == 0 ? x : way == 1 ? w - 1 - x : way == 2 ? y : h - 1 - y;
        return std::clamp(from + along / every, 0, top);
    });
    if (kind == 2)
        for (int n = 0; n < w * h / 12; ++n) {
            const int x = pick(0, w - 1), y = pick(0, h - 1);
            const int v = std::clamp(*rungOf(r, c.at(x, y)) + (pick(0, 1) ? 1 : -1), 0, top);
            c.at(x, y) = rungCell(r, v);
        }
    return c;
}

int stars(const Canvas& c) {
    int n = 0;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            n += isSprayStar(c.at(x, y).ch);
    return n;
}

} // namespace

TEST_CASE("every star glyph is in CP437", "[spray][brush]") {
    for (const char32_t ch : {U'.', U'·', U'∙', U'+', U'*', U'☼'}) {
        CHECK(isSprayStar(ch));
        CHECK(unicodeToCp437(ch).has_value());
    }
    CHECK_FALSE(isSprayStar(U' '));
    CHECK_FALSE(isSprayStar(U'░'));
}

TEST_CASE("a spray is one field: the seed fixes it, another seed moves it", "[spray][brush]") {
    const Canvas c = field(steel(), 60, 30, [](int x, int) { return x / 8; });
    for (const SprayKind kind : {SprayKind::Stars, SprayKind::Haze, SprayKind::Smoke}) {
        INFO(int(kind));
        const SprayBrush a(steel(), {kind, 40, 7}, kInk), b(steel(), {kind, 40, 8}, kInk);
        const Canvas first = applied(c, everywhere(c, a));
        CHECK(applied(c, everywhere(c, a)) == first);
        CHECK_FALSE(applied(c, everywhere(c, b)) == first);
        CHECK_FALSE(first == c);
    }
    // the free functions name the field the brush paints
    const SprayOptions haze{SprayKind::Haze, 50, 3};
    Canvas blank(40, 20);
    const Canvas hazy = applied(blank, everywhere(blank, SprayBrush(steel(), haze, kInk)));
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x) {
            INFO(x << "," << y);
            CHECK(rungOf(steel(), hazy.at(x, y)) == sprayLift(haze, x, y, 40, 20));
        }
    const SprayOptions sky{SprayKind::Stars, 100, 3};
    const Canvas starry = applied(blank, everywhere(blank, SprayBrush(steel(), sky, kInk)));
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            if (const auto s = sprayStar(sky, x, y, 40, 20))
                CHECK(starry.at(x, y).ch == *s);
            else
                CHECK(starry.at(x, y).ch == U' ');
}

TEST_CASE("a spray doesn't pile up: strokes over strokes take cells toward the one field, never past it",
          "[spray][brush]") {
    const Ramp r = steel();
    for (const SprayKind kind : {SprayKind::Stars, SprayKind::Haze, SprayKind::Smoke}) {
        INFO(int(kind));
        const SprayBrush b(r, {kind, 45, 12}, kInk);
        const Canvas c(50, 24);
        const Canvas once = applied(c, everywhere(c, b));
        // over its own field it changes nothing
        CHECK(everywhere(once, b).empty());
        CHECK(drag(once, b, {Dab{5, 5}, Dab{40, 18}}, 9).empty());
        // overlapping drags, one after another: stars lay the same field
        // exactly here; haze and smoke never go past it, and end short of it
        // only where the guard held a cell back (at most two cells here)
        Canvas strokes = c;
        for (int y = 0; y < 24; y += 3)
            strokes = applied(strokes, drag(strokes, b, {Dab{0, y}, Dab{49, y}}, 5));
        INFO(rungMap(once, r) << "strokes\n" << rungMap(strokes, r));
        if (kind == SprayKind::Stars) {
            CHECK(strokes == once);
            continue;
        }
        int short_ = 0;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 50; ++x) {
                INFO(x << "," << y);
                const int want = *rungOf(r, once.at(x, y)), got = *rungOf(r, strokes.at(x, y));
                CHECK(got <= want);
                short_ += got < want;
            }
        CHECK(short_ <= 2);
    }
}

TEST_CASE("density sets how much of the field a spray lays", "[spray][brush]") {
    const Canvas c(320, 160);
    // stars: one candidate in each 8 x 4 block, a star with the density's chance
    for (const int d : {10, 30, 60}) {
        const int n = stars(applied(c, everywhere(c, SprayBrush(steel(), {SprayKind::Stars, d, 5}, kInk))));
        const double want = 40.0 * 40.0 * d / 100.0;
        INFO(d << ": " << n << " stars, about " << want);
        CHECK(std::abs(n - want) < want * 0.2);
    }
    // haze and smoke: the share of the canvas they cover
    for (const SprayKind kind : {SprayKind::Haze, SprayKind::Smoke}) {
        double last = 0.0;
        for (const int d : {15, 40, 70}) {
            int covered = 0;
            for (const int seed : {1, 2, 3})
                for (int y = 0; y < 160; ++y)
                    for (int x = 0; x < 320; ++x)
                        covered += sprayLift({kind, d, seed}, x, y, 320, 160) > 0;
            const double share = covered / (3.0 * 320 * 160);
            INFO(int(kind) << " at " << d << "%: " << share);
            CHECK(std::abs(share - d / 100.0) < 0.1);
            CHECK(share > last);
            last = share;
        }
    }
}

TEST_CASE("stars keep off shapes and apart from each other", "[spray][brush]") {
    Canvas c(64, 32);
    // a red block and a dithered band
    for (int y = 10; y < 20; ++y)
        for (int x = 20; x < 36; ++x)
            c.at(x, y) = Cell{U' ', Color::indexed(7), Color::indexed(4)};
    for (int x = 0; x < 64; ++x)
        c.at(x, 28) = Cell{U'▒', Color::indexed(8), Color::indexed(0)};
    const auto cells = written(everywhere(c, SprayBrush(steel(), {SprayKind::Stars, 100, 9}, kInk)));
    REQUIRE(cells.size() > 20);
    for (const auto& [p, cell] : cells) {
        INFO(p.first << "," << p.second);
        CHECK(isSprayStar(cell.ch));
        CHECK(cell.fg == kInk);
        // four columns and two rows of open ground all round: off the
        // block, or well inside it on its colour
        if (cell.bg == Color::indexed(4)) {
            CHECK((p.first >= 24 && p.first <= 31 && p.second >= 12 && p.second <= 17));
        } else {
            CHECK(cell.bg == Color::indexed(0));
            CHECK((p.first < 16 || p.first > 39 || p.second < 8 || p.second > 21));
        }
        CHECK(std::abs(p.second - 28) > 2);
        for (const auto& [q, other] : cells)
            if (q != p)
                CHECK((std::abs(q.first - p.first) >= 5 || std::abs(q.second - p.second) >= 3));
    }
    // on the block's own colour a star takes it as its ground
    const auto inside =
        written(drag(c, SprayBrush(steel(), {SprayKind::Stars, 100, 9}, kInk), {Dab{27, 14}}, 3));
    for (const auto& [p, cell] : inside)
        CHECK(cell.bg == Color::indexed(4));
    // not on ground of the ink's own colour
    Canvas white(30, 12);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 30; ++x)
            white.at(x, y).bg = Color::indexed(7);
    CHECK(everywhere(white, SprayBrush(steel(), {SprayKind::Stars, 100, 9}, Color::indexed(7))).empty());
}

TEST_CASE("the right button takes stars off", "[spray][brush]") {
    const Canvas c(40, 20);
    const SprayOptions o{SprayKind::Stars, 100, 4};
    const Canvas sky = applied(c, everywhere(c, SprayBrush(steel(), o, kInk)));
    REQUIRE(stars(sky) > 10);
    const Canvas wiped = applied(sky, everywhere(sky, SprayBrush(steel(), o, kInk, ShadeDirection::Darken)));
    CHECK(stars(wiped) == 0);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            CHECK(wiped.at(x, y).bg == c.at(x, y).bg);
    // nothing else is touched
    CHECK(everywhere(c, SprayBrush(steel(), o, kInk, ShadeDirection::Darken)).empty());
}

TEST_CASE("haze lightens toward the field, and darkens with the right button", "[spray][brush]") {
    // Up (down) toward the field's rung over the colour's solid rung, never
    // past it, and short of it only where the guard holds a cell back; on
    // this mid-ramp ground more than seven in ten lifted cells reach it.
    const Ramp r = steel();
    const Canvas mid = field(r, 60, 30, [](int, int) { return 4; });
    const SprayOptions haze{SprayKind::Haze, 50, 6};
    const Canvas up = applied(mid, everywhere(mid, SprayBrush(r, haze, kInk)));
    const Canvas down = applied(mid, everywhere(mid, SprayBrush(r, haze, kInk, ShadeDirection::Darken)));
    INFO("up\n" << rungMap(up, r) << "down\n" << rungMap(down, r));
    int lifted = 0, reachedUp = 0, reachedDown = 0;
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 60; ++x) {
            const int lift = sprayLift(haze, x, y, 60, 30);
            INFO(x << "," << y);
            const int u = rungOf(r, up.at(x, y)).value_or(-1), d = rungOf(r, down.at(x, y)).value_or(-1);
            CHECK(u >= 4);
            CHECK(u <= 4 + lift);
            CHECK(d <= 4);
            CHECK(d >= 4 - lift);
            lifted += lift > 0;
            reachedUp += lift > 0 && u == 4 + lift;
            reachedDown += lift > 0 && d == 4 - lift;
        }
    CHECK(lifted > 300);
    CHECK(reachedUp * 10 > lifted * 7);
    CHECK(reachedDown * 10 > lifted * 7);
    CHECK(added(mid, up, r).empty());
    CHECK(added(mid, down, r).empty());
    // the top colour can't be lightened, black can't be darkened
    const Canvas top = field(r, 30, 10, [&](int, int) { return r.topRung(); });
    CHECK(everywhere(top, SprayBrush(r, haze, kInk)).empty());
    const Canvas black(30, 10);
    CHECK(everywhere(black, SprayBrush(r, haze, kInk, ShadeDirection::Darken)).empty());
}

TEST_CASE("a spray keeps to the selection and the alpha lock", "[spray][brush]") {
    for (const SprayKind kind : {SprayKind::Stars, SprayKind::Haze}) {
        INFO(int(kind));
        Canvas c(40, 20);
        const SprayBrush b(steel(), {kind, 100, 2}, kInk);
        StrokeOptions sel;
        sel.selection = std::make_shared<Mask>(Mask::rect(40, 20, 5, 4, 30, 15));
        const auto in = written(everywhere(c, b, sel));
        REQUIRE_FALSE(in.empty());
        for (const auto& [p, cell] : in) {
            CHECK(p.first >= 5);
            CHECK(p.first <= 30);
            CHECK(p.second >= 4);
            CHECK(p.second <= 15);
        }
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 40; ++x)
                c.at(x, y).transparent = x < 20;
        StrokeOptions lock;
        lock.lockTransparent = true;
        const auto locked = written(everywhere(c, b, lock));
        REQUIRE_FALSE(locked.empty());
        for (const auto& [p, cell] : locked)
            CHECK(p.first >= 20);
    }
}

TEST_CASE("with iCE off a spray writes no bright background", "[spray][brush][ice]") {
    const Ramp r = *findRamp("fire");
    StrokeOptions o;
    o.ice = false;
    const Canvas c = field(r, 60, 20, [&](int x, int) { return std::min(x / 6, r.topRung()); });
    for (const SprayKind kind : {SprayKind::Haze, SprayKind::Smoke})
        for (const ShadeDirection dir : {ShadeDirection::Lighten, ShadeDirection::Darken}) {
            const CellEdit e = everywhere(c, SprayBrush(r, {kind, 80, 3}, kInk, dir), o);
            for (const CellChange& ch : e.changes)
                CHECK(ch.after.bg.paletteIndex() < 8);
        }
    // stars on a bright ground: only with iCE on
    Canvas bright(30, 12);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 30; ++x)
            bright.at(x, y).bg = Color::indexed(9);
    CHECK(everywhere(bright, SprayBrush(r, {SprayKind::Stars, 100, 3}, kInk), o).empty());
    CHECK_FALSE(everywhere(bright, SprayBrush(r, {SprayKind::Stars, 100, 3}, kInk)).empty());
}

TEST_CASE("on a flat canvas a mirrored spray paints the exact mirror image", "[spray][brush][symmetry]") {
    const Ramp r = steel();
    for (const SymmetryMode mode : {SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY})
        for (const SprayKind kind : {SprayKind::Stars, SprayKind::Haze, SprayKind::Smoke})
            for (const int w : {40, 41}) {
                const int h = 21 - (w & 1);
                INFO(int(mode) << " " << int(kind) << " " << w << "x" << h);
                const Canvas c = field(r, w, h, [](int, int) { return 4; });
                StrokeOptions o;
                o.symmetry.mode = mode;
                const auto cells = written(drag(c, SprayBrush(r, {kind, kind == SprayKind::Stars ? 100 : 60, 5}, kInk),
                                                {Dab{6, 5}, Dab{14, 9}}, 11, o));
                REQUIRE_FALSE(cells.empty());
                const bool mx = mode != SymmetryMode::MirrorY, my = mode != SymmetryMode::MirrorX;
                for (const auto& [p, cell] : cells) {
                    INFO(p.first << "," << p.second);
                    const Point q{mx ? w - 1 - p.first : p.first, my ? h - 1 - p.second : p.second};
                    REQUIRE(cells.count(q));
                    CHECK(cells.at(q) == cell);
                    if (isSprayStar(cell.ch) && q != p)
                        CHECK((std::abs(q.first - p.first) >= 5 || std::abs(q.second - p.second) >= 3));
                }
            }
}

TEST_CASE("stars, haze and smoke add no ladder lint finding", "[spray][brush][lint]") {
    std::mt19937 rng(1234);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    int painted = 0, runs = 0;
    for (const char* name : {"steel", "fire", "ice", "toxic", "sunset"}) {
        const Ramp r = *findRamp(name);
        for (const SymmetryMode mode :
             {SymmetryMode::None, SymmetryMode::MirrorX, SymmetryMode::MirrorY, SymmetryMode::MirrorXY})
            for (const bool ice : {true, false})
                for (const SprayKind kind : {SprayKind::Stars, SprayKind::Haze, SprayKind::Smoke})
                    for (const ShadeDirection dir : {ShadeDirection::Lighten, ShadeDirection::Darken})
                        for (int round = 0; round < 2; ++round) {
                            const int w = pick(24, 48), h = pick(12, 24);
                            Canvas start = startCanvas(pick(0, 2), r, w, h, rng);
                            if (kind == SprayKind::Stars)
                                // a few stars to take off, and half-block pixels for the noise
                                for (int n = pick(0, w * h / 20); n > 0; --n) {
                                    Cell& cell = start.at(pick(0, w - 1), pick(0, h - 1));
                                    cell.fg = Color::indexed(std::uint8_t(pick(0, 15)));
                                    cell.ch = pick(0, 1) ? U'▀' : U'*';
                                }
                            StrokeOptions o;
                            o.ice = ice;
                            o.symmetry.mode = mode;
                            const SprayBrush b(r, {kind, pick(20, 90), pick(0, 999)}, kInk, dir);
                            std::vector<Dab> dabs;
                            for (int n = pick(2, 5); n > 0; --n)
                                dabs.push_back(Dab{pick(0, w - 1), pick(0, h - 1)});
                            const Canvas after = applied(start, drag(start, b, dabs, pick(3, 11), o));
                            const auto more = added(start, after, r);
                            INFO(name << " mode " << int(mode) << " ice " << ice << " kind " << int(kind) << " dir "
                                      << int(dir) << "\n"
                                      << rungMap(start, r) << "after\n"
                                      << rungMap(after, r) << joined(more));
                            CHECK(more.empty());
                            if (kind != SprayKind::Stars) {
                                painted += !(after == start);
                                ++runs;
                            }
                        }
    }
    // in this sweep more than half the haze and smoke runs paint
    INFO(painted << " of " << runs);
    CHECK(painted * 2 > runs);
}


TEST_CASE("stars add no lint finding: not even noise, read over the whole canvas", "[spray][brush][lint]") {
    // Codex round 1, finding 1: lonely ▀ pixels on a flat ground; stars on
    // the ground between them take counted cells out of the noise's
    // denominator, and the noise goes over the limit.
    for (const bool ice : {true, false}) {
        INFO("ice " << ice);
        Canvas c(40, 20);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 40; ++x)
                c.at(x, y) = Cell{U' ', Color::indexed(7), Color::indexed(7)};
        for (const int x : {2, 5, 8, 11, 14, 17, 20})
            for (const int y : {3, 7, 11, 15})
                c.at(x, y) = Cell{U'▀', Color::indexed(4), Color::indexed(7)};
        StrokeOptions o;
        o.ice = ice;
        const SprayBrush b(steel(), {SprayKind::Stars, 100, 1}, kInk);
        const Canvas after = applied(c, everywhere(c, b, o));
        CHECK(added(c, after, steel()).empty());
        // and some stars land here
        CHECK(stars(after) > 0);
    }
}

TEST_CASE("a second stroke the same as the first moves only held cells, toward the field", "[spray][brush]") {
    // Codex round 1, finding 2: cells the first stroke held back went up
    // when the same stroke came again.
    {
        const Ramp r = steel();
        const SprayBrush haze(r, {SprayKind::Haze, 50, 12}, kInk);
        const Canvas blank(20, 10);
        const std::vector<Dab> dabs{Dab{0, 0}, Dab{9, 0}};
        const Canvas once = applied(blank, drag(blank, haze, dabs, 1));
        const CellEdit again = drag(once, haze, dabs, 1);
        INFO(rungMap(once, r) << "again\n" << rungMap(applied(once, again), r));
        CHECK(again.empty());
    }

    // Over random grounds, strokes, kinds and symmetries a repeat of haze or
    // smoke changes only cells the first stroke left short of the field,
    // each toward it and never past it, so nothing piles up. In this sweep a
    // repeat of stars changes nothing, and fewer than one in twenty haze or
    // smoke repeats changes anything (the guard judges each stroke against
    // its own start, so a repeat can take a cell the first had to hold
    // back).
    std::mt19937 rng(77);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    int changed = 0, repeated = 0, runs = 0;
    for (const char* name : {"steel", "fire", "ice"})
        for (const SymmetryMode mode : {SymmetryMode::None, SymmetryMode::MirrorX, SymmetryMode::MirrorXY})
            for (const SprayKind kind : {SprayKind::Stars, SprayKind::Haze, SprayKind::Smoke})
                for (const ShadeDirection dir : {ShadeDirection::Lighten, ShadeDirection::Darken})
                    for (int round = 0; round < 3; ++round) {
                        const Ramp ramp = *findRamp(name);
                        const int w = pick(20, 44), h = pick(10, 22);
                        Canvas start = startCanvas(pick(0, 2), ramp, w, h, rng);
                        if (kind == SprayKind::Stars)
                            // runs of stars side by side on the ramp's colours, to
                            // take off: a wipe can wait on its neighbour's
                            for (int n = pick(0, w * h / 30); n > 0; --n) {
                                const int x0 = pick(0, w - 1), y0 = pick(0, h - 1), across = pick(0, 1);
                                const auto bg = Color::indexed(ramp.colours[size_t(pick(0, int(ramp.colours.size()) - 1))]);
                                for (int k = pick(1, 4); k > 0; --k) {
                                    const int x = std::min(x0 + (across ? k : 0), w - 1);
                                    const int y = std::min(y0 + (across ? 0 : k), h - 1);
                                    start.at(x, y) = Cell{pick(0, 1) ? U'*' : U'.', kInk, bg};
                                }
                            }
                        StrokeOptions o;
                        o.ice = pick(0, 1);
                        o.symmetry.mode = mode;
                        const SprayOptions so{kind, pick(20, 90), pick(0, 999)};
                        const SprayBrush b(ramp, so, kInk, dir);
                        std::vector<Dab> path;
                        for (int n = pick(2, 5); n > 0; --n)
                            path.push_back(Dab{pick(0, w - 1), pick(0, h - 1)});
                        const int size = pick(1, 9);
                        const Canvas first = applied(start, drag(start, b, path, size, o));
                        const CellEdit second = drag(first, b, path, size, o);
                        INFO(name << " mode " << int(mode) << " kind " << int(kind) << " dir " << int(dir) << "\n"
                                  << rungMap(start, ramp) << "first\n"
                                  << rungMap(first, ramp) << "second\n"
                                  << rungMap(applied(first, second), ramp));
                        if (kind == SprayKind::Stars) {
                            CHECK(second.empty());
                            continue;
                        }
                        changed += !(first == start);
                        repeated += !second.empty();
                        ++runs;
                        const bool lighten = dir == ShadeDirection::Lighten;
                        const bool foldX = mode != SymmetryMode::None;
                        const bool foldY = mode == SymmetryMode::MirrorXY;
                        const auto shown = [&](const Cell& cell) {
                            return rungOf(ramp, o.ice ? cell : iceCell(cell, false));
                        };
                        for (const CellChange& ch : second.changes) {
                            INFO("cell " << ch.x << "," << ch.y);
                            const auto r0 = shown(start.at(ch.x, ch.y));
                            const auto r1 = shown(first.at(ch.x, ch.y));
                            const auto r2 = shown(ch.after);
                            REQUIRE(r0);
                            REQUIRE(r1);
                            REQUIRE(r2);
                            const int base = lighten ? *r0 / kRungsPerStep * kRungsPerStep
                                                     : (*r0 + kRungsPerStep - 1) / kRungsPerStep * kRungsPerStep;
                            const int lift = sprayLift(so, ch.x, ch.y, w, h, foldX, foldY);
                            const int target = lighten ? base + lift : base - lift;
                            CHECK(*r1 != target);
                            CHECK((lighten ? *r1 < *r2 && *r2 <= target : *r1 > *r2 && *r2 >= target));
                        }
                    }
    INFO(changed << " of " << runs << " changed, " << repeated << " repeats changed");
    CHECK(changed * 2 > runs);
    CHECK(repeated * 20 < runs);
}

TEST_CASE("a spray dab's work is its neighbourhood's, not the stroke's", "[spray][brush][perf]") {
    // Codex round 1, finding 3: every dab judged again every cell the
    // stroke had held back, however far behind. Here (100% on a blank
    // canvas, size-9 dabs down its middle) the guard holds back every cell
    // the stroke reaches, so the held cells grow with the stroke.
    const Ramp r = steel();
    const Canvas c(40, 700);
    const SprayBrush haze(r, {SprayKind::Haze, 100, 3}, kInk);
    const auto lastDab = [&](int rows) {
        Stroke s(c, haze, BrushSettings{9, Tip::Square});
        for (int y = 0; y <= rows; y += 4)
            s.addDab(Dab{20, y});
        REQUIRE(s.preview() == c);
        const long long before = lint::cellsRead();
        s.addDab(Dab{20, rows});
        return lint::cellsRead() - before;
    };
    const long long shortStroke = lastDab(60), longStroke = lastDab(600);
    INFO(shortStroke << " reads after 60 rows, " << longStroke << " after 600");
    CHECK(longStroke <= shortStroke * 2);
}

TEST_CASE("a star wipe a neighbour's wipe frees is taken in the same dab, so a repeat changes nothing",
          "[spray][brush]") {
    // Codex round 2, finding 1: wiping (1, 1) first would leave a rung-4
    // speck among rung-3 neighbours; once (2, 1) is wiped it has a rung-4
    // neighbour, and the same stroke again took it.
    const Ramp r = steel();
    Canvas c = field(r, 3, 3, [](int, int) { return 3; });
    for (const int x : {1, 2})
        c.at(x, 1) = Cell{U'*', Color::indexed(15), Color::indexed(8)};
    const SprayBrush wipe(r, {SprayKind::Stars, 100, 1}, kInk, ShadeDirection::Darken);
    const Canvas once = applied(c, drag(c, wipe, {Dab{1, 1}}, 3));
    INFO(rungMap(once, r));
    CHECK(stars(once) == 0);
    CHECK(added(c, once, r).empty());
    CHECK(drag(once, wipe, {Dab{1, 1}}, 3).empty());
}

TEST_CASE("stars compete for the canvas's noise budget, and earlier stars win it", "[spray][brush][lint]") {
    // Codex round 2, finding 2: the noise fixture's ten open-ground stars in
    // columns 25-39, five of which fit under the noise limit. A serpentine
    // drag over them and the same drag reversed land different fives.
    Canvas c(40, 20);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 40; ++x)
            c.at(x, y) = Cell{U' ', Color::indexed(7), Color::indexed(7)};
    const Canvas flat = c;
    for (const int x : {2, 5, 8, 11, 14, 17, 20})
        for (const int y : {3, 7, 11, 15})
            c.at(x, y) = Cell{U'▀', Color::indexed(4), Color::indexed(7)};
    const SprayBrush b(steel(), {SprayKind::Stars, 100, 1}, kInk);
    std::vector<Dab> path;
    for (int y = 0; y < 20; ++y)
        for (int i = 0; i < 15; ++i)
            path.push_back(Dab{y % 2 ? 39 - i : 25 + i, y});
    std::vector<Dab> back(path.rbegin(), path.rend());
    // the stars a dab would lay by itself, in the order a path meets them
    const auto eligible = [&](const std::vector<Dab>& p) {
        std::vector<Point> out;
        for (const Dab& d : p)
            if (!drag(c, b, {d}, 1).empty())
                out.push_back({d.x, d.y});
        return out;
    };
    const auto landed = [&](const Canvas& base, const std::vector<Dab>& p) {
        std::vector<Point> out;
        const Canvas after = applied(base, drag(base, b, p, 1));
        CHECK(added(base, after, steel()).empty());
        for (const Dab& d : p)
            if (isSprayStar(after.at(d.x, d.y).ch))
                out.push_back({d.x, d.y});
        return out;
    };
    const auto ahead = eligible(path), behind = eligible(back);
    REQUIRE(ahead.size() == 10);
    const auto forward = landed(c, path), reversed = landed(c, back);
    // the budget binds: each way lands the first five it meets
    CHECK(forward == std::vector<Point>(ahead.begin(), ahead.begin() + 5));
    CHECK(reversed == std::vector<Point>(behind.begin(), behind.begin() + 5));
    // with no budget to compete for, the order makes no difference
    const auto all = landed(flat, path);
    CHECK(all.size() >= 10);
    auto again = landed(flat, back);
    std::reverse(again.begin(), again.end());
    CHECK(all == again);
}

TEST_CASE("a spray dab retries only the held cells near it, never a chain of them", "[spray][brush][perf]") {
    // Codex round 2, finding 3: a dab that lands a cell brought the held
    // cells within four of it into the round, and those the cells within
    // four of them, so one dab could take a whole run the stroke had held.
    // Here, on a blank canvas, a stroke that has reached only ░ cells holds
    // all but at most three of them (four shade cells of one glyph are a
    // finding), and a dab that lands a block of ▒ lets some of those in its
    // reach go.
    const Ramp r = steel();
    const int W = 240, H = 120;
    const Canvas c(W, H);
    const SprayOptions o{SprayKind::Haze, 60, 3};
    const SprayBrush haze(r, o, kInk);
    std::vector<int> lift(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            lift[size_t(y) * W + x] = sprayLift(o, x, y, W, H);
    const auto at = [&](int x, int y) {
        return x < 0 || y < 0 || x >= W || y >= H ? -1 : lift[size_t(y) * W + x];
    };
    // the ░ cells in reach (four cells) of each other from `first`, nearest
    // first
    const auto reach = [&](Point first) {
        std::vector<Point> run{first};
        std::vector<char> seen(lift.size());
        seen[size_t(first.second) * W + first.first] = 1;
        for (size_t i = 0; i < run.size() && run.size() < 600; ++i)
            for (int dy = -4; dy <= 4; ++dy)
                for (int dx = -4; dx <= 4; ++dx) {
                    const int x = run[i].first + dx, y = run[i].second + dy;
                    if (at(x, y) == 1 && !seen[size_t(y) * W + x] && run.size() < 600) {
                        seen[size_t(y) * W + x] = 1;
                        run.push_back({x, y});
                    }
                }
        return run;
    };
    // a ▒ cell the last dabs land, inside a block of ▒, and the run of ░
    // from a cell beside it
    Point centre{-1, -1};
    std::vector<Point> run;
    for (int y = 6; y < H - 6 && run.size() < 600; ++y)
        for (int x = 6; x < W - 6 && run.size() < 600; ++x) {
            bool solid = true;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 2; ++dx)
                    solid = solid && at(x + dx, y + dy) == 2;
            for (int dy = -3; dy <= 3 && solid && run.size() < 600; ++dy)
                for (int dx = -3; dx <= 3 && run.size() < 600; ++dx)
                    if (at(x + dx, y + dy) == 1) {
                        centre = {x, y};
                        run = reach({x + dx, y + dy});
                    }
        }
    REQUIRE(run.size() == 600);
    // ░ cells only: held, but for three at most. A ▒ by itself would be a
    // speck, so the first ▒ dab is held too; the second, beside it, lands.
    const Point second{centre.first + 1, centre.second};
    const auto lastDab = [&](size_t n) {
        Stroke s(c, haze, BrushSettings{1, Tip::Square});
        for (size_t i = 0; i < n; ++i)
            s.addDab(Dab{run[i].first, run[i].second}, false);
        s.addDab(Dab{centre.first, centre.second}, false);
        // one glyph needs four shade cells, so up to three ░ land here
        const Canvas held = s.preview();
        int landed = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                landed += !(held.at(x, y) == c.at(x, y));
        REQUIRE(landed <= 3);
        const long long before = lint::cellsRead();
        s.addDab(Dab{second.first, second.second}, false);
        const long long reads = lint::cellsRead() - before;
        const Canvas after = s.preview();
        CHECK_FALSE(after.at(second.first, second.second) == c.at(second.first, second.second));
        // the held ░ in reach of it are tried again, and here some land
        int freed = 0;
        for (size_t i = 0; i < n; ++i) {
            const auto [x, y] = run[i];
            if (std::abs(x - second.first) <= 4 && std::abs(y - second.second) <= 4)
                freed += held.at(x, y) == c.at(x, y) && !(after.at(x, y) == c.at(x, y));
        }
        CHECK(freed > 0);
        // nothing further than four cells off the dab moves
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (std::abs(x - second.first) > 4 || std::abs(y - second.second) > 4) {
                    INFO(n << " held: " << x << "," << y);
                    CHECK(after.at(x, y) == held.at(x, y));
                }
        return reads;
    };
    const long long shortRun = lastDab(60), longRun = lastDab(600);
    INFO(shortRun << " reads after 60 held cells, " << longRun << " after 600");
    CHECK(longRun * 4 <= shortRun * 5);
}

TEST_CASE("a round dab retries no held cell more than four cells from its footprint", "[spray][brush][perf]") {
    // Codex round 3, finding 1: a dab retried the held cells within four
    // cells of its footprint's bounding box, and a round tip's box reaches
    // further than the tip. The selection is the one cell (20, 20): a
    // one-cell dab there is held (a lone lift is a speck), and a size-64
    // round dab at (52, 36) covers its box's corner but not (20, 20) or any
    // cell within four of it, and reaches no other selected cell.
    const Ramp r = steel();
    const Canvas c(100, 100);
    const SprayBrush haze(r, {SprayKind::Haze, 100, 3}, kInk);
    StrokeOptions o;
    auto sel = std::make_shared<Mask>(100, 100);
    sel->set(20, 20);
    o.selection = sel;
    const auto round = footprint(52, 36, 64, Tip::Round, 100, 100);
    int x0 = 100, y0 = 100;
    for (const auto& [x, y] : round) {
        INFO(x << "," << y);
        REQUIRE((std::abs(x - 20) > 4 || std::abs(y - 20) > 4));
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
    }
    REQUIRE(x0 - 4 <= 20);
    REQUIRE(y0 - 4 <= 20);
    Stroke s(c, haze, BrushSettings{64, Tip::Round, 1.0, PressureTo::Size}, o);
    s.addDab(Dab{20, 20, 1.0 / 64}, false);
    REQUIRE(s.preview().at(20, 20) == c.at(20, 20));
    const long long before = lint::cellsRead();
    s.addDab(Dab{52, 36}, false);
    CHECK(lint::cellsRead() == before);
    CHECK(s.preview().at(20, 20) == c.at(20, 20));
}

TEST_CASE("haze at 100% on a blank canvas, inside its edges: a lone cell and a first 2x2, 3x3 or 9x9 dab land "
          "nothing; a pair lands; a row lands, and a 5x5 dab after it",
          "[spray][brush][lint]") {
    // Codex round 3, finding 2. At 100% the field is all ▒; four or more
    // shade cells of one glyph are a finding, fewer are not, and a cell
    // lighter than every neighbour, with three or more on the canvas, is a
    // speck. These are the cases this
    // canvas and these dabs, all well inside its edges, give: the guard
    // decides, and other dabs land otherwise (a corner dab, and a lone cell
    // after the row, below). A lone ▒ is held; a second beside it lands the
    // pair. A first dab of 2x2, 3x3 or 9x9 lands none of its cells.
    const Ramp r = steel();
    const Canvas c(40, 20);
    const SprayBrush haze(r, {SprayKind::Haze, 100, 3}, kInk);
    CHECK(drag(c, haze, {Dab{10, 10}}, 1).empty());
    const CellEdit pair = drag(c, haze, {Dab{10, 10}, Dab{11, 10}}, 1);
    const Canvas two = applied(c, pair);
    INFO(rungMap(two, r));
    CHECK(written(pair).size() == 2);
    CHECK(rungOf(r, two.at(10, 10)) == 2);
    CHECK(rungOf(r, two.at(11, 10)) == 2);
    CHECK(added(c, two, r).empty());
    for (const int size : {2, 3, 9}) {
        INFO("size " << size);
        CHECK(drag(c, haze, {Dab{20, 10}}, size).empty());
        CHECK(drag(c, haze, {Dab{20, 10}, Dab{24, 10}}, size).empty());
    }
    // A row of ten one-cell dabs along row 3 lands its first nine at ▒ and
    // holds its newest a rung short, at ░, so the canvas has two shade
    // glyphs; a 5x5 dab at (20, 12) after it lands all 25 cells.
    Stroke s(c, haze, BrushSettings{5, Tip::Square, 1.0, PressureTo::Size});
    for (int x = 2; x < 12; ++x)
        s.addDab(Dab{x, 3, 0.2});
    const Canvas row = s.preview();
    INFO(rungMap(row, r));
    for (int x = 2; x < 11; ++x)
        CHECK(rungOf(r, row.at(x, 3)) == 2);
    CHECK(rungOf(r, row.at(11, 3)) == 1);
    s.addDab(Dab{20, 12}, false);
    const Canvas big = s.preview();
    INFO(rungMap(big, r));
    for (int y = 10; y <= 14; ++y)
        for (int x = 18; x <= 22; ++x)
            CHECK(rungOf(r, big.at(x, y)) == 2);
    CHECK(added(c, big, r).empty());
}

TEST_CASE("haze at 100% on a blank canvas: a first 2x2 dab in the corner lands, one in the middle doesn't",
          "[spray][brush][lint]") {
    // Codex round 4, repro 1: the footprint is (0,0)..(1,1). Together the
    // four ▒ are one glyph, but (0, 0) alone is no speck (a corner has two
    // neighbours, and a speck needs three), and the rest follows from it.
    const Ramp r = steel();
    const Canvas c(40, 20);
    const SprayBrush haze(r, {SprayKind::Haze, 100, 3}, kInk);
    REQUIRE(drag(c, haze, {Dab{20, 10}}, 2).empty());
    const CellEdit corner = drag(c, haze, {Dab{1, 1}}, 2);
    const Canvas after = applied(c, corner);
    INFO(rungMap(after, r));
    CHECK(written(corner).size() == 4);
    int dark = 0, light = 0;
    for (int y = 0; y <= 1; ++y)
        for (int x = 0; x <= 1; ++x) {
            dark += rungOf(r, after.at(x, y)) == 2;
            light += rungOf(r, after.at(x, y)) == 1;
        }
    CHECK(dark == 3);
    CHECK(light == 1);
    CHECK(added(c, after, r).empty());
}

TEST_CASE("haze at 100% on a blank canvas: after a row that holds a ░, a lone cell away from it stays at rung 0",
          "[spray][brush][lint]") {
    // Codex round 4, repro 2: the row leaves two shade glyphs on the canvas,
    // but a one-cell dab at (20, 12), nine rows off it, is held: lifted alone
    // to ░ or to ▒ it is a speck either way.
    const Ramp r = steel();
    const Canvas c(40, 20);
    const SprayBrush haze(r, {SprayKind::Haze, 100, 3}, kInk);
    Stroke s(c, haze, BrushSettings{5, Tip::Square, 1.0, PressureTo::Size});
    for (int x = 2; x < 12; ++x)
        s.addDab(Dab{x, 3, 0.2});
    REQUIRE(rungOf(r, s.preview().at(11, 3)) == 1);
    const Canvas row = s.preview();
    s.addDab(Dab{20, 12, 0.2}, false);
    INFO(rungMap(s.preview(), r));
    CHECK(rungOf(r, s.preview().at(20, 12)) == 0);
    CHECK(s.preview() == row);
}
