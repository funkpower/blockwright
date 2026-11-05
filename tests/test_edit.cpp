// Keyboard editing commands: cursor movement, typing, Backspace/Delete, and
// cell deltas applied and reverted.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/despeckle.h"
#include "blockwright/edit.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/layer.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/target.h"
#include "blockwright/width.h"

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

    // the last column: written, the cursor stays, past the glyph
    r = typeGlyph(cv, {2, 1}, U'B', pen);
    CHECK(r.edit.changes.size() == 1);
    CHECK(r.cursor == Cursor{2, 1, true});
    // and stays past it typing there again
    CHECK(typeGlyph(cv, r.cursor, U'C', pen).cursor == Cursor{2, 1, true});
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
    CHECK(r.cursor == Cursor{5, 1, true});

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
            s += out.at(x, 0).ch.codePoint().value();
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
    CHECK(r.cursor == Cursor{3, 0, true});
    // text longer than the row: the last column keeps the last character
    r = replaceText(cv, {2, 0}, 0, 0, U"xyz", pen);
    CHECK(row(r) == U"abxz");
    CHECK(r.cursor == Cursor{3, 0, true});
    // text that stops short of the last column leaves the cursor on it
    r = replaceText(cv, {2, 0}, 0, 0, U"x", pen);
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

TEST_CASE("Backspace after typing on the last column clears that column") {
    // #28 review P3-1: Backspace cleared the cell left of the last one
    Canvas cv(3, 1);
    const Pen pen;
    EditResult r = typeGlyph(cv, {1, 0}, U'a', pen);
    r.edit.apply(cv);
    r = typeGlyph(cv, r.cursor, U'b', pen);
    r.edit.apply(cv);
    CHECK(r.cursor == Cursor{2, 0, true});
    r = backspace(cv, r.cursor);
    r.edit.apply(cv);
    CHECK(cv.at(2, 0) == Cell{});
    CHECK(cv.at(1, 0).ch == U'a');
    CHECK(r.cursor == Cursor{2, 0});
    // the next Backspace goes on leftwards as usual
    r = backspace(cv, r.cursor);
    r.edit.apply(cv);
    CHECK(cv.at(1, 0) == Cell{});
    CHECK(r.cursor == Cursor{1, 0});

    // a one-column canvas: the only column is the last
    Canvas one(1, 1);
    r = typeGlyph(one, {0, 0}, U'x', pen);
    r.edit.apply(one);
    r = backspace(one, r.cursor);
    r.edit.apply(one);
    CHECK(one.at(0, 0) == Cell{});
    CHECK(r.cursor == Cursor{0, 0});

    // past the end means nothing off the last column
    Canvas row(3, 1);
    row.at(0, 0).ch = U'a';
    r = backspace(row, {1, 0, true});
    REQUIRE(r.edit.changes.size() == 1);
    CHECK(r.edit.changes[0].x == 0);
    // Delete clears the last column and the cursor is back on it
    row.at(2, 0).ch = U'c';
    r = deleteCell(row, {2, 0, true});
    CHECK(r.edit.changes.size() == 1);
    CHECK(r.cursor == Cursor{2, 0});
}

TEST_CASE("an input-method replacement past the end counts back from after the last column") {
    Canvas cv(3, 1);
    cv.at(1, 0).ch = U'a';
    cv.at(2, 0).ch = U'e';
    // the dead key's e was typed on the last column; é replaces it
    EditResult r = replaceText(cv, {2, 0, true}, -1, 1, U"é", Pen{});
    REQUIRE(r.edit.changes.size() == 1);
    CHECK(r.edit.changes[0].x == 2);
    CHECK(r.edit.changes[0].after.ch == U'é');
    CHECK(r.cursor == Cursor{2, 0, true});
    // without past-the-end the same range is the cell left of the cursor
    r = replaceText(cv, {2, 0}, -1, 1, U"é", Pen{});
    REQUIRE(r.edit.changes.size() == 1);
    CHECK(r.edit.changes[0] == CellChange{1, 0, Cell{U'a'}, Cell{U'é'}});
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

TEST_CASE("the F-key glyph sets are Moebius's, and every glyph is typeable") {
    // set 6 (the default) is the block and shade set
    CHECK(kDefaultGlyphSet == 5);
    CHECK(glyphSet(kDefaultGlyphSet) == GlyphSet{U'░', U'▒', U'▓', U'█', U'▀', U'▄', U'▌', U'▐', U'■', U'·'});
    CHECK(glyphSet(0) == GlyphSet{U'┌', U'┐', U'└', U'┘', U'─', U'│', U'├', U'┤', U'┴', U'┬'});
    CHECK(glyphSetGlyph(1, 0) == U'╔');
    CHECK(glyphSetGlyph(15, 9) == U'\'');
    // the index wraps
    CHECK(glyphSet(kGlyphSetCount) == glyphSet(0));
    CHECK(glyphSet(-1) == glyphSet(kGlyphSetCount - 1));
    for (int s = 0; s < kCp437GlyphSetCount; ++s)
        for (int k = 0; k < kGlyphSetKeys; ++k) {
            INFO("set " << s + 1 << " F" << k + 1);
            CHECK(isTypeable(glyphSetGlyph(s, k)));
        }
}

TEST_CASE("the typing policy is CP437 only for an ANSI document saved as a CP437 .ans") {
    CHECK(typingPolicy(Target::Ansi, TextEncoding::Cp437) == TypingPolicy::Cp437);
    CHECK(typingPolicy(Target::Ansi, TextEncoding::Utf8) == TypingPolicy::Unicode);
    CHECK(typingPolicy(Target::Irc, TextEncoding::Cp437) == TypingPolicy::Unicode);
    CHECK(typingPolicy(Target::Irc, TextEncoding::Utf8) == TypingPolicy::Unicode);
}

TEST_CASE("the Unicode policy types any glyph a UTF-8 .ans holds, the CP437 policy only CP437's") {
    // wide and narrow, a cluster too (#359's UTF-8 rules)
    for (const std::u32string_view k : {U"A", U"▚", U"★", U"○", U"◢", U"▔", U"中", U"q\u0302"}) {
        INFO("U+" << std::hex << unsigned(k[0]));
        CHECK(isTypeable(k, TypingPolicy::Unicode));
    }
    CHECK(isTypeable(U"A", TypingPolicy::Cp437));
    CHECK_FALSE(isTypeable(U"▚", TypingPolicy::Cp437));
    CHECK_FALSE(isTypeable(U"★", TypingPolicy::Cp437));
    CHECK_FALSE(isTypeable(U"中", TypingPolicy::Cp437));
    // controls, under either
    for (const char32_t ch : {char32_t(0x0085), char32_t(0x2028), U'\t', char32_t(0x7f), char32_t(0xd800)}) {
        INFO("U+" << std::hex << unsigned(ch));
        CHECK_FALSE(isTypeable(std::u32string(1, ch), TypingPolicy::Unicode));
        CHECK_FALSE(isTypeable(std::u32string(1, ch), TypingPolicy::Cp437));
    }
}

TEST_CASE("typing a Unicode glyph writes it under the Unicode policy and nothing under CP437") {
    const Canvas cv(3, 1);
    const EditResult r = typeGlyph(cv, {1, 0}, U'★', Pen{}, TypingPolicy::Unicode);
    Canvas after = cv;
    r.edit.apply(after);
    CHECK(after.at(1, 0).ch == U'★');
    CHECK(r.cursor == Cursor{2, 0});
    const EditResult refused = typeGlyph(cv, {1, 0}, U'★', Pen{}, TypingPolicy::Cp437);
    CHECK(refused.edit.empty());
    CHECK(refused.rejected == U"★");

    const TypingOptions unicode{TypingPolicy::Unicode};
    const EditResult t = replaceText(cv, {0, 0}, 0, 0, U"▚★", Pen{}, Cell{}, unicode);
    Canvas typed = cv;
    t.edit.apply(typed);
    CHECK(typed.at(0, 0).ch == U'▚');
    CHECK(typed.at(1, 0).ch == U'★');
    CHECK(replaceText(cv, {0, 0}, 0, 0, U"▚★", Pen{}, Cell{}, TypingOptions{}).edit.empty());

    // a wide glyph types as a pair under Unicode, as #359 types it in a
    // UTF-8 document, and is refused under CP437
    const EditResult wide = replaceText(cv, {0, 0}, 0, 0, U"a中", Pen{}, Cell{}, unicode);
    Canvas pair = cv;
    wide.edit.apply(pair);
    CHECK(pair.at(0, 0).ch == U'a');
    CHECK(pair.at(1, 0).ch == U'中');
    CHECK(pair.at(2, 0).ch.isContinuation());
    CHECK(wide.cursor == Cursor{1, 0, true});
    const EditResult cp437 = replaceText(cv, {0, 0}, 0, 0, U"a中", Pen{}, Cell{}, TypingOptions{});
    CHECK(cp437.edit.empty());
    CHECK(cp437.rejected == U"中");
}

TEST_CASE("four Unicode F-key sets follow Moebius's, offered only under the Unicode policy") {
    CHECK(glyphSetCount(TypingPolicy::Cp437) == 16);
    CHECK(glyphSetCount(TypingPolicy::Unicode) == 20);
    CHECK(glyphSet(16) == GlyphSet{U'▖', U'▗', U'▘', U'▝', U'▚', U'▞', U'▙', U'▛', U'▜', U'▟'});
    CHECK(glyphSet(17) == GlyphSet{U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█', U'▔', U'▕'});
    CHECK(glyphSet(18) == GlyphSet{U'▏', U'▎', U'▍', U'▌', U'▋', U'▊', U'▉', U'█', U'▐', U'▕'});
    CHECK(glyphSet(19) == GlyphSet{U'◢', U'◣', U'◤', U'◥', U'◆', U'◇', U'○', U'●', U'★', U'☆'});
    for (int s = kCp437GlyphSetCount; s < kGlyphSetCount; ++s)
        for (int k = 0; k < kGlyphSetKeys; ++k) {
            INFO("set " << s + 1 << " F" << k + 1);
            CHECK(codePointWidth(glyphSetGlyph(s, k)) == 1);
            const char32_t ch = glyphSetGlyph(s, k);
            CHECK(isTypeable(std::u32string_view(&ch, 1), TypingPolicy::Unicode));
        }
}

TEST_CASE("a drawn edit keeps the dither mark only on a cell it leaves looking the same", "[dither]") {
    // #160: the gradient's dither marks (Cell::dithered) are settled per
    // edit as the lost-blink marks are
    Cell plain{U'▒', Color::indexed(7), Color::indexed(8), 0, false};
    Cell marked = plain;
    marked.dithered = true;
    const Cell other{U'█', Color::indexed(7), Color::indexed(0), 0, false};

    Canvas one(1, 1);
    one.at(0, 0) = marked;
    SECTION("a new look drops it") {
        const CellEdit e = settleMarks(one, CellEdit{{{0, 0, marked, other}}});
        REQUIRE(e.changes.size() == 1);
        CHECK_FALSE(e.changes[0].after.dithered);
    }
    SECTION("the same look keeps it, so there is nothing to do") {
        CHECK(settleMarks(one, CellEdit{{{0, 0, marked, plain}}}).changes.empty());
    }
    SECTION("a drawn cell takes no mark from its tool") {
        one.at(0, 0) = plain;
        CHECK(settleMarks(one, CellEdit{{{0, 0, plain, marked}}}).changes.empty());
    }
    Canvas two(2, 1);
    two.at(0, 0) = plain;
    two.at(1, 0) = marked;
    SECTION("the gradient's own edit keeps its marks and drops the ones it writes over") {
        CellEdit gradient{{{0, 0, plain, marked}, {1, 0, marked, plain}}};
        gradient.dithers = true;
        const CellEdit e = settleMarks(two, gradient);
        CHECK(e.dithers);
        REQUIRE(e.changes.size() == 2);
        CHECK(e.changes[0].after.dithered);
        CHECK_FALSE(e.changes[1].after.dithered);
    }
    SECTION("the gradient's flag reaches the layer's edit") {
        // settleMarks runs on the layer's edit, after fitLayerEdit
        CellEdit gradient{{{0, 0, plain, marked}}};
        gradient.dithers = true;
        const Layer l = makeLayer(LayerKind::Cells, 2, 1);
        CHECK(toLayerEdit(l, gradient).dithers);
        CHECK(fitLayerEdit(l, gradient)->dithers);
    }
    SECTION("a move carries them as they are") {
        CellEdit move{{{0, 0, plain, marked}, {1, 0, marked, other}}};
        move.moves = true;
        const CellEdit e = settleMarks(two, move);
        CHECK(e.changes[0].after.dithered);
        CHECK_FALSE(e.changes[1].after.dithered);
    }
    SECTION("a brush stroke over the cell: the same look keeps it, a new one drops it") {
        Canvas c(2, 1);
        c.at(0, 0) = marked;
        c.at(1, 0) = marked;
        const DespeckleBrush brush(*findRamp("steel"));
        Stroke s(c, brush, BrushSettings{});
        CHECK(s.set(0, 0, plain));
        CHECK(s.set(1, 0, other));
        const Canvas out = s.preview();
        CHECK(out.at(0, 0).dithered);
        CHECK_FALSE(out.at(1, 0).dithered);
        const CellEdit e = s.edit();
        REQUIRE(e.changes.size() == 1);
        CHECK(e.changes[0].x == 1);
    }
}

// PLAN §16, Cursor and typing (#334 slice 8).
namespace {

const TypingOptions kUnicode{TypingPolicy::Unicode};

// A row of `w` cells with 中 as a pair at `x`.
Canvas withPair(int w, int x, char32_t wide = U'中') {
    Canvas cv(w, 1);
    cv.at(x, 0).ch = wide;
    cv.at(x + 1, 0).ch = Glyph::continuation();
    return cv;
}

// `r`'s edit settled (settleWide, as Document::edit does) and applied.
void land(Canvas& cv, const EditResult& r) { settleWide(cv, r.edit).apply(cv); }

std::u32string cps(Glyph g) {
    std::u32string s;
    for (const char32_t cp : codePoints(g))
        s += cp;
    return s;
}

} // namespace

TEST_CASE("the cursor never rests on a continuation", "[wide]") {
    const Canvas cv = withPair(6, 1);
    CHECK(moveCursor(cv, {0, 0}, 1, 0) == Cursor{1, 0});
    // a step right from the lead goes past its continuation, and back to it
    CHECK(moveCursor(cv, {1, 0}, 1, 0) == Cursor{3, 0});
    CHECK(moveCursor(cv, {3, 0}, -1, 0) == Cursor{1, 0});
    // a click or a clamp on the continuation puts it on the lead
    CHECK(moveCursor(cv, {2, 0}, 0, 0) == Cursor{1, 0});
    // up and down onto a continuation land on its lead
    Canvas two(6, 2);
    two.at(1, 1).ch = U'中';
    two.at(2, 1).ch = Glyph::continuation();
    CHECK(moveCursor(two, {2, 0}, 0, 1) == Cursor{1, 1});
    CHECK(moveCursor(two, {2, 0}, 0, 0) == Cursor{2, 0});
    // a pair on the last two columns: Right from its lead stays on it
    const Canvas end = withPair(6, 4);
    CHECK(moveCursor(end, {4, 0}, 1, 0) == Cursor{4, 0});
    CHECK(moveCursor(end, {3, 0}, 1, 0) == Cursor{4, 0});
    // past the end means the last column or a lead whose continuation it is
    CHECK(atRowEnd(end, {4, 0}));
    CHECK(atRowEnd(end, {5, 0}));
    CHECK_FALSE(atRowEnd(end, {3, 0}));
    CHECK_FALSE(atRowEnd(cv, {4, 0}));
    CHECK(atRowEnd(cv, {5, 0}));
}

TEST_CASE("typing splits text into clusters by width", "[wide]") {
    using V = std::vector<std::u32string>;
    CHECK(splitClusters(U"a中b") == V{U"a", U"中", U"b"});
    CHECK(splitClusters(U"éx") == V{U"é", U"x"});
    // 👨‍👩‍👧 is three wide clusters, each joiner on the one before it
    CHECK(splitClusters(U"👨‍👩‍👧") == V{U"👨‍", U"👩‍", U"👧"});
    // a lone mark at the start keeps the marks after it; a control takes none
    CHECK(splitClusters(U"́̂a") == V{U"́̂", U"a"});
    CHECK(splitClusters(U"\u0085́") == V{U"\u0085", U"́"});
}

TEST_CASE("what may be typed follows the document's encoding", "[wide]") {
    // CP437: CP437 glyphs only, as before
    CHECK(isTypeable(U"é", TypingPolicy::Cp437));
    CHECK_FALSE(isTypeable(U"中", TypingPolicy::Cp437));
    CHECK_FALSE(isTypeable(U"★", TypingPolicy::Cp437));
    CHECK_FALSE(isTypeable(U"é", TypingPolicy::Cp437));
    // UTF-8: any cluster a UTF-8 .ans holds
    CHECK(isTypeable(U"中", TypingPolicy::Unicode));
    CHECK(isTypeable(U"★", TypingPolicy::Unicode));
    CHECK(isTypeable(U"q̂", TypingPolicy::Unicode));
    CHECK(isTypeable(U"👨‍", TypingPolicy::Unicode));
    CHECK(isTypeable(U"○", TypingPolicy::Unicode));  // CP437 0x09, which a CP437 .ans can't hold
    // controls, C1, the line and paragraph separators are typed in neither
    for (const char32_t c : {char32_t(0x01), char32_t(0x7f), char32_t(0x85), char32_t(0x9b), char32_t(0x2028),
                             char32_t(0x2029)}) {
        CHECK_FALSE(isTypeable(std::u32string(1, c), TypingPolicy::Unicode));
        CHECK_FALSE(isTypeable(std::u32string(1, c), TypingPolicy::Cp437));
    }
    // nor more than 16 code points
    CHECK(isTypeable(U"q" + std::u32string(15, U'̂'), TypingPolicy::Unicode));
    CHECK_FALSE(isTypeable(U"q" + std::u32string(16, U'̂'), TypingPolicy::Unicode));
}

TEST_CASE("typing a wide cluster writes the pair and moves two cells", "[wide]") {
    const Pen pen{Color::indexed(Red), Color::indexed(Blue)};
    Canvas cv(5, 1);
    EditResult r = typeCluster(cv, {0, 0}, U"中", pen, kUnicode);
    CHECK(r.cursor == Cursor{2, 0});
    CHECK(r.notice.empty());
    land(cv, r);
    CHECK(cv.at(0, 0) == Cell{U'中', pen.fg, pen.bg, 0, false});
    CHECK(cv.at(1, 0) == Cell{Glyph::continuation(), pen.fg, pen.bg, 0, false});
    // a CP437 document refuses it and says which, changing nothing
    r = typeCluster(Canvas(5, 1), {0, 0}, U"中", pen, {});
    CHECK(r.edit.empty());
    CHECK(r.rejected == U"中");
    CHECK(r.cursor == Cursor{0, 0});
    // onto the last two columns: the cursor stays on the lead, past the end
    r = typeCluster(cv, {3, 0}, U"👍", pen, kUnicode);
    CHECK(r.cursor == Cursor{3, 0, true});
    land(cv, r);
    CHECK(cv.at(3, 0).ch == Glyph(U'👍'));
    CHECK(cv.at(4, 0).ch.isContinuation());
    // and typing again there overwrites that pair
    r = typeCluster(cv, r.cursor, U"国", pen, kUnicode);
    CHECK(r.cursor == Cursor{3, 0, true});
    land(cv, r);
    CHECK(cv.at(3, 0).ch == Glyph(U'国'));
    CHECK(cv.at(4, 0).ch.isContinuation());
    // so does a key's text, which the app types with replaceText
    r = replaceText(cv, r.cursor, 0, 0, U"中", pen, Cell{}, kUnicode);
    CHECK(r.cursor == Cursor{3, 0, true});
    CHECK(r.notice.empty());
    land(cv, r);
    CHECK(cv.at(3, 0).ch == Glyph(U'中'));
    CHECK(cv.at(4, 0).ch.isContinuation());
    // and a narrow one there is written on the lead, as typed on it
    r = replaceText(cv, r.cursor, 0, 0, U"a", pen, Cell{}, kUnicode);
    CHECK(r.cursor == Cursor{4, 0});
    land(cv, r);
    CHECK(cv.at(3, 0).ch == Glyph(U'a'));
    CHECK(cv.at(4, 0).ch == Glyph(U' '));
    // on the last column it is refused, with a notice
    Canvas row(5, 1);
    r = typeCluster(row, {4, 0}, U"中", pen, kUnicode);
    CHECK(r.edit.empty());
    CHECK(r.cursor == Cursor{4, 0});
    CHECK(r.notice == "中 doesn't fit on the last column; not typed");
    // a one-column canvas has only the last column
    CHECK(typeCluster(Canvas(1, 1), {0, 0}, U"中", pen, kUnicode).edit.empty());
}

TEST_CASE("a cluster typed over a pair's lead keeps the pair whole", "[wide]") {
    const Pen pen;
    // a wide one written over the lead writes its continuation too, even
    // where the continuation's cell doesn't change
    Canvas cv = withPair(4, 0);
    cv.at(0, 0).fg = pen.fg;
    cv.at(1, 0).fg = pen.fg;
    EditResult r = typeCluster(cv, {0, 0}, U"国", pen, kUnicode);
    REQUIRE(r.edit.changes.size() == 2);
    CHECK(r.edit.changes[1].x == 1);
    land(cv, r);
    CHECK(cv.at(0, 0).ch == Glyph(U'国'));
    CHECK(cv.at(1, 0).ch.isContinuation());
    // a narrow one breaks it: the continuation becomes a space
    r = typeCluster(cv, {0, 0}, U"a", pen, kUnicode);
    CHECK(r.cursor == Cursor{1, 0});
    land(cv, r);
    CHECK(cv.at(0, 0).ch == Glyph(U'a'));
    CHECK(cv.at(1, 0).ch == Glyph(U' '));
    // a cursor handed a continuation types on its lead
    cv = withPair(4, 1);
    r = typeCluster(cv, {2, 0}, U"x", pen, kUnicode);
    REQUIRE_FALSE(r.edit.empty());
    CHECK(r.edit.changes[0].x == 1);
    // a wide one whose continuation lands on another pair's lead breaks that pair
    cv = withPair(5, 1);
    land(cv, typeCluster(cv, {0, 0}, U"国", pen, kUnicode));
    CHECK(cv.at(0, 0).ch == Glyph(U'国'));
    CHECK(cv.at(1, 0).ch.isContinuation());
    CHECK(cv.at(2, 0).ch == Glyph(U' '));
}

TEST_CASE("a narrow cluster typed over a stored pair's own base keeps it", "[wide]") {
    // the lead's cell doesn't change, only its continuation does (a space),
    // and the settle must not take that for a lead left with its glyph
    for (const Cursor at : {Cursor{0, 0}, Cursor{0, 0, true}}) {
        Canvas cv = withPair(2, 0, U'e');
        land(cv, replaceText(cv, at, 0, 0, U"e", Pen{}, Cell{}, kUnicode));
        CHECK(cv.at(0, 0).ch == Glyph(U'e'));
        CHECK(cv.at(1, 0).ch == Glyph(U' '));
        cv = withPair(2, 0, U'e');
        land(cv, typeCluster(cv, at, U"e", Pen{}, kUnicode));
        CHECK(cv.at(0, 0).ch == Glyph(U'e'));
        CHECK(cv.at(1, 0).ch == Glyph(U' '));
    }
    // in another pen the lead's cell changes, and the same holds
    const Pen red{Color::indexed(Red), Color::indexed(Black)};
    Canvas cv = withPair(3, 0, U'e');
    land(cv, replaceText(cv, {0, 0}, 0, 0, U"e", red, Cell{}, kUnicode));
    CHECK(cv.at(0, 0).ch == Glyph(U'e'));
    CHECK(cv.at(0, 0).fg == Color::indexed(Red));
    CHECK(cv.at(1, 0).ch == Glyph(U' '));
    // and in mirror mode, on both sides
    cv = withPair(10, 0, U'e');
    settleWide(cv, mirrorEdit(cv, typeCluster(cv, {0, 0}, U"e", Pen{}, kUnicode).edit)).apply(cv);
    CHECK(cv.at(0, 0).ch == Glyph(U'e'));
    CHECK(cv.at(1, 0).ch == Glyph(U' '));
    CHECK(cv.at(9, 0).ch == Glyph(U'e'));
    CHECK(cv.at(8, 0).ch == Glyph(U' '));
}

TEST_CASE("a lone mark joins the cluster left of the cursor", "[wide]") {
    const Pen pen;
    // NFC-normalised: e then U+0301 is é, in either encoding
    for (const TypingOptions& opts : {TypingOptions{}, kUnicode}) {
        Canvas cv(4, 1);
        land(cv, typeCluster(cv, {0, 0}, U"e", pen, opts));
        const EditResult r = typeMarks(cv, {1, 0}, U"́", opts);
        CHECK(r.cursor == Cursor{1, 0});
        land(cv, r);
        CHECK(cv.at(0, 0).ch == Glyph(U'é'));
    }
    // with no precomposed form it is a cluster, in UTF-8
    Canvas cv(4, 1);
    cv.at(0, 0).ch = U'q';
    cv.at(0, 0).bg = Color::indexed(Red);
    EditResult r = typeMarks(cv, {1, 0}, U"̂", kUnicode);
    land(cv, r);
    CHECK(cv.at(0, 0).ch.isCluster());
    CHECK(cps(cv.at(0, 0).ch) == U"q̂");
    CHECK(cv.at(0, 0).bg == Color::indexed(Red));  // the cell keeps its colours
    // and CP437 refuses it, naming the cluster
    Canvas c437(4, 1);
    c437.at(0, 0).ch = U'q';
    r = typeMarks(c437, {1, 0}, U"̂");
    CHECK(r.edit.empty());
    CHECK(r.rejected == U"q̂");
    // onto a pair: the lead takes it and the pair stays whole
    cv = withPair(4, 0, U'❤');
    r = typeMarks(cv, {2, 0}, U"️", kUnicode);
    land(cv, r);
    CHECK(cps(cv.at(0, 0).ch) == U"❤️");
    CHECK(cv.at(1, 0).ch.isContinuation());
    // and onto a stored pair whose base is narrow, which NFC composes to
    // another base: the pair keeps the width it was stored with
    cv = withPair(4, 0, U'e');
    r = typeMarks(cv, {2, 0}, U"́", kUnicode);
    land(cv, r);
    CHECK(cv.at(0, 0).ch == Glyph(U'é'));
    CHECK(cv.at(1, 0).ch.isContinuation());
    // past the end, the cluster under the cursor: a narrow one on the last
    // column, or a pair's lead
    cv = Canvas(4, 1);
    cv.at(3, 0).ch = U'q';
    land(cv, typeMarks(cv, {3, 0, true}, U"̂", kUnicode));
    CHECK(cps(cv.at(3, 0).ch) == U"q̂");
    cv = withPair(4, 2);
    land(cv, typeMarks(cv, {2, 0, true}, U"̂", kUnicode));
    CHECK(cps(cv.at(2, 0).ch) == U"中̂");
    CHECK(cv.at(3, 0).ch.isContinuation());
    // refused at column 0
    r = typeMarks(Canvas(4, 1), {0, 0}, U"́", kUnicode);
    CHECK(r.edit.empty());
    CHECK(r.notice == "U+0301 has no character before it to join; not typed");
    // and as a 17th code point
    cv = Canvas(4, 1);
    land(cv, typeCluster(cv, {0, 0}, U"q" + std::u32string(15, U'̂'), pen, kUnicode));
    REQUIRE(codePoints(cv.at(0, 0).ch).size == 16);
    r = typeMarks(cv, {1, 0}, U"̂", kUnicode);
    CHECK(r.edit.empty());
    CHECK(r.notice == "U+0302 would be past the 16 code points a cell holds; not typed");
}

TEST_CASE("a cluster typed past the pool's cap is typed as its base", "[wide]") {
    // a cap the pool is already at: no new cluster goes in
    const TypingOptions full{TypingPolicy::Unicode, clusterPool().size()};
    Canvas cv(4, 1);
    const std::size_t before = clusterPool().size();
    EditResult r = typeCluster(cv, {0, 0}, U"w̆̇̈̉̊̋", Pen{}, full);
    CHECK(r.notice == "1 cluster typed as its base: too many distinct clusters");
    land(cv, r);
    CHECK(cv.at(0, 0).ch == Glyph(U'w'));
    CHECK(clusterPool().size() == before);
    // and so is a mark joined past it
    r = typeMarks(cv, {1, 0}, U"̆̇̈̉̊̌", full);
    CHECK(r.notice == "1 cluster typed as its base: too many distinct clusters");
    CHECK(r.edit.empty());
    // an input-method commit counts them
    r = replaceText(Canvas(4, 1), {0, 0}, 0, 0, U"w̆̇̈̉̊̍w̆̇̈̉̊̎",
                    Pen{}, Cell{}, full);
    CHECK(r.notice == "2 clusters typed as their base: too many distinct clusters");
    CHECK(clusterPool().size() == before);
}

TEST_CASE("Backspace and Delete clear the whole pair", "[wide]") {
    Canvas cv = withPair(5, 1);
    EditResult r = backspace(cv, {3, 0});
    CHECK(r.cursor == Cursor{1, 0});
    land(cv, r);
    CHECK(cv.at(1, 0) == Cell{});
    CHECK(cv.at(2, 0) == Cell{});
    cv = withPair(5, 1);
    r = deleteCell(cv, {1, 0});
    CHECK(r.cursor == Cursor{1, 0});
    land(cv, r);
    CHECK(cv.at(1, 0) == Cell{});
    CHECK(cv.at(2, 0) == Cell{});
    // a Delete on the continuation clears the pair from its lead
    cv = withPair(5, 1);
    r = deleteCell(cv, {2, 0});
    CHECK(r.cursor == Cursor{1, 0});
    CHECK(r.edit.changes.size() == 2);
    // past the end on a pair's lead: Backspace clears that pair and stays
    cv = withPair(5, 3);
    r = backspace(cv, {3, 0, true});
    CHECK(r.cursor == Cursor{3, 0});
    land(cv, r);
    CHECK(cv.at(3, 0) == Cell{});
    CHECK(cv.at(4, 0) == Cell{});
    // to a transparent blank on an upper layer
    cv = withPair(5, 1);
    land(cv, backspace(cv, {3, 0}, transparentCell()));
    CHECK(cv.at(1, 0).transparent);
    CHECK(cv.at(2, 0).transparent);
    CHECK_FALSE(cv.at(2, 0).ch.isContinuation());
}

TEST_CASE("an input-method commit is written in clusters", "[wide]") {
    const Pen pen;
    Canvas cv(5, 1);
    EditResult r = replaceText(cv, {0, 0}, 0, 0, U"中a国", pen, Cell{}, kUnicode);
    CHECK(r.cursor == Cursor{3, 0, true});
    CHECK(r.notice.empty());
    land(cv, r);
    CHECK(cv.at(0, 0).ch == Glyph(U'中'));
    CHECK(cv.at(1, 0).ch.isContinuation());
    CHECK(cv.at(2, 0).ch == Glyph(U'a'));
    CHECK(cv.at(3, 0).ch == Glyph(U'国'));
    CHECK(cv.at(4, 0).ch.isContinuation());
    // a wide one that would start on the last column is dropped and reported
    cv = Canvas(5, 1);
    r = replaceText(cv, {0, 0}, 0, 0, U"中中中", pen, Cell{}, kUnicode);
    CHECK(r.notice == "中 would start on the last column; dropped");
    CHECK(r.cursor == Cursor{4, 0});
    land(cv, r);
    CHECK(cv.at(3, 0).ch.isContinuation());
    CHECK(cv.at(4, 0) == Cell{});
    // its clusters with marks are interned as one cell each
    cv = Canvas(5, 1);
    land(cv, replaceText(cv, {0, 0}, 0, 0, U"q̂̃z", pen, Cell{}, kUnicode));
    CHECK(cps(cv.at(0, 0).ch) == U"q̂̃");
    CHECK(cv.at(1, 0).ch == Glyph(U'z'));
    // CP437 refuses one it can't hold, and all of the commit with it
    r = replaceText(Canvas(5, 1), {0, 0}, 0, 0, U"a中", pen);
    CHECK(r.edit.empty());
    CHECK(r.rejected == U"中");
    // a commit that starts with a mark and has more after it is refused
    r = replaceText(Canvas(5, 1), {1, 0}, 0, 0, U"́a", pen, Cell{}, kUnicode);
    CHECK(r.edit.empty());
    CHECK(r.notice == "U+0301 starts text with more after it; not typed");
    // one past a pair it wrote on the last two columns goes on that pair's
    // lead, as typed there, not on the continuation it just wrote
    cv = Canvas(2, 1);
    r = replaceText(cv, {0, 0}, 0, 0, U"中a", pen, Cell{}, kUnicode);
    CHECK(r.cursor == Cursor{1, 0});
    land(cv, r);
    CHECK(cv.at(0, 0).ch == Glyph(U'a'));
    CHECK(cv.at(1, 0).ch == Glyph(U' '));
    // a commit dropped whole leaves the cursor as it was, past the end too
    cv = Canvas(5, 1);
    cv.at(4, 0).ch = U'a';
    r = replaceText(cv, {4, 0, true}, 0, 0, U"中", pen, Cell{}, kUnicode);
    CHECK(r.edit.empty());
    CHECK(r.notice == "中 would start on the last column; dropped");
    CHECK(r.cursor == Cursor{4, 0, true});
}

TEST_CASE("an input-method replacement range is counted in clusters", "[wide]") {
    const Pen pen;
    // 中 at 0-1 and a at 2, the cursor after them: two clusters back is
    // column 0, and the two replaced take three cells
    Canvas cv = withPair(5, 0);
    cv.at(2, 0).ch = U'a';
    EditResult r = replaceText(cv, {3, 0}, -2, 2, U"x", pen, Cell{}, kUnicode);
    CHECK(r.cursor == Cursor{1, 0});
    land(cv, r);
    CHECK(cv.at(0, 0).ch == Glyph(U'x'));
    CHECK(cv.at(1, 0) == Cell{});
    CHECK(cv.at(2, 0) == Cell{});
    // past the end on a pair's lead the range counts back from after its
    // continuation: one cluster back is the pair
    cv = withPair(5, 3);
    r = replaceText(cv, {3, 0, true}, -1, 1, U"国", pen, Cell{}, kUnicode);
    CHECK(r.cursor == Cursor{3, 0, true});
    land(cv, r);
    CHECK(cv.at(3, 0).ch == Glyph(U'国'));
    CHECK(cv.at(4, 0).ch.isContinuation());
    // a range forward from the cursor steps over a pair as one
    cv = withPair(5, 1);
    cv.at(3, 0).ch = U'b';
    r = replaceText(cv, {0, 0}, 1, 1, U"", pen, Cell{}, kUnicode);
    land(cv, r);
    CHECK(cv.at(1, 0) == Cell{});
    CHECK(cv.at(2, 0) == Cell{});
    CHECK(cv.at(3, 0).ch == Glyph(U'b'));
}

TEST_CASE("a pair typed in mirror mode is mirrored whole", "[wide]") {
    Canvas cv(10, 1);
    const EditResult r = typeCluster(cv, {0, 0}, U"中", Pen{}, kUnicode);
    int unmirrored = -1;
    settleWide(cv, mirrorEdit(cv, r.edit, &unmirrored)).apply(cv);
    CHECK(unmirrored == 0);
    CHECK(cv.at(0, 0).ch == Glyph(U'中'));
    CHECK(cv.at(1, 0).ch.isContinuation());
    CHECK(cv.at(8, 0).ch == Glyph(U'中'));
    CHECK(cv.at(9, 0).ch.isContinuation());
    // typed over with the same pen, and a mark joined to it: both images
    // stay whole, though neither continuation's cell changes
    settleWide(cv, mirrorEdit(cv, typeCluster(cv, {0, 0}, U"国", Pen{}, kUnicode).edit)).apply(cv);
    CHECK(cv.at(0, 0).ch == Glyph(U'国'));
    CHECK(cv.at(1, 0).ch.isContinuation());
    CHECK(cv.at(8, 0).ch == Glyph(U'国'));
    CHECK(cv.at(9, 0).ch.isContinuation());
    settleWide(cv, mirrorEdit(cv, typeMarks(cv, {2, 0}, U"̂", kUnicode).edit)).apply(cv);
    CHECK(cps(cv.at(0, 0).ch) == U"国̂");
    CHECK(cv.at(1, 0).ch.isContinuation());
    CHECK(cps(cv.at(8, 0).ch) == U"国̂");
    CHECK(cv.at(9, 0).ch.isContinuation());
    // one over the centre column of an odd width gets no copy
    Canvas odd(9, 1);
    settleWide(odd, mirrorEdit(odd, typeCluster(odd, {3, 0}, U"中", Pen{}, kUnicode).edit, &unmirrored)).apply(odd);
    CHECK(unmirrored == 1);
    CHECK(odd.at(3, 0).ch == Glyph(U'中'));
    CHECK(odd.at(4, 0).ch.isContinuation());
    // a lone mark joined in mirror mode joins the mirrored cluster too
    Canvas marks(10, 1);
    marks.at(0, 0).ch = U'q';
    marks.at(9, 0).ch = U'q';
    settleWide(marks, mirrorEdit(marks, typeMarks(marks, {1, 0}, U"̂", kUnicode).edit)).apply(marks);
    CHECK(cps(marks.at(9, 0).ch) == U"q̂");
}

TEST_CASE("a pair typed on a layer is fitted whole or not at all", "[wide]") {
    const Pen pen;
    // alpha lock: the lead would make a clear cell opaque, so neither half
    // is written, though the continuation's cell is opaque already
    Layer l = makeLayer(LayerKind::Cells, 4, 1, "top");
    for (int x = 0; x < 4; ++x)
        l.cells.at(x, 0) = transparentCell();
    land(l.cells, typeCluster(l.cells, {1, 0}, U"A", pen, kUnicode));
    l.props.alphaLocked = true;
    std::optional<CellEdit> e = fitLayerEdit(l, typeCluster(l.cells, {0, 0}, U"中", pen, kUnicode).edit);
    REQUIRE(e);
    CHECK(e->empty());
    // and both are where both cells are opaque
    land(l.cells, typeCluster(l.cells, {2, 0}, U"B", pen, kUnicode));
    e = fitLayerEdit(l, typeCluster(l.cells, {1, 0}, U"中", pen, kUnicode).edit);
    REQUIRE(e);
    CHECK(e->changes.size() == 2);
    // a layer one column left of the canvas: a pair typed in its last
    // column would lose its continuation off the layer, so neither half is
    Layer m = makeLayer(LayerKind::Cells, 4, 1, "moved", -1);
    const Canvas shown = layerCanvas(m, 4, 1);
    e = fitLayerEdit(m, typeCluster(shown, {2, 0}, U"中", pen, kUnicode).edit);
    REQUIRE(e);
    CHECK(e->empty());
    e = fitLayerEdit(m, typeCluster(shown, {1, 0}, U"中", pen, kUnicode).edit);
    REQUIRE(e);
    CHECK(e->changes.size() == 2);
}

namespace {

// `canvasEdit` as Document::edit lands it on `l`: fitted, then settled.
std::optional<CellEdit> fitted(const Layer& l, const CellEdit& canvasEdit) {
    std::optional<CellEdit> e = fitLayerEdit(l, canvasEdit);
    if (e)
        *e = settleMarks(l.cells, settleWide(l.cells, std::move(*e)));
    return e;
}

// Yellow on blue, the pair's colours below.
Cell pairCell(Glyph g) { return Cell{g, Color::indexed(Yellow), Color::indexed(Blue), 0, false}; }

} // namespace

TEST_CASE("a pair moved or pasted past a layer's edge leaves a space in its colours", "[wide]") {
    // four cells one column left of the canvas: a pair at its columns 1-2
    // (canvas 0-1) and Z at 3 (canvas 2); canvas column 3 is off the layer
    Layer l = makeLayer(LayerKind::Cells, 4, 1, "shifted", -1);
    l.cells.at(1, 0) = pairCell(U'中');
    l.cells.at(2, 0) = pairCell(Glyph::continuation());
    l.cells.at(3, 0) = Cell{Glyph(U'Z')};
    const Canvas was = l.cells;
    const Canvas shown = layerCanvas(l, 4, 1);
    REQUIRE(shown.at(0, 0).ch == Glyph(U'中'));
    const auto check = [&](const CellEdit& canvasEdit, bool cleared) {
        const std::optional<CellEdit> e = fitted(l, canvasEdit);
        REQUIRE(e);
        e->apply(l.cells);
        // the lead lands on Z and its continuation off the layer: a space
        // in the pair's colours, as at the canvas edge
        CHECK(l.cells.at(3, 0) == pairCell(U' '));
        CHECK(l.cells.at(1, 0) == (cleared ? transparentCell() : was.at(1, 0)));
        CHECK(l.cells.at(2, 0) == (cleared ? transparentCell() : was.at(2, 0)));
        const Canvas landed = l.cells;
        e->revert(l.cells);
        CHECK(l.cells == was);
        e->apply(l.cells);
        CHECK(l.cells == landed);
    };
    SECTION("moved two columns right") {
        check(moveSelection(shown, Selection({0, 0, 2, 1}), 2, 0, PasteMode::Normal, transparentCell()).edit, true);
    }
    SECTION("pasted two columns right") {
        check(pasteBlock(shown, copyBlock(shown, Selection({0, 0, 2, 1})), 2, 0, PasteMode::Normal), false);
    }
}

TEST_CASE("a pair pasted half onto an alpha lock's clear cell leaves a space in its colours", "[wide]") {
    Layer l = makeLayer(LayerKind::Cells, 4, 1, "locked");
    l.cells.at(0, 0) = Cell{Glyph(U'Z')};
    l.props.alphaLocked = true;
    CellBlock pair(2, 1);
    pair.at(0, 0) = pairCell(U'中');
    pair.at(1, 0) = pairCell(Glyph::continuation());
    const std::optional<CellEdit> e = fitted(l, pasteBlock(l.cells, pair, 0, 0, PasteMode::Normal));
    REQUIRE(e);
    e->apply(l.cells);
    CHECK(l.cells.at(0, 0) == pairCell(U' '));
    CHECK(l.cells.at(1, 0).transparent);
    // typed, the same pair is refused whole
    const std::optional<CellEdit> typed =
        fitLayerEdit(l, typeCluster(l.cells, {0, 0}, U"中", Pen{Color::indexed(Yellow), Color::indexed(Blue)}, kUnicode).edit);
    REQUIRE(typed);
    CHECK(typed->empty());
}

TEST_CASE("a typed pair refused in mirror mode leaves the art under it", "[wide]") {
    // opaque pairs at 1-2 and 3-4, the rest clear; 国 typed at 0 would
    // write its lead on a clear cell, so alpha lock refuses it and its
    // image at 4-5, and neither may take the pairs beside it with it
    Layer l = makeLayer(LayerKind::Cells, 6, 1, "locked");
    l.cells.at(1, 0) = pairCell(U'中');
    l.cells.at(2, 0) = pairCell(Glyph::continuation());
    l.cells.at(3, 0) = pairCell(U'中');
    l.cells.at(4, 0) = pairCell(Glyph::continuation());
    l.props.alphaLocked = true;
    const Canvas was = l.cells;
    const CellEdit typed = typeCluster(l.cells, {0, 0}, U"国", Pen{}, kUnicode).edit;
    int unmirrored = -1;
    const std::optional<CellEdit> e = fitted(l, mirrorEdit(l.cells, typed, &unmirrored));
    REQUIRE(e);
    CHECK(unmirrored == 0);
    CHECK(e->empty());
    e->apply(l.cells);
    CHECK(l.cells == was);
    // unlocked, the pair, its image and what each breaks land as before
    l.props.alphaLocked = false;
    const std::optional<CellEdit> open = fitted(l, mirrorEdit(l.cells, typed));
    REQUIRE(open);
    open->apply(l.cells);
    CHECK(l.cells.at(0, 0).ch == Glyph(U'国'));
    CHECK(l.cells.at(1, 0).ch.isContinuation());
    CHECK(l.cells.at(2, 0) == pairCell(U' '));
    CHECK(l.cells.at(3, 0) == pairCell(U' '));
    CHECK(l.cells.at(4, 0).ch == Glyph(U'国'));
    CHECK(l.cells.at(5, 0).ch.isContinuation());
}

TEST_CASE("a pair clipped at a layer's edge onto its own glyph leaves a space in its colours", "[wide]") {
    // as above, but the destination already holds 中 in the pair's colours,
    // so the edit leaves the lead's cell out as unchanged
    Layer l = makeLayer(LayerKind::Cells, 4, 1, "shifted", -1);
    l.cells.at(1, 0) = pairCell(U'中');
    l.cells.at(2, 0) = pairCell(Glyph::continuation());
    l.cells.at(3, 0) = pairCell(U'中');
    const Canvas was = l.cells;
    const Canvas shown = layerCanvas(l, 4, 1);
    const auto check = [&](const CellEdit& canvasEdit, bool cleared) {
        const std::optional<CellEdit> e = fitted(l, canvasEdit);
        REQUIRE(e);
        e->apply(l.cells);
        CHECK(l.cells.at(3, 0) == pairCell(U' '));
        CHECK(l.cells.at(1, 0) == (cleared ? transparentCell() : was.at(1, 0)));
        CHECK(l.cells.at(2, 0) == (cleared ? transparentCell() : was.at(2, 0)));
        const Canvas landed = l.cells;
        e->revert(l.cells);
        CHECK(l.cells == was);
        e->apply(l.cells);
        CHECK(l.cells == landed);
    };
    SECTION("moved two columns right") {
        check(moveSelection(shown, Selection({0, 0, 2, 1}), 2, 0, PasteMode::Normal, transparentCell()).edit, true);
    }
    SECTION("pasted two columns right") {
        check(pasteBlock(shown, copyBlock(shown, Selection({0, 0, 2, 1})), 2, 0, PasteMode::Normal), false);
    }
    SECTION("pasted in mirror mode, its image's continuation off the layer") {
        // six columns, the layer one left of the canvas: canvas 5 is off it.
        // A pair pasted at canvas 0-1 has its image at 4-5, its lead on the
        // layer's lone 中 and its continuation clipped.
        Layer m = makeLayer(LayerKind::Cells, 6, 1, "shifted", -1);
        m.cells.at(5, 0) = pairCell(U'中');
        const Canvas mwas = m.cells;
        const Canvas mshown = layerCanvas(m, 6, 1);
        CellBlock pair(2, 1);
        pair.at(0, 0) = pairCell(U'中');
        pair.at(1, 0) = pairCell(Glyph::continuation());
        const std::optional<CellEdit> e =
            fitted(m, mirrorEdit(mshown, pasteBlock(mshown, pair, 0, 0, PasteMode::Normal)));
        REQUIRE(e);
        e->apply(m.cells);
        CHECK(m.cells.at(1, 0) == pairCell(U'中'));
        CHECK(m.cells.at(2, 0) == pairCell(Glyph::continuation()));
        CHECK(m.cells.at(5, 0) == pairCell(U' '));
        const Canvas landed = m.cells;
        e->revert(m.cells);
        CHECK(m.cells == mwas);
        e->apply(m.cells);
        CHECK(m.cells == landed);
    }
}

TEST_CASE("a pair pasted, moved or flipped over a pair in the same colours stays whole", "[wide]") {
    // 中 at 0-1 and 国 at 2-3, both yellow on blue: a continuation landing
    // on one is unchanged, but its pair is new
    Canvas cv(4, 1);
    cv.at(0, 0) = pairCell(U'中');
    cv.at(1, 0) = pairCell(Glyph::continuation());
    cv.at(2, 0) = pairCell(U'国');
    cv.at(3, 0) = pairCell(Glyph::continuation());
    SECTION("pasted") {
        CellBlock b(2, 1);
        b.at(0, 0) = pairCell(U'国');
        b.at(1, 0) = pairCell(Glyph::continuation());
        settleWide(cv, pasteBlock(cv, b, 0, 0, PasteMode::Normal)).apply(cv);
        CHECK(cv.at(0, 0) == pairCell(U'国'));
        CHECK(cv.at(1, 0) == pairCell(Glyph::continuation()));
    }
    SECTION("moved") {
        settleWide(cv, moveSelection(cv, Selection({2, 0, 2, 1}), -2, 0, PasteMode::Normal, Cell{}).edit).apply(cv);
        CHECK(cv.at(0, 0) == pairCell(U'国'));
        CHECK(cv.at(1, 0) == pairCell(Glyph::continuation()));
        CHECK(cv.at(2, 0) == Cell{});
        CHECK(cv.at(3, 0) == Cell{});
    }
    SECTION("flipped") {
        settleWide(cv, transformSelection(cv, Selection({0, 0, 4, 1}), Transform::FlipHorizontal, Cell{}).edit)
            .apply(cv);
        CHECK(cv.at(0, 0) == pairCell(U'国'));
        CHECK(cv.at(1, 0) == pairCell(Glyph::continuation()));
        CHECK(cv.at(2, 0) == pairCell(U'中'));
        CHECK(cv.at(3, 0) == pairCell(Glyph::continuation()));
    }
}

TEST_CASE("mirror typing finds an unchanged pair lead", "[wide]") {
    Layer l = makeLayer(LayerKind::Cells, 6, 1, "cells");
    l.cells = Canvas(6, 1);
    l.cells.at(0, 0).ch = U'中'; // legacy lone glyph
    l.cells.at(3, 0).ch = U'Z';
    const auto typed = typeCluster(l.cells, {0, 0}, U"中", Pen{}, kUnicode).edit;
    REQUIRE(typed.changes.size() == 1);
    REQUIRE(typed.changes.front().x == 1);
    const auto e = fitted(l, mirrorEdit(l.cells, typed));
    REQUIRE(e);
    e->apply(l.cells);
    CHECK(l.cells.at(4, 0).ch == Glyph(U'中'));
    CHECK(l.cells.at(5, 0).ch.isContinuation());
}

TEST_CASE("mirror cleanup does not reserve the image", "[wide]") {
    Layer l = makeLayer(LayerKind::Cells, 6, 1, "cells");
    l.cells = withPair(6, 2);
    const auto typed = typeCluster(l.cells, {1, 0}, U"国", Pen{}, kUnicode).edit;
    const auto e = fitted(l, mirrorEdit(l.cells, typed));
    REQUIRE(e);
    e->apply(l.cells);
    CHECK(l.cells.at(1, 0).ch == Glyph(U'国'));
    CHECK(l.cells.at(2, 0).ch.isContinuation());
    CHECK(l.cells.at(3, 0).ch == Glyph(U'国'));
    CHECK(l.cells.at(4, 0).ch.isContinuation());
}

TEST_CASE("mirror typing over an unchanged lead clears the pair its image lands on", "[wide]") {
    // a lone 中 at 0 typed over as a pair, so the edit leaves its lead out,
    // and 国 at 3-4: the image at 4-5 lands on that pair's continuation,
    // and the settle makes its lead a space
    Layer l = makeLayer(LayerKind::Cells, 6, 1, "cells");
    l.cells = Canvas(6, 1);
    l.cells.at(0, 0).ch = U'中';
    l.cells.at(3, 0).ch = U'国';
    l.cells.at(4, 0).ch = Glyph::continuation();
    const auto typed = typeCluster(l.cells, {0, 0}, U"中", Pen{}, kUnicode).edit;
    REQUIRE(typed.changes.size() == 1);
    const auto e = fitted(l, mirrorEdit(l.cells, typed));
    REQUIRE(e);
    e->apply(l.cells);
    CHECK(l.cells.at(0, 0).ch == Glyph(U'中'));
    CHECK(l.cells.at(1, 0).ch.isContinuation());
    CHECK(l.cells.at(3, 0).ch == Glyph(U' '));
    CHECK(l.cells.at(4, 0).ch == Glyph(U'中'));
    CHECK(l.cells.at(5, 0).ch.isContinuation());
}

TEST_CASE("mirror typing keeps every pair whole from every column over every pair", "[wide]") {
    enum class Under { Nothing, LoneLead, Pair, PairLeft, PairRight };
    for (const int w : {6, 7})
        for (int x = 0; x < w; ++x)
            for (const Under under : {Under::Nothing, Under::LoneLead, Under::Pair, Under::PairLeft, Under::PairRight}) {
                CAPTURE(w, x, int(under));
                Layer l = makeLayer(LayerKind::Cells, w, 1, "cells");
                l.cells = Canvas(w, 1);
                const auto pairAt = [&](int at) {
                    if (at < 0 || at + 1 >= w)
                        return false;
                    l.cells.at(at, 0).ch = U'国';
                    l.cells.at(at + 1, 0).ch = Glyph::continuation();
                    return true;
                };
                bool set = true;
                switch (under) {
                case Under::Nothing: break;
                case Under::LoneLead: l.cells.at(x, 0).ch = U'中'; break;
                case Under::Pair: set = pairAt(x); break;
                case Under::PairLeft: set = pairAt(x - 1); break;
                case Under::PairRight: set = pairAt(x + 1); break;
                }
                if (!set)
                    continue;
                const Canvas was = l.cells;
                const CellEdit typed = typeCluster(l.cells, {x, 0}, U"中", Pen{}, kUnicode).edit;
                const auto e = fitted(l, mirrorEdit(l.cells, typed));
                REQUIRE(e);
                e->apply(l.cells);
                const auto wide = [](Glyph g) { return g == Glyph(U'中') || g == Glyph(U'国'); };
                for (int c = 0; c < w; ++c) {
                    CAPTURE(c);
                    const Glyph g = l.cells.at(c, 0).ch;
                    if (g.isContinuation()) {
                        // no lone continuation
                        CHECK(c > 0);
                        CHECK((c > 0 && wide(l.cells.at(c - 1, 0).ch)));
                    } else if (wide(g) && !(l.cells.at(c, 0) == was.at(c, 0) && under == Under::LoneLead)) {
                        // every lead with its continuation, but for the
                        // lone one typed nowhere near
                        CHECK((c + 1 < w && l.cells.at(c + 1, 0).ch.isContinuation()));
                    }
                }
                // the pair typed, wherever the cursor put it, and its image
                // when neither overlaps the other or the edge
                int lead = -1;
                for (const CellChange& c : typed.changes)
                    if (c.after.ch == Glyph(U'中') && lead < 0)
                        lead = c.x;
                if (lead < 0 && under == Under::LoneLead && x + 1 < w)
                    lead = x;  // only its continuation changed
                if (lead < 0 || lead + 1 >= w)
                    continue;
                CHECK(l.cells.at(lead, 0).ch == Glyph(U'中'));
                CHECK(l.cells.at(lead + 1, 0).ch.isContinuation());
                const int ml = mirrorColumn(w, lead + 1);
                if (std::abs(ml - lead) >= 2) {
                    CHECK(l.cells.at(ml, 0).ch == Glyph(U'中'));
                    CHECK(l.cells.at(ml + 1, 0).ch.isContinuation());
                }
            }
}

TEST_CASE("an input method's UTF-16 range is counted in whole clusters", "[wide]") {
    // X, q + U+0302 (two units) and 😀 as a pair (a surrogate pair, two
    // units), then blanks: from column 4, the boundaries are 5, 4, 2 and 0
    // units back
    Canvas cv(8, 1);
    land(cv, typeCluster(cv, {0, 0}, U"X", Pen{}, kUnicode));
    land(cv, typeCluster(cv, {1, 0}, U"q", Pen{}, kUnicode));
    land(cv, typeMarks(cv, {2, 0}, U"̂", kUnicode));
    land(cv, typeCluster(cv, {2, 0}, U"😀", Pen{}, kUnicode));
    REQUIRE(cps(cv.at(1, 0).ch) == U"q̂");
    REQUIRE(cv.at(3, 0).ch.isContinuation());
    const Cursor c{4, 0};
    CHECK(clusterRange(cv, c, 0, 0) == ClusterRange{0, 0});
    CHECK(clusterRange(cv, c, -2, 2) == ClusterRange{-1, 1});
    CHECK(clusterRange(cv, c, -4, 4) == ClusterRange{-2, 2});
    CHECK(clusterRange(cv, c, -5, 5) == ClusterRange{-3, 3});
    // a range starting or ending inside a surrogate pair or among a
    // cluster's marks takes the whole cluster
    CHECK(clusterRange(cv, c, -1, 1) == ClusterRange{-1, 1});
    CHECK(clusterRange(cv, c, -2, 1) == ClusterRange{-1, 1});
    CHECK(clusterRange(cv, c, -3, 1) == ClusterRange{-2, 1});
    CHECK(clusterRange(cv, c, -3, 2) == ClusterRange{-2, 2});
    CHECK(clusterRange(cv, c, -5, 2) == ClusterRange{-3, 2});
    // forward from column 0, and off the row a column a unit, as before
    CHECK(clusterRange(cv, {0, 0}, 1, 2) == ClusterRange{1, 1});
    CHECK(clusterRange(cv, {0, 0}, 2, 1) == ClusterRange{1, 1});
    CHECK(clusterRange(cv, {0, 0}, 3, 1) == ClusterRange{2, 1});
    CHECK(clusterRange(cv, c, -7, 1) == ClusterRange{-5, 1});
    CHECK(clusterRange(cv, c, 0, 10) == ClusterRange{0, 10});
    // past the end, from after the cluster under the cursor
    CHECK(clusterRange(cv, {7, 0, true}, -1, 1) == ClusterRange{-1, 1});
    // replaceText given the converted range replaces 😀 alone
    const ClusterRange r = clusterRange(cv, c, -2, 2);
    land(cv, replaceText(cv, c, r.start, r.length, U"B", Pen{}, Cell{}, kUnicode));
    CHECK(cps(cv.at(1, 0).ch) == U"q̂");
    CHECK(cv.at(2, 0).ch == Glyph(U'B'));
    CHECK(cv.at(3, 0) == Cell{});
}

TEST_CASE("typing writes the pen's formatting", "[irc][attrs]") {
    // #350 slice 3b: a typed cell carries Pen::attrs as the glyph brush's does
    const std::uint8_t bu = attr::Bold | attr::Underline;
    const Pen pen{Color::indexed(Yellow), Color::indexed(Blue), bu};
    Canvas cv(6, 1);
    // a key's glyph (the F-key sets)
    EditResult r = typeGlyph(cv, {0, 0}, U'A', pen);
    REQUIRE(r.edit.changes.size() == 1);
    CHECK(r.edit.changes[0].after == Cell{U'A', pen.fg, pen.bg, bu, false});
    land(cv, r);
    // a wide cluster: both cells of the pair
    r = typeCluster(cv, r.cursor, U"中", pen, kUnicode);
    land(cv, r);
    CHECK(cv.at(1, 0) == Cell{U'中', pen.fg, pen.bg, bu, false});
    CHECK(cv.at(2, 0) == Cell{Glyph::continuation(), pen.fg, pen.bg, bu, false});
    // a lone mark joins the cell and keeps its formatting
    land(cv, typeCluster(cv, {3, 0}, U"e", pen, kUnicode));
    land(cv, typeMarks(cv, {4, 0}, U"́", kUnicode));
    CHECK(cv.at(3, 0) == Cell{U'é', pen.fg, pen.bg, bu, false});

    // an input-method commit: each cluster written, a cleared cell blank
    const Pen italic{Color::indexed(Red), Color::indexed(Black), attr::Italic};
    Canvas row(6, 1);
    for (int x = 0; x < 6; ++x)
        row.at(x, 0) = Cell{U'z', Color::indexed(Grey), Color::indexed(Black), attr::Underline, false};
    r = replaceText(row, {0, 0}, 0, 4, U"ab中", italic, Cell{}, kUnicode);
    land(row, r);
    CHECK(row.at(0, 0) == Cell{U'a', italic.fg, italic.bg, attr::Italic, false});
    CHECK(row.at(1, 0) == Cell{U'b', italic.fg, italic.bg, attr::Italic, false});
    CHECK(row.at(2, 0) == Cell{U'中', italic.fg, italic.bg, attr::Italic, false});
    CHECK(row.at(3, 0) == Cell{Glyph::continuation(), italic.fg, italic.bg, attr::Italic, false});
    CHECK(row.at(4, 0).attrs == attr::Underline);  // outside the range: as it was

    // a pen without formatting types over a formatted cell without it
    land(row, typeGlyph(row, {0, 0}, U'q', Pen{}));
    CHECK(row.at(0, 0) == Cell{U'q', Pen{}.fg, Pen{}.bg, 0, false});
    land(row, replaceText(row, {1, 0}, 0, 1, U"r", Pen{}, Cell{}, kUnicode));
    CHECK(row.at(1, 0) == Cell{U'r', Pen{}.fg, Pen{}.bg, 0, false});
    // and Backspace and Delete clear to the blank, which has none
    land(row, backspace(row, {2, 0}));
    CHECK(row.at(1, 0) == Cell{});
    land(row, deleteCell(row, {2, 0}));
    CHECK(row.at(2, 0) == Cell{});
    CHECK(row.at(3, 0) == Cell{});
}
