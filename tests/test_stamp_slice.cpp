// Resizable Library stamps (#345 slice 2, PLAN §12): the slice insets a
// stamp's sidecar may carry, how they are checked, and the re-cut that
// resizes a frame or band through drawFrame.

#include "util.h"

#include "blockwright/canvas.h"
#include "blockwright/cp437.h"
#include "blockwright/frame.h"
#include "blockwright/library.h"
#include "blockwright/selection.h"
#include "blockwright/stamp_slice.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <climits>
#include <fstream>
#include <string>

using namespace bw;
using namespace bw::library;
using Catch::Matchers::ContainsSubstring;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const std::string& name) {
    const fs::path dir = fs::path(BW_TEST_TMP) / "stamp-slice" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void put(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

Cell cell(char32_t ch, int fg = Grey) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(std::uint8_t(fg));
    c.bg = Color::indexed(Black);
    return c;
}

// A block from rows of glyphs, all light grey on black.
CellBlock rows(std::initializer_list<std::u32string> rs) {
    const int h = int(rs.size()), w = int(rs.begin()->size());
    CellBlock b(w, h);
    int y = 0;
    for (const std::u32string& r : rs) {
        for (int x = 0; x < w; ++x)
            b.at(x, y) = cell(r[size_t(x)]);
        ++y;
    }
    return b;
}

std::u32string row(const CellBlock& b, int y) {
    std::u32string s;
    for (int x = 0; x < b.width; ++x)
        s += b.at(x, y).ch.codePoint().value_or(U'?');
    return s;
}

// A 3x3 box: one cell of each corner, edge and middle.
CellBlock box() { return rows({U"┌─┐", U"│ │", U"└─┘"}); }

// A band whose middle is three different cells, so the tiling shows.
CellBlock band() { return rows({U"[abc]"}); }

constexpr StampSlice kFrame{1, 1, 1, 1, true, true};
constexpr StampSlice kBand{1, 1, 0, 0, true, false};

// Writes `block` as a stamp with `slice`, then replaces its sidecar's insets
// with `insets` (a JSON value), and returns the .ans.
fs::path stampWithInsets(const fs::path& dir, const std::string& insets) {
    const StampFiles f = encodeStamp("Box", "frames", {"frame"}, box(), false, kFrame);
    const fs::path ans = dir / "box.ans";
    writeStamp(ans, f);
    std::string json = f.json;
    const std::string key = "\"insets\": [1, 1, 1, 1]";
    const size_t at = json.find(key);
    REQUIRE(at != std::string::npos);
    json.replace(at, key.size(), "\"insets\": " + insets);
    put(pairedSidecar(ans), json);
    return ans;
}

} // namespace

TEST_CASE("a slice is checked against the cells it cuts") {
    CHECK(sliceProblem(box(), kFrame).empty());
    CHECK(sliceProblem(band(), kBand).empty());
    CHECK(sliceProblem(band(), {2, 2, 0, 0, true, false}).empty());
    // the middle can't be empty, on either axis
    CHECK_THAT(sliceProblem(box(), {1, 2, 1, 1, true, true}), ContainsSubstring("no column"));
    CHECK_FALSE(sliceProblem(box(), {2, 1, 1, 1, true, true}).empty());
    CHECK_FALSE(sliceProblem(box(), {1, 1, 0, 3, true, true}).empty());
    CHECK_FALSE(sliceProblem(box(), {3, 0, 1, 1, true, true}).empty());
    CHECK_FALSE(sliceProblem(box(), {-1, 1, 1, 1, true, true}).empty());
    // an axis that doesn't resize takes no insets
    CHECK_FALSE(sliceProblem(band(), {1, 1, 1, 0, true, false}).empty());
    // one that resizes in neither is no slice
    CHECK_FALSE(sliceProblem(box(), {0, 0, 0, 0, false, false}).empty());
    CHECK_FALSE(sliceProblem(CellBlock(), kFrame).empty());
    // insets near INT_MAX don't overflow their sum
    CHECK_FALSE(sliceProblem(box(), {INT_MAX, INT_MAX, 1, 1, true, true}).empty());
}

TEST_CASE("a stamp holding a wide character can't be resizable") {
    CellBlock b = rows({U"[ab ]"});
    b.at(1, 0).ch = U'字';
    b.at(2, 0).ch = Glyph::continuation();
    REQUIRE(blockHoldsPair(b));
    CHECK_THAT(sliceProblem(b, kBand), ContainsSubstring("wide character"));
    CHECK_THROWS_AS(encodeStamp("Wide", "frames", {}, b, false, kBand), LibraryError);
}

TEST_CASE("a frame resizes at its minimum, at odd sizes and large ones") {
    const CellBlock b = box();
    CHECK(sliceMinimumWidth(b, kFrame) == 2);
    CHECK(sliceMinimumHeight(b, kFrame) == 2);
    CHECK(sliceMaximumWidth(b, kFrame) == kMaxColumns);
    CHECK(sliceMaximumHeight(b, kFrame) == kMaxRows);

    SECTION("the minimum is the corners alone") {
        const auto r = resizeStamp(b, kFrame, 2, 2);
        REQUIRE(r);
        CHECK(row(*r, 0) == U"┌┐");
        CHECK(row(*r, 1) == U"└┘");
        CHECK_FALSE(resizeStamp(b, kFrame, 1, 2));
        CHECK_FALSE(resizeStamp(b, kFrame, 2, 1));
        CHECK_FALSE(resizeStamp(b, kFrame, 0, 0));
        CHECK_FALSE(resizeStamp(b, kFrame, -5, 3));
    }
    SECTION("an odd size repeats the edges and the middle") {
        const auto r = resizeStamp(b, kFrame, 7, 5);
        REQUIRE(r);
        REQUIRE(r->width == 7);
        REQUIRE(r->height == 5);
        CHECK(row(*r, 0) == U"┌─────┐");
        for (int y = 1; y < 4; ++y)
            CHECK(row(*r, y) == U"│     │");
        CHECK(row(*r, 4) == U"└─────┘");
    }
    SECTION("the original size gives the stamp back") {
        CHECK(resizeStamp(b, kFrame, 3, 3) == b);
    }
    SECTION("up to the canvas's limits, and not past them") {
        const auto r = resizeStamp(b, kFrame, kMaxColumns, 80);
        REQUIRE(r);
        CHECK(r->width == kMaxColumns);
        CHECK(r->at(kMaxColumns - 1, 79).ch == Glyph(U'┘'));
        CHECK(resizeStamp(b, kFrame, 3, kMaxRows));
        // both limits at once: 2,000,000 cells
        const auto all = resizeStamp(b, kFrame, kMaxColumns, kMaxRows);
        REQUIRE(all);
        CHECK(all->width == kMaxColumns);
        CHECK(all->height == kMaxRows);
        CHECK(all->at(0, 0).ch == Glyph(U'┌'));
        CHECK(all->at(kMaxColumns - 1, kMaxRows - 1).ch == Glyph(U'┘'));
        CHECK(all->at(0, kMaxRows - 1).ch == Glyph(U'└'));
        CHECK(all->at(kMaxColumns / 2, kMaxRows / 2).ch == Glyph(U' '));
        CHECK_FALSE(resizeStamp(b, kFrame, kMaxColumns + 1, kMaxRows));
        CHECK_FALSE(resizeStamp(b, kFrame, kMaxColumns + 1, 3));
        CHECK_FALSE(resizeStamp(b, kFrame, 3, kMaxRows + 1));
        CHECK_FALSE(resizeStamp(b, kFrame, INT_MAX, INT_MAX));
    }
    SECTION("a pixel layer's block stays one") {
        CellBlock p = b;
        p.pixelLayer = true;
        CHECK(resizeStamp(p, kFrame, 5, 4)->pixelLayer);
    }
}

TEST_CASE("a band resizes in width only, its middle tiled from the left") {
    const CellBlock b = band();
    CHECK(sliceMinimumWidth(b, kBand) == 2);
    CHECK(sliceMinimumHeight(b, kBand) == 1);
    CHECK(sliceMaximumHeight(b, kBand) == 1);
    CHECK(row(*resizeStamp(b, kBand, 9, 1), 0) == U"[abcabca]");
    CHECK(row(*resizeStamp(b, kBand, 4, 1), 0) == U"[ab]");
    CHECK(row(*resizeStamp(b, kBand, 2, 1), 0) == U"[]");
    CHECK_FALSE(resizeStamp(b, kBand, 9, 2));
}

TEST_CASE("a flip or rotation turns the slice with the cells") {
    const StampSlice s{1, 2, 3, 4, true, true};
    CHECK(transformedSlice(s, Transform::FlipHorizontal) == StampSlice{2, 1, 3, 4, true, true});
    CHECK(transformedSlice(s, Transform::FlipVertical) == StampSlice{1, 2, 4, 3, true, true});
    CHECK(transformedSlice(s, Transform::RotateClockwise) == StampSlice{4, 3, 1, 2, true, true});
    CHECK(transformedSlice(s, Transform::RotateCounterClockwise) == StampSlice{3, 4, 2, 1, true, true});
    // a band on its side resizes in height only
    CHECK(transformedSlice(kBand, Transform::RotateClockwise) == StampSlice{0, 0, 1, 1, false, true});

    // Turning then resizing is resizing then turning.
    const CellBlock b = rows({U"ABC", U"DEF", U"GHI"});
    for (const Transform t : {Transform::FlipHorizontal, Transform::FlipVertical, Transform::RotateClockwise,
                              Transform::RotateCounterClockwise}) {
        INFO(int(t));
        const bool turn = t == Transform::RotateClockwise || t == Transform::RotateCounterClockwise;
        const auto a = resizeStamp(transformed(b, t), transformedSlice(kFrame, t), turn ? 4 : 6, turn ? 6 : 4);
        const auto r = resizeStamp(b, kFrame, 6, 4);
        REQUIRE(a);
        REQUIRE(r);
        CHECK(*a == transformed(*r, t));
    }
    // With several cells to a repeat cut part-way, they differ: each edge
    // repeats from the corner it starts at, so a flip moves the anchor. A
    // turned float re-cuts the stamp as saved and turns that instead.
    const CellBlock big = rows({U"AB12CD", U"EF34GH", U"ij56kl", U"mn78op", U"QR90ST", U"UVwxYZ"});
    const StampSlice cut{2, 1, 1, 2, true, true};
    REQUIRE(sliceProblem(big, cut).empty());
    const auto flippedFirst = resizeStamp(transformed(big, Transform::FlipHorizontal),
                                          transformedSlice(cut, Transform::FlipHorizontal), 11, 5);
    const auto cutFirst = resizeStamp(big, cut, 11, 5);
    REQUIRE(flippedFirst);
    REQUIRE(cutFirst);
    CHECK_FALSE(*flippedFirst == transformed(*cutFirst, Transform::FlipHorizontal));
    const CellBlock side = transformed(band(), Transform::RotateClockwise);
    const StampSlice sideSlice = transformedSlice(kBand, Transform::RotateClockwise);
    CHECK(sliceProblem(side, sideSlice).empty());
    const auto tall = resizeStamp(side, sideSlice, 1, 8);
    REQUIRE(tall);
    CHECK(tall->height == 8);
    CHECK_FALSE(resizeStamp(side, sideSlice, 2, 8));
}

TEST_CASE("a stamp past a frame style's source limit resizes in each dimension") {
    REQUIRE(120 > kMaxFrameSourceColumns);
    REQUIRE(120 > kMaxFrameSourceRows);
    // 120 x 3: a band whose middle is 118 different-ish cells
    std::u32string top(120, U'─'), mid(120, U' ');
    top.front() = U'┌';
    top.back() = U'┐';
    mid.front() = mid.back() = U'│';
    std::u32string bottom = top;
    bottom.front() = U'└';
    bottom.back() = U'┘';
    for (int x = 1; x < 119; ++x)
        mid[size_t(x)] = char32_t(U'a' + x % 26);
    const CellBlock wide = rows({top, mid, bottom});
    REQUIRE(sliceProblem(wide, kFrame).empty());
    const auto w = resizeStamp(wide, kFrame, 250, 4);
    REQUIRE(w);
    CHECK(w->at(0, 0).ch == Glyph(U'┌'));
    CHECK(w->at(249, 3).ch == Glyph(U'┘'));
    // the middle repeats every 118 columns from the left
    CHECK(w->at(1 + 118, 1).ch == w->at(1, 1).ch);
    CHECK(w->at(200, 2).ch == Glyph(char32_t(U'a' + (200 - 118) % 26)));
    CHECK(resizeStamp(wide, kFrame, 120, 3) == wide);
    CHECK(resizeStamp(wide, kFrame, 2, 2));
    // a band of it, width only
    const CellBlock strip = rows({mid});
    CHECK(row(*resizeStamp(strip, kBand, 120, 1), 0) == mid);
    CHECK(resizeStamp(strip, kBand, kMaxColumns, 1));

    // 3 x 120
    CellBlock tall(3, 120);
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 3; ++x)
            tall.at(x, y) = cell(y == 0 ? U"┌─┐"[x] : y == 119 ? U"└─┘"[x] : x == 1 ? char32_t(U'a' + y % 26) : U'│');
    REQUIRE(sliceProblem(tall, kFrame).empty());
    const auto t = resizeStamp(tall, kFrame, 5, 300);
    REQUIRE(t);
    CHECK(t->at(4, 299).ch == Glyph(U'┘'));
    CHECK(t->at(2, 1 + 118).ch == t->at(2, 1).ch);
    CHECK(resizeStamp(tall, kFrame, 3, 120) == tall);
}

TEST_CASE("a flip twice or a quarter turn and back or four quarter turns leave every CP437 glyph as it was") {
    // what addTurn relies on to drop turns from a float's list
    CellBlock all(16, 16);
    for (int b = 0; b < 256; ++b)
        all.at(b % 16, b / 16) = cell(cp437ToUnicode(std::uint8_t(b)));
    const auto apply = [](CellBlock b, std::initializer_list<Transform> ts) {
        for (const Transform t : ts)
            b = transformed(b, t);
        return b;
    };
    using T = Transform;
    CHECK(apply(all, {T::FlipHorizontal, T::FlipHorizontal}) == all);
    CHECK(apply(all, {T::FlipVertical, T::FlipVertical}) == all);
    CHECK(apply(all, {T::RotateClockwise, T::RotateCounterClockwise}) == all);
    CHECK(apply(all, {T::RotateCounterClockwise, T::RotateClockwise}) == all);
    CHECK(apply(all, {T::RotateClockwise, T::RotateClockwise, T::RotateClockwise, T::RotateClockwise}) == all);
    CHECK(apply(all, {T::RotateCounterClockwise, T::RotateCounterClockwise, T::RotateCounterClockwise,
                      T::RotateCounterClockwise}) == all);
    // and glyph by glyph, so a failure names it
    for (int b = 0; b < 256; ++b) {
        const char32_t ch = cp437ToUnicode(std::uint8_t(b));
        INFO("byte " << b);
        CHECK(flipGlyph(flipGlyph(ch, Flip::Horizontal), Flip::Horizontal) == ch);
        CHECK(flipGlyph(flipGlyph(ch, Flip::Vertical), Flip::Vertical) == ch);
        CHECK(rotateGlyph(rotateGlyph(ch, true), false) == ch);
        CHECK(rotateGlyph(rotateGlyph(ch, false), true) == ch);
        CHECK(rotateGlyph(rotateGlyph(rotateGlyph(rotateGlyph(ch, true), true), true), true) == ch);
        CHECK(rotateGlyph(rotateGlyph(rotateGlyph(rotateGlyph(ch, false), false), false), false) == ch);
    }
}

TEST_CASE("a turned stamp's limits are on the size it shows") {
    using T = Transform;
    const CellBlock b = band();
    // on its side a band is up to kMaxRows long, cut kMaxRows wide first
    const auto tall = resizeTurnedStamp(b, kBand, {T::RotateClockwise}, 1, kMaxRows);
    REQUIRE(tall);
    CHECK(tall->width == 1);
    CHECK(tall->height == kMaxRows);
    // [abcabc…] read down, each glyph turned: the cut is past the columns,
    // so it isn't resizeStamp's
    REQUIRE_FALSE(resizeStamp(b, kBand, 401, 1));
    const auto side = resizeTurnedStamp(b, kBand, {T::RotateClockwise}, 1, 401);
    REQUIRE(side);
    CHECK(side->at(0, 0).ch == Glyph(rotateGlyph(U'[', true)));
    CHECK(side->at(0, 400).ch == Glyph(rotateGlyph(U']', true)));
    for (int y = 1; y < 400; ++y)
        if (side->at(0, y).ch != Glyph(U"abc"[(y - 1) % 3]))
            FAIL("row " << y);
    CHECK_FALSE(resizeTurnedStamp(b, kBand, {T::RotateClockwise}, 1, kMaxRows + 1));
    CHECK_FALSE(resizeTurnedStamp(b, kBand, {T::RotateClockwise}, 2, 8));  // its width is fixed
    // a frame on its side: no wider than the columns, as tall as the rows
    CHECK_FALSE(resizeTurnedStamp(box(), kFrame, {T::RotateCounterClockwise}, kMaxColumns + 1, 3));
    CHECK(resizeTurnedStamp(box(), kFrame, {T::RotateCounterClockwise}, kMaxColumns, 401));
    CHECK(resizeTurnedStamp(box(), kFrame, {T::RotateCounterClockwise, T::FlipVertical}, 3, kMaxRows));
    // two quarter turns are back the right way up
    CHECK_FALSE(resizeTurnedStamp(b, kBand, {T::RotateClockwise, T::RotateClockwise}, 1, 401));
    CHECK_FALSE(resizeTurnedStamp(b, kBand, {T::RotateClockwise, T::RotateClockwise}, kMaxColumns + 1, 1));
    CHECK(resizeTurnedStamp(b, kBand, {T::RotateClockwise, T::RotateClockwise}, kMaxColumns, 1));
    // no turns is resizeStamp
    CHECK(resizeTurnedStamp(box(), kFrame, {}, 7, 5) == resizeStamp(box(), kFrame, 7, 5));
}

TEST_CASE("a turned band's fixed width obeys the column limit") {
    CellBlock b(3, kMaxColumns + 1);
    for (Cell& c : b.cells)
        c.ch = U'x';
    const StampSlice band{1, 1, 0, 0, true, false};
    REQUIRE(sliceProblem(b, band).empty());
    CHECK_FALSE(resizeTurnedStamp(b, band, {Transform::RotateClockwise}, kMaxColumns + 1, 8));
    // nor a 3 x 5000 band to 5000 x 5000, past the canvas's cells
    CellBlock longest(3, kMaxRows);
    for (Cell& c : longest.cells)
        c.ch = U'x';
    CHECK_FALSE(resizeTurnedStamp(longest, band, {Transform::RotateClockwise}, kMaxRows, kMaxRows));
    // at the columns it still resizes
    CellBlock widest(3, kMaxColumns);
    for (Cell& c : widest.cells)
        c.ch = U'x';
    const auto r = resizeTurnedStamp(widest, band, {Transform::RotateClockwise}, kMaxColumns, 8);
    REQUIRE(r);
    CHECK(r->width == kMaxColumns);
    CHECK(r->height == 8);
}

namespace {

// Every CP437 glyph in a 16 x 17 block (not square, so a turn shows), each
// a different foreground on a different background, so a glyph whose turn
// swaps its colours shows too.
CellBlock everyGlyph() {
    CellBlock all(16, 17);
    for (int i = 0; i < 16 * 17; ++i) {
        Cell c;
        c.ch = cp437ToUnicode(std::uint8_t(i % 256));
        c.fg = Color::indexed(std::uint8_t(i % 16));
        c.bg = Color::indexed(std::uint8_t((i / 16 + 3) % 16));
        all.cells[std::size_t(i)] = c;
    }
    return all;
}

CellBlock replay(CellBlock b, const std::vector<Transform>& ts) {
    for (const Transform t : ts)
        b = transformed(b, t);
    return b;
}

} // namespace

TEST_CASE("addTurn keeps one list per state, and each turn after it lands on the state it keeps") {
    // The relations addTurn reduces by, checked on the cells: if replaying
    // each list it keeps and then a turn gives what replaying the list it
    // keeps for the two gives, every sequence replays the same as its
    // reduced list, by induction on its length.
    using T = Transform;
    const CellBlock all = everyGlyph();
    const T kTurns[] = {T::FlipHorizontal, T::FlipVertical, T::RotateClockwise, T::RotateCounterClockwise};
    // the lists reachable, found by closing over addTurn from none
    std::vector<std::vector<Transform>> lists = {{}};
    for (size_t i = 0; i < lists.size() && lists.size() <= 64; ++i)
        for (const T t : kTurns) {
            std::vector<Transform> next = lists[i];
            addTurn(next, t);
            if (std::find(lists.begin(), lists.end(), next) == lists.end())
                lists.push_back(next);
        }
    // the eight orientations, each with the brackets a half turn round or not
    REQUIRE(lists.size() == 16);
    std::vector<CellBlock> shown;
    for (const auto& l : lists) {
        CHECK(l.size() <= kMaxTurns);
        shown.push_back(replay(all, l));
    }
    // sixteen different results: no list could be shorter by being another
    for (size_t i = 0; i < shown.size(); ++i)
        for (size_t j = i + 1; j < shown.size(); ++j)
            CHECK(shown[i] != shown[j]);
    for (const auto& l : lists)
        for (const T t : kTurns) {
            std::vector<Transform> next = l;
            addTurn(next, t);
            INFO("list of " << l.size() << " then turn " << int(t));
            CHECK(replay(all, next) == transformed(replay(all, l), t));
        }
    // and glyph by glyph, the relations themselves, so a failure names it
    const std::u32string brackets = U"([)]<«>»{≤}≥";
    const auto r = [](char32_t c, int n, bool cw) {
        for (int i = 0; i < n; ++i)
            c = rotateGlyph(c, cw);
        return c;
    };
    for (int b = 0; b < 256; ++b) {
        const char32_t ch = cp437ToUnicode(std::uint8_t(b));
        INFO("byte " << b);
        const char32_t h = flipGlyph(ch, Flip::Horizontal), v = flipGlyph(ch, Flip::Vertical);
        // H and V commute, and the two make a half turn
        CHECK(flipGlyph(h, Flip::Vertical) == flipGlyph(v, Flip::Horizontal));
        CHECK(flipGlyph(h, Flip::Vertical) == r(ch, 2, true));
        if (brackets.find(ch) == std::u32string::npos) {
            // a flip then a quarter turn is the turn back then the flip
            CHECK(rotateGlyph(h, true) == flipGlyph(rotateGlyph(ch, false), Flip::Horizontal));
            CHECK(rotateGlyph(h, false) == flipGlyph(rotateGlyph(ch, true), Flip::Horizontal));
        } else {
            // a bracket: a horizontal flip is a half turn, a vertical one none
            CHECK(h == r(ch, 2, true));
            CHECK(v == ch);
            CHECK(rotateGlyph(ch, true) != ch);
        }
    }
}

TEST_CASE("addTurn keeps a float's turns to a fixed few") {
    using T = Transform;
    std::vector<Transform> turns;
    for (int i = 0; i < 1000; ++i)
        for (const T t : {T::FlipHorizontal, T::FlipVertical, T::FlipHorizontal, T::FlipVertical})
            addTurn(turns, t);
    CHECK(turns.empty());
    // 10,000 at random: the short list replays as the whole sequence does
    const CellBlock all = everyGlyph();
    std::uint32_t seed = 0x345u;
    std::vector<Transform> every;
    CellBlock full = all;
    for (int i = 0; i < 10000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const T t = T(seed >> 30);
        every.push_back(t);
        full = transformed(full, t);
        addTurn(turns, t);
        REQUIRE(turns.size() <= kMaxTurns);
    }
    CHECK(replay(all, turns) == full);
    CHECK(replay(all, turns) == replay(all, every));
}

TEST_CASE("addTurn drops the turns that undo each other") {
    using T = Transform;
    std::vector<Transform> turns;
    for (int i = 0; i < 1000; ++i) {
        addTurn(turns, T::FlipHorizontal);
        addTurn(turns, T::FlipHorizontal);
    }
    CHECK(turns.empty());
    addTurn(turns, T::RotateClockwise);
    addTurn(turns, T::RotateCounterClockwise);
    CHECK(turns.empty());
    for (int i = 0; i < 4; ++i)
        addTurn(turns, T::RotateCounterClockwise);
    CHECK(turns.empty());
    // nested: a flip inside a turn and back
    addTurn(turns, T::RotateClockwise);
    addTurn(turns, T::FlipVertical);
    addTurn(turns, T::FlipVertical);
    addTurn(turns, T::RotateCounterClockwise);
    CHECK(turns.empty());
    // what doesn't undo is reduced to the orientation it makes
    addTurn(turns, T::FlipHorizontal);
    addTurn(turns, T::FlipVertical);
    addTurn(turns, T::RotateClockwise);
    addTurn(turns, T::RotateClockwise);
    addTurn(turns, T::RotateClockwise);
    // FH FV is a half turn, and three more quarter turns make one
    CHECK(turns == std::vector<Transform>{T::RotateClockwise});
    addTurn(turns, T::RotateClockwise);
    CHECK(turns == std::vector<Transform>{T::FlipHorizontal, T::FlipVertical});
    // and the shorter list cuts what the longer one would
    const CellBlock big = rows({U"┌╥─┐", U"╞═╡│", U"│▀▄│", U"└┴─┘"});
    const StampSlice cut{1, 2, 1, 2, true, true};
    const std::vector<Transform> longer = {T::RotateClockwise, T::FlipHorizontal, T::FlipHorizontal,
                                           T::RotateClockwise, T::RotateCounterClockwise, T::FlipVertical};
    std::vector<Transform> shorter;
    for (const Transform t : longer)
        addTurn(shorter, t);
    CHECK(shorter == std::vector<Transform>{T::RotateClockwise, T::FlipVertical});
    CHECK(resizeTurnedStamp(big, cut, shorter, 9, 7) == resizeTurnedStamp(big, cut, longer, 9, 7));
}

TEST_CASE("a frame's or band's sidecar carries its insets after ircBytes") {
    const StampFiles frame = encodeStamp("Box", "frames", {"frame"}, box(), false, kFrame);
    CHECK_THAT(frame.json, ContainsSubstring("\"ircBytes\": " ));
    CHECK_THAT(frame.json, ContainsSubstring(",\n  \"insets\": [1, 1, 1, 1]\n}"));
    const StampFiles b = encodeStamp("Band", "frames", {}, band(), false, StampSlice{2, 1, 0, 0, true, false});
    CHECK_THAT(b.json, ContainsSubstring("\"insets\": [2, 1]\n"));
    // a stamp of a fixed size is written as before
    CHECK(encodeStamp("Box", "frames", {}, box(), false).json.find("insets") == std::string::npos);

    const fs::path dir = scratch("roundtrip");
    writeStamp(dir / "box.ans", frame);
    writeStamp(dir / "band.ans", b);
    CHECK(readStamp(dir / "box.ans").meta.slice == kFrame);
    CHECK(readStamp(dir / "band.ans").meta.slice == StampSlice{2, 1, 0, 0, true, false});
    const ScanResult r = scan(dir);
    CHECK(r.problems.empty());
    CHECK(r.stamps.size() == 2);
}

TEST_CASE("encodeStamp refuses a slice that can't cut the cells, or resizes in height only") {
    CHECK_THROWS_WITH(encodeStamp("Box", "frames", {}, box(), false, StampSlice{2, 1, 1, 1, true, true}),
                      ContainsSubstring("can't be resizable"));
    CHECK_THROWS_AS(encodeStamp("Box", "frames", {}, box(), false, StampSlice{0, 0, 1, 1, false, true}),
                    LibraryError);
}

TEST_CASE("a sidecar's insets are refused unless whole cells the art can be cut by") {
    const fs::path dir = scratch("refused");
    for (const std::string bad : {"[1, 1, 1]", "[]", "[1]", "[1, 1, 1, 1, 1]", "\"1, 1\"", "1", "{}", "null",
                                  "[1, -1]", "[1.5, 1]", "[1, 1e9]", "[\"1\", 1]", "[1, true]", "[1, null]"}) {
        INFO(bad);
        const fs::path ans = stampWithInsets(dir, bad);
        CHECK_THROWS_WITH(readStamp(ans), ContainsSubstring("insets"));
    }
    // numbers in range that don't fit the art
    for (const std::string bad : {"[2, 1, 1, 1]", "[1, 1, 0, 3]", "[3, 0]", "[5000, 0]"}) {
        INFO(bad);
        CHECK_THROWS_WITH(readStamp(stampWithInsets(dir, bad)), ContainsSubstring("can't cut the art"));
    }
    // a band of a 3-row stamp: in range, and it resizes in width only
    CHECK(readStamp(stampWithInsets(dir, "[1, 1]")).meta.slice == StampSlice{1, 1, 0, 0, true, false});
    CHECK(readStamp(stampWithInsets(dir, "[0, 0, 0, 0]")).meta.slice == StampSlice{0, 0, 0, 0, true, true});
}

TEST_CASE("a sidecar past the byte limit is refused and scanned as a problem") {
    const fs::path dir = scratch("large");
    const StampFiles f = encodeStamp("Box", "frames", {}, box(), false, kFrame);
    writeStamp(dir / "box.ans", f);
    writeStamp(dir / "ok.ans", encodeStamp("Ok", "frames", {}, box(), false));
    std::string json = f.json;
    json.insert(json.size() - 2, std::string(kMaxSidecarBytes, ' '));
    put(dir / "box.json", json);
    CHECK_THROWS_WITH(readStamp(dir / "box.ans"), ContainsSubstring("larger than"));
    ScanResult r;
    REQUIRE_NOTHROW(r = scan(dir));
    REQUIRE(r.problems.size() == 1);
    CHECK(r.problems[0].path == dir / "box.ans");
    CHECK_THAT(r.problems[0].message, ContainsSubstring("larger than"));
    CHECK(r.stamps.size() == 1);
    // one at the limit is read
    json = f.json;
    json.insert(json.size() - 2, std::string(kMaxSidecarBytes - f.json.size(), ' '));
    REQUIRE(json.size() == kMaxSidecarBytes);
    put(dir / "box.json", json);
    CHECK(readStamp(dir / "box.ans").meta.slice == kFrame);
}

TEST_CASE("readRegularFile stops at its limit") {
    const fs::path dir = scratch("limit");
    put(dir / "f", std::string(100, 'x'));
    CHECK(readRegularFile(dir / "f", 100).size() == 100);
    CHECK_THROWS_WITH(readRegularFile(dir / "f", 99), ContainsSubstring("larger than 99 bytes"));
    CHECK_THROWS_AS(readRegularFile(dir / "f", 0), LibraryError);
    CHECK(readRegularFile(dir / "f").size() == 100);
}

TEST_CASE("scan reports a stamp with bad insets and reads the rest") {
    const fs::path dir = scratch("scan");
    stampWithInsets(dir, "[9, 9, 9, 9]");
    writeStamp(dir / "band.ans", encodeStamp("Band", "frames", {}, band(), false, kBand));
    const ScanResult r = scan(dir);
    REQUIRE(r.problems.size() == 1);
    CHECK_THAT(r.problems[0].message, ContainsSubstring("insets"));
    REQUIRE(r.stamps.size() == 1);
    CHECK(r.stamps[0].meta.slice == kBand);
}
