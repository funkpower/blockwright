// Keyboard editing commands: cursor movement, typing, Backspace/Delete, and
// cell deltas applied and reverted.

#include "blockwright/edit.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

using namespace bw;

TEST_CASE("the cursor moves and stays on the canvas") {
    const Canvas cv(4, 3);
    CHECK(moveCursor(cv, {0, 0}, 1, 0) == Cursor{1, 0});
    CHECK(moveCursor(cv, {1, 1}, 0, 1) == Cursor{1, 2});
    CHECK(moveCursor(cv, {0, 0}, -1, -1) == Cursor{0, 0});
    CHECK(moveCursor(cv, {3, 2}, 1, 1) == Cursor{3, 2});
    CHECK(moveCursor(cv, {9, 9}, 0, 0) == Cursor{3, 2});
    CHECK(moveCursor(Canvas(0, 0), {2, 2}, 1, 1) == Cursor{0, 0});
}

TEST_CASE("only glyphs the file can hold are typeable") {
    CHECK(isTypeable(U'A'));
    CHECK(isTypeable(U' '));
    CHECK(isTypeable(U'~'));
    CHECK(isTypeable(U'é'));
    CHECK(isTypeable(U'░'));
    CHECK(isTypeable(U'☺'));   // CP437 0x01, drawn as a glyph
    CHECK_FALSE(isTypeable(U'★'));  // no CP437 code
    CHECK_FALSE(isTypeable(U'…'));
    CHECK_FALSE(isTypeable(U'\t'));
    CHECK_FALSE(isTypeable(U'\x1b'));
    CHECK_FALSE(isTypeable(U'\x7f'));
    CHECK_FALSE(isTypeable(U'\0'));
    CHECK_FALSE(isTypeable(U'○'));  // CP437 0x09, but the parser reads that byte as TAB
    CHECK_FALSE(isTypeable(U'◙'));  // 0x0A, LF
    CHECK_FALSE(isTypeable(U'♪'));  // 0x0D, CR
    CHECK_FALSE(isTypeable(U'→'));  // 0x1A, SUB
    CHECK_FALSE(isTypeable(U'←'));  // 0x1B, ESC
}

TEST_CASE("typing writes the glyph in the pen's colours and moves right") {
    Canvas cv(3, 2);
    Pen pen{Color::indexed(Yellow), Color::indexed(Blue)};
    EditResult r = typeGlyph(cv, {1, 1}, U'A', pen);
    REQUIRE(r.edit.changes.size() == 1);
    const CellChange& c = r.edit.changes[0];
    CHECK(c.x == 1);
    CHECK(c.y == 1);
    CHECK(c.before == Cell{});
    CHECK(c.after == Cell{U'A', Color::indexed(Yellow), Color::indexed(Blue), 0, false});
    CHECK(r.cursor == Cursor{2, 1});

    r.edit.apply(cv);
    CHECK(cv.at(1, 1).ch == U'A');

    // the last column: written, the cursor stays
    r = typeGlyph(cv, {2, 1}, U'B', pen);
    CHECK(r.edit.changes.size() == 1);
    CHECK(r.cursor == Cursor{2, 1});
}

TEST_CASE("the default pen is grey on black") {
    const Pen pen;
    CHECK(pen.fg == Color::indexed(Grey));
    CHECK(pen.bg == Color::indexed(Black));
}

TEST_CASE("typing an untypeable glyph changes nothing") {
    const Canvas cv(3, 1);
    const EditResult r = typeGlyph(cv, {1, 0}, U'★', Pen{});
    CHECK(r.edit.empty());
    CHECK(r.cursor == Cursor{1, 0});
}

TEST_CASE("typing what a cell already holds is no change, but still moves") {
    Canvas cv(3, 1);
    cv.at(0, 0) = Cell{U'x', Color::indexed(Grey), Color::indexed(Black), 0, false};
    const EditResult r = typeGlyph(cv, {0, 0}, U'x', Pen{});
    CHECK(r.edit.empty());
    CHECK(r.cursor == Cursor{1, 0});
}

TEST_CASE("replacing text writes from the range's start and ends the cursor after it") {
    Canvas cv(6, 2);
    for (int x = 0; x < 6; ++x)
        cv.at(x, 1).ch = U"abcdef"[x];
    const Pen pen{Color::indexed(Yellow), Color::indexed(Blue)};
    const Cell q{U'é', pen.fg, pen.bg, 0, false};

    // one cell before the cursor, replaced by one
    EditResult r = replaceText(cv, {2, 1}, -1, 1, U"é", pen);
    REQUIRE(r.edit.changes.size() == 1);
    CHECK(r.edit.changes[0] == CellChange{1, 1, Cell{U'b'}, q});
    CHECK(r.cursor == Cursor{2, 1});

    // three cells replaced by one: the other two are cleared
    r = replaceText(cv, {4, 1}, -3, 3, U"é", pen);
    Canvas out = cv;
    r.edit.apply(out);
    CHECK(out.at(0, 1).ch == U'a');
    CHECK(out.at(1, 1) == q);
    CHECK(out.at(2, 1) == Cell{});
    CHECK(out.at(3, 1) == Cell{});
    CHECK(out.at(4, 1).ch == U'e');
    CHECK(r.cursor == Cursor{2, 1});

    // one cell replaced by two: the second overwrites the next cell
    r = replaceText(cv, {1, 1}, 0, 1, U"xy", pen);
    out = cv;
    r.edit.apply(out);
    CHECK(out.at(1, 1).ch == U'x');
    CHECK(out.at(2, 1).ch == U'y');
    CHECK(out.at(3, 1).ch == U'd');
    CHECK(r.cursor == Cursor{3, 1});

    // nothing to write: the range is cleared and the cursor goes to its start
    r = replaceText(cv, {3, 1}, -2, 2, U"", pen);
    out = cv;
    r.edit.apply(out);
    CHECK(out.at(1, 1) == Cell{});
    CHECK(out.at(2, 1) == Cell{});
    CHECK(out.at(3, 1).ch == U'd');
    CHECK(r.cursor == Cursor{1, 1});

    // no range is typing; the other row is untouched
    r = replaceText(cv, {5, 1}, 0, 0, U"z", pen);
    CHECK(r.edit.changes == std::vector<CellChange>{{5, 1, Cell{U'f'}, Cell{U'z', pen.fg, pen.bg, 0, false}}});
    CHECK(r.cursor == Cursor{5, 1});

    // the edit reverts
    out = cv;
    replaceText(cv, {4, 1}, -3, 3, U"é", pen).edit.apply(out);
    replaceText(cv, {4, 1}, -3, 3, U"é", pen).edit.revert(out);
    CHECK(out == cv);
}

TEST_CASE("a replacement range off the row is clamped to it") {
    Canvas cv(4, 1);
    for (int x = 0; x < 4; ++x)
        cv.at(x, 0).ch = U"abcd"[x];
    const Pen pen;
    const auto row = [&](const EditResult& r) {
        Canvas out = cv;
        r.edit.apply(out);
        std::u32string s;
        for (int x = 0; x < 4; ++x)
            s += out.at(x, 0).ch;
        return s;
    };

    // starts before column 0: only the cells on the row are replaced
    EditResult r = replaceText(cv, {1, 0}, -3, 3, U"x", pen);
    CHECK(row(r) == U"xbcd");
    CHECK(r.cursor == Cursor{1, 0});
    r = replaceText(cv, {2, 0}, -3, 4, U"x", pen);
    CHECK(row(r) == U"x  d");
    CHECK(r.cursor == Cursor{1, 0});
    // wholly before column 0: written at column 0, nothing cleared
    r = replaceText(cv, {0, 0}, -5, 2, U"x", pen);
    CHECK(row(r) == U"xbcd");
    CHECK(r.cursor == Cursor{1, 0});
    // runs off the end: cleared to the last column
    r = replaceText(cv, {1, 0}, 0, 50, U"x", pen);
    CHECK(row(r) == U"ax  ");
    CHECK(r.cursor == Cursor{2, 0});
    // starts past the end: written on the last column, as typing there is
    r = replaceText(cv, {2, 0}, 9, 1, U"xy", pen);
    CHECK(row(r) == U"abcy");
    CHECK(r.cursor == Cursor{3, 0});
    // text longer than the row: the last column keeps the last character
    r = replaceText(cv, {2, 0}, 0, 0, U"xyz", pen);
    CHECK(row(r) == U"abxz");
    CHECK(r.cursor == Cursor{3, 0});
    // extremes don't overflow
    r = replaceText(cv, {3, 0}, std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), U"x", pen);
    CHECK(row(r) == U"abcx");
    r = replaceText(cv, {0, 0}, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), U"x", pen);
    CHECK(row(r) == U"xbcd");
    // negative length replaces nothing
    r = replaceText(cv, {1, 0}, 0, -2, U"x", pen);
    CHECK(row(r) == U"axcd");
}

TEST_CASE("replacing text with an untypeable glyph in it changes nothing") {
    Canvas cv(4, 1);
    cv.at(1, 0).ch = U'b';
    const EditResult r = replaceText(cv, {1, 0}, -1, 2, U"A★B", Pen{});
    CHECK(r.edit.empty());
    CHECK(r.cursor == Cursor{1, 0});
    CHECK(replaceText(Canvas(0, 0), {0, 0}, 0, 0, U"x", Pen{}).edit.empty());
}

TEST_CASE("Backspace clears the cell to the left and moves there") {
    Canvas cv(3, 1);
    cv.at(0, 0).ch = U'a';
    cv.at(1, 0).ch = U'b';
    cv.at(0, 0).bg = Color::indexed(Red);
    EditResult r = backspace(cv, {1, 0});
    REQUIRE(r.edit.changes.size() == 1);
    CHECK(r.edit.changes[0].x == 0);
    CHECK(r.edit.changes[0].after == Cell{});
    CHECK(r.cursor == Cursor{0, 0});
    r.edit.apply(cv);
    CHECK(cv.at(0, 0) == Cell{});
    CHECK(cv.at(1, 0).ch == U'b');

    // nothing to the left of the first column
    r = backspace(cv, {0, 0});
    CHECK(r.edit.empty());
    CHECK(r.cursor == Cursor{0, 0});
}

TEST_CASE("Delete clears the cell under the cursor") {
    Canvas cv(3, 1);
    cv.at(1, 0).ch = U'b';
    const EditResult r = deleteCell(cv, {1, 0});
    REQUIRE(r.edit.changes.size() == 1);
    CHECK(r.edit.changes[0].before.ch == U'b');
    CHECK(r.edit.changes[0].after == Cell{});
    CHECK(r.cursor == Cursor{1, 0});
    CHECK(deleteCell(Canvas(3, 1), {1, 0}).edit.empty());
}

TEST_CASE("an edit reverts to the canvas it was made on") {
    Canvas cv(4, 2);
    cv.at(3, 1).ch = U'z';
    const Canvas start = cv;
    Pen pen{Color::indexed(LightRed), Color::indexed(Black)};
    CellEdit all;
    Cursor c{2, 1};
    for (char32_t ch : {U'h', U'i'}) {
        const EditResult r = typeGlyph(cv, c, ch, pen);
        r.edit.apply(cv);
        all.changes.insert(all.changes.end(), r.edit.changes.begin(), r.edit.changes.end());
        c = r.cursor;
    }
    // both writes landed: the second on the last column, over the z
    CHECK(cv.at(2, 1).ch == U'h');
    CHECK(cv.at(3, 1).ch == U'i');
    const Canvas after = cv;
    all.revert(cv);
    CHECK(cv == start);
    all.apply(cv);
    CHECK(cv == after);
}

TEST_CASE("changes outside the canvas are skipped") {
    Canvas cv(2, 2);
    CellEdit e;
    e.changes.push_back({5, 5, Cell{}, Cell{U'x'}});
    e.apply(cv);
    e.revert(cv);
    CHECK(cv == Canvas(2, 2));
}
