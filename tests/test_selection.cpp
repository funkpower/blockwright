// Selections, paste modes, flips, rotations, glyph remaps and mirror mode.

#include "blockwright/selection.h"

#include "blockwright/cp437.h"
#include "blockwright/layer.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace bw;

namespace {

Cell glyph(char32_t ch, std::uint8_t fg = Grey, std::uint8_t bg = Black) {
    return Cell{ch, Color::indexed(fg), Color::indexed(bg), 0, false};
}

// A canvas whose row y holds `rows[y]`, grey on blue so no cell is blank.
Canvas canvasOf(const std::vector<std::u32string>& rows) {
    Canvas cv(static_cast<int>(rows[0].size()), static_cast<int>(rows.size()));
    for (int y = 0; y < cv.height(); ++y)
        for (int x = 0; x < cv.width(); ++x)
            cv.at(x, y) = glyph(rows[y][x], Grey, Blue);
    return cv;
}

std::u32string row(const Canvas& cv, int y) {
    std::u32string s;
    for (int x = 0; x < cv.width(); ++x)
        s += cv.at(x, y).ch;
    return s;
}

Canvas applied(Canvas cv, const CellEdit& e) {
    e.apply(cv);
    return cv;
}

// Each change once, before = the canvas, after != before.
void checkWellFormed(const Canvas& cv, const CellEdit& e) {
    for (size_t i = 0; i < e.changes.size(); ++i) {
        const CellChange& c = e.changes[i];
        REQUIRE(cv.contains(c.x, c.y));
        CHECK(c.before == cv.at(c.x, c.y));
        CHECK_FALSE(identical(c.before, c.after));
        for (size_t j = i + 1; j < e.changes.size(); ++j)
            CHECK_FALSE((e.changes[j].x == c.x && e.changes[j].y == c.y));
    }
}

} // namespace

TEST_CASE("a cell rectangle from two corners, clipped and tested") {
    CHECK(CellRect::fromCorners(3, 4, 1, 2) == CellRect{1, 2, 3, 3});
    CHECK(CellRect::fromCorners(5, 5, 5, 5) == CellRect{5, 5, 1, 1});
    const CellRect r{2, 1, 3, 2};
    CHECK(r.contains(2, 1));
    CHECK(r.contains(4, 2));
    CHECK_FALSE(r.contains(5, 2));
    CHECK_FALSE(r.contains(2, 3));
    CHECK(r.intersected({0, 0, 4, 4}) == CellRect{2, 1, 2, 2});
    CHECK(r.intersected({10, 10, 2, 2}).empty());
    CHECK(CellRect{}.empty());
    CHECK(Selection(CellRect{-2, -2, 5, 5}).clipped(Canvas(2, 2)).bounds() == CellRect{0, 0, 2, 2});
    CHECK(Selection(CellRect{5, 5, 2, 2}).clipped(Canvas(2, 2)).empty());
}

TEST_CASE("blank cells: holes and spaces on black") {
    CHECK(isBlank(Cell{}));
    CHECK(isBlank(glyph(U'\0')));
    CHECK(isBlank(glyph(U' ')));
    CHECK(isBlank(glyph(U' ', Red, Black)));  // the foreground doesn't show
    Cell hole = glyph(U'A');
    hole.transparent = true;
    CHECK(isBlank(hole));
    CHECK_FALSE(isBlank(glyph(U'A')));
    CHECK_FALSE(isBlank(glyph(U' ', Grey, Blue)));
    CHECK_FALSE(isBlank(glyph(U'█', Black, Black)));  // drawn, if invisibly
    Cell blink = Cell{};
    blink.attrs = attr::Blink;  // a bright background under iCE
    CHECK_FALSE(isBlank(blink));
    CHECK(isBlank(Cell{U' ', Color::indexed(Grey), Color::fromRgb({0, 0, 0}), 0, false}));
    CHECK_FALSE(isBlank(Cell{U' ', Color::indexed(Grey), Color::fromRgb({0, 0, 1}), 0, false}));
}

TEST_CASE("copy takes the selected cells, clipped to the canvas") {
    const Canvas cv = canvasOf({U"abcd", U"efgh", U"ijkl"});
    const CellBlock b = copyBlock(cv, Selection(CellRect{1, 1, 2, 2}));
    REQUIRE(b.width == 2);
    REQUIRE(b.height == 2);
    CHECK(b.at(0, 0) == cv.at(1, 1));
    CHECK(b.at(1, 1) == cv.at(2, 2));

    const CellBlock edge = copyBlock(cv, Selection(CellRect{3, 2, 5, 5}));
    CHECK(edge.width == 1);
    CHECK(edge.height == 1);
    CHECK(edge.at(0, 0).ch == U'l');
    CHECK(copyBlock(cv, Selection()).empty());
    CHECK(copyBlock(cv, Selection(CellRect{9, 9, 2, 2})).empty());
}

TEST_CASE("cut is copy plus a clear, and the clear is one edit") {
    const Canvas cv = canvasOf({U"abcd", U"efgh"});
    const Selection sel(CellRect{1, 0, 2, 2});
    const CellEdit e = clearSelection(cv, sel);
    checkWellFormed(cv, e);
    CHECK(e.changes.size() == 4);
    const Canvas out = applied(cv, e);
    CHECK(row(out, 0) == U"a  d");
    CHECK(out.at(1, 0) == Cell{});
    Canvas back = out;
    e.revert(back);
    CHECK(back == cv);
    // clearing blank cells changes nothing
    CHECK(clearSelection(out, sel).empty());
}

TEST_CASE("paste normal writes every cell and drops what falls off the canvas") {
    const Canvas cv = canvasOf({U"....", U"....", U"...."});
    CellBlock b(2, 2);
    b.at(0, 0) = glyph(U'A');
    b.at(1, 0) = Cell{};  // blank: normal mode writes it
    b.at(0, 1) = glyph(U'C');
    b.at(1, 1) = glyph(U'D');

    const CellEdit e = pasteBlock(cv, b, 1, 1, PasteMode::Normal);
    checkWellFormed(cv, e);
    const Canvas out = applied(cv, e);
    CHECK(row(out, 1) == U".A .");
    CHECK(row(out, 2) == U".CD.");

    // off the right and bottom edges
    CHECK(row(applied(cv, pasteBlock(cv, b, 3, 2, PasteMode::Normal)), 2) == U"...A");
    // off the top-left
    const Canvas tl = applied(cv, pasteBlock(cv, b, -1, -1, PasteMode::Normal));
    CHECK(row(tl, 0) == U"D...");
    // entirely off
    CHECK(pasteBlock(cv, b, 4, 0, PasteMode::Normal).empty());
    CHECK(pasteBlock(cv, b, -2, 0, PasteMode::Normal).empty());
    CHECK(pasteBlock(cv, b, 2147483600, 2147483600, PasteMode::Normal).empty());
    CHECK(pasteBlock(cv, CellBlock{}, 0, 0, PasteMode::Normal).empty());
}

TEST_CASE("paste transparent skips the block's blank cells") {
    const Canvas cv = canvasOf({U"xxxx"});
    CellBlock b(4, 1);
    b.at(0, 0) = glyph(U'A');
    b.at(1, 0) = Cell{};                     // space on black: skipped
    b.at(2, 0) = glyph(U' ', Grey, Red);     // space on red: written
    b.at(3, 0) = glyph(U'\0');               // NUL on black: skipped
    const Canvas out = applied(cv, pasteBlock(cv, b, 0, 0, PasteMode::Transparent));
    CHECK(row(out, 0) == U"Ax x");
    CHECK(out.at(1, 0) == cv.at(1, 0));
    CHECK(out.at(2, 0).bg == Color::indexed(Red));
}

TEST_CASE("paste under writes only into the canvas's blank cells") {
    Canvas cv = canvasOf({U"x  x"});
    cv.at(1, 0) = Cell{};
    cv.at(2, 0) = glyph(U' ', Grey, Green);  // a coloured space is drawn
    CellBlock b(4, 1);
    for (int x = 0; x < 4; ++x)
        b.at(x, 0) = glyph(U'A' + x);
    const Canvas out = applied(cv, pasteBlock(cv, b, 0, 0, PasteMode::Under));
    CHECK(row(out, 0) == U"xB x");
}

TEST_CASE("holes in a block never write, in any mode") {
    const Canvas cv = canvasOf({U"xx"});
    CellBlock b(2, 1);
    b.at(0, 0) = glyph(U'A');
    b.at(1, 0) = glyph(U'B');
    b.at(1, 0).transparent = true;
    for (const PasteMode m : {PasteMode::Normal, PasteMode::Transparent})
        CHECK(row(applied(cv, pasteBlock(cv, b, 0, 0, m)), 0) == U"Ax");
}

TEST_CASE("flips remap half blocks, box drawing, slashes and brackets") {
    CHECK(flipGlyph(U'▀', Flip::Vertical) == U'▄');
    CHECK(flipGlyph(U'▄', Flip::Vertical) == U'▀');
    CHECK(flipGlyph(U'▀', Flip::Horizontal) == U'▀');
    CHECK(flipGlyph(U'▌', Flip::Horizontal) == U'▐');
    CHECK(flipGlyph(U'▐', Flip::Horizontal) == U'▌');
    CHECK(flipGlyph(U'▌', Flip::Vertical) == U'▌');

    CHECK(flipGlyph(U'┌', Flip::Horizontal) == U'┐');
    CHECK(flipGlyph(U'└', Flip::Horizontal) == U'┘');
    CHECK(flipGlyph(U'├', Flip::Horizontal) == U'┤');
    CHECK(flipGlyph(U'╔', Flip::Horizontal) == U'╗');
    CHECK(flipGlyph(U'╒', Flip::Horizontal) == U'╕');
    CHECK(flipGlyph(U'╟', Flip::Horizontal) == U'╢');
    CHECK(flipGlyph(U'┬', Flip::Horizontal) == U'┬');
    CHECK(flipGlyph(U'┌', Flip::Vertical) == U'└');
    CHECK(flipGlyph(U'┬', Flip::Vertical) == U'┴');
    CHECK(flipGlyph(U'╒', Flip::Vertical) == U'╘');
    CHECK(flipGlyph(U'╦', Flip::Vertical) == U'╩');
    CHECK(flipGlyph(U'═', Flip::Vertical) == U'═');

    CHECK(flipGlyph(U'/', Flip::Horizontal) == U'\\');
    CHECK(flipGlyph(U'\\', Flip::Vertical) == U'/');
    CHECK(flipGlyph(U'(', Flip::Horizontal) == U')');
    CHECK(flipGlyph(U']', Flip::Horizontal) == U'[');
    CHECK(flipGlyph(U'{', Flip::Horizontal) == U'}');
    CHECK(flipGlyph(U'<', Flip::Horizontal) == U'>');
    CHECK(flipGlyph(U'(', Flip::Vertical) == U'(');
    CHECK(flipGlyph(U'►', Flip::Horizontal) == U'◄');
    CHECK(flipGlyph(U'▲', Flip::Vertical) == U'▼');
    CHECK(flipGlyph(U'¬', Flip::Horizontal) == U'⌐');

    // symmetric glyphs and letters stay
    for (const char32_t ch : std::u32string(U"A b░▒▓█■·─│┼═║╬"))
        CHECK(flipGlyph(ch, Flip::Horizontal) == ch);
}

TEST_CASE("rotation remaps half blocks and box drawing a quarter turn") {
    CHECK(rotateGlyph(U'▀', true) == U'▐');
    CHECK(rotateGlyph(U'▐', true) == U'▄');
    CHECK(rotateGlyph(U'▄', true) == U'▌');
    CHECK(rotateGlyph(U'▌', true) == U'▀');
    CHECK(rotateGlyph(U'▀', false) == U'▌');
    CHECK(rotateGlyph(U'─', true) == U'│');
    CHECK(rotateGlyph(U'═', false) == U'║');
    CHECK(rotateGlyph(U'┌', true) == U'┐');
    CHECK(rotateGlyph(U'┌', false) == U'└');
    CHECK(rotateGlyph(U'╒', true) == U'╖');
    CHECK(rotateGlyph(U'├', true) == U'┬');
    CHECK(rotateGlyph(U'/', true) == U'\\');
    CHECK(rotateGlyph(U'-', true) == U'|');
    CHECK(rotateGlyph(U'▲', true) == U'►');
    CHECK(rotateGlyph(U'░', true) == U'░');
    CHECK(rotateGlyph(U'A', true) == U'A');
}

TEST_CASE("every glyph remap is closed and undoes itself") {
    // all 40 CP437 box glyphs, the half blocks and the paired glyphs
    const std::u32string glyphs =
        U"│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌▀▄▌▐/\\()[]{}<>≤≥►◄▲▼→←↑↓⌐¬-|↔↕░A ";
    for (const char32_t ch : glyphs) {
        INFO(static_cast<std::uint32_t>(ch));
        CHECK(flipGlyph(flipGlyph(ch, Flip::Horizontal), Flip::Horizontal) == ch);
        CHECK(flipGlyph(flipGlyph(ch, Flip::Vertical), Flip::Vertical) == ch);
        CHECK(rotateGlyph(rotateGlyph(ch, true), false) == ch);
        CHECK(rotateGlyph(rotateGlyph(ch, false), true) == ch);
        char32_t r = ch;
        for (int i = 0; i < 4; ++i)
            r = rotateGlyph(r, true);
        CHECK(r == ch);
        // a remap lands on a glyph the remaps know (box glyphs stay box glyphs)
        if (glyphs.find(ch) < 40) {
            CHECK(glyphs.find(flipGlyph(ch, Flip::Horizontal)) < 40);
            CHECK(glyphs.find(flipGlyph(ch, Flip::Vertical)) < 40);
            CHECK(glyphs.find(rotateGlyph(ch, true)) < 40);
        }
    }
    // two flips are a half turn
    for (const char32_t ch : glyphs.substr(0, 44))
        CHECK(flipGlyph(flipGlyph(ch, Flip::Horizontal), Flip::Vertical) == rotateGlyph(rotateGlyph(ch, true), true));
}

TEST_CASE("block flips and rotations move cells and remap glyphs") {
    // ab▀
    // c▌d
    CellBlock b(3, 2);
    const std::u32string cells = U"ab▀c▌d";
    for (int i = 0; i < 6; ++i)
        b.at(i % 3, i / 3) = glyph(cells[i], static_cast<std::uint8_t>(i + 1));

    const CellBlock h = transformed(b, Transform::FlipHorizontal);
    CHECK(h.width == 3);
    CHECK(h.at(0, 0).ch == U'▀');
    CHECK(h.at(2, 0).ch == U'a');
    CHECK(h.at(1, 1).ch == U'▐');
    CHECK(h.at(1, 1).fg == b.at(1, 1).fg);

    const CellBlock v = transformed(b, Transform::FlipVertical);
    CHECK(v.at(0, 0).ch == U'c');
    CHECK(v.at(2, 1).ch == U'▄');

    const CellBlock cw = transformed(b, Transform::RotateClockwise);
    REQUIRE(cw.width == 2);
    REQUIRE(cw.height == 3);
    // c a
    // ▀ b
    // d ▐
    CHECK(cw.at(0, 0).ch == U'c');
    CHECK(cw.at(1, 0).ch == U'a');
    CHECK(cw.at(0, 1).ch == U'▀');  // ▌ turned
    CHECK(cw.at(1, 2).ch == U'▐');  // ▀ turned
    CHECK(cw.at(0, 2).ch == U'd');
    CHECK(cw.at(0, 2).fg == b.at(2, 1).fg);

    const CellBlock ccw = transformed(b, Transform::RotateCounterClockwise);
    REQUIRE(ccw.width == 2);
    // ▌ d
    // b ▄
    // a c
    CHECK(ccw.at(0, 0).ch == U'▌');
    CHECK(ccw.at(1, 1).ch == U'▄');
    CHECK(ccw.at(0, 2).ch == U'a');

    CHECK(transformed(h, Transform::FlipHorizontal) == b);
    CHECK(transformed(v, Transform::FlipVertical) == b);
    CHECK(transformed(cw, Transform::RotateCounterClockwise) == b);
    CellBlock r = b;
    for (int i = 0; i < 4; ++i)
        r = transformed(r, Transform::RotateClockwise);
    CHECK(r == b);
    CHECK(transformed(CellBlock{}, Transform::RotateClockwise).empty());
}

TEST_CASE("a selection flips in place as one edit") {
    const Canvas cv = canvasOf({U"ab▌.", U"cd/.", U"...."});
    const Selection sel(CellRect{0, 0, 3, 2});
    const SelectionEdit e = transformSelection(cv, sel, Transform::FlipHorizontal);
    checkWellFormed(cv, e.edit);
    CHECK(e.selection == sel);
    const Canvas out = applied(cv, e.edit);
    CHECK(row(out, 0) == U"▐ba.");
    CHECK(row(out, 1) == U"\\dc.");
    CHECK(row(out, 2) == U"....");
    Canvas back = out;
    e.edit.revert(back);
    CHECK(back == cv);
}

TEST_CASE("a selection rotates in place from its top-left corner") {
    const Canvas cv = canvasOf({U"abc..", U"def..", U".....", U"....."});
    const SelectionEdit e = transformSelection(cv, Selection(CellRect{0, 0, 3, 2}), Transform::RotateClockwise);
    checkWellFormed(cv, e.edit);
    CHECK(e.selection.bounds() == CellRect{0, 0, 2, 3});
    const Canvas out = applied(cv, e.edit);
    CHECK(row(out, 0) == U"da ..");  // c's old cell is cleared
    CHECK(row(out, 1) == U"eb ..");
    CHECK(row(out, 2) == U"fc...");
    CHECK(out.at(2, 0) == Cell{});

    // turned past the bottom edge: the cells there are dropped, and so is
    // that part of the selection
    const Canvas tall = canvasOf({U"abcd", U"...."});
    const SelectionEdit t = transformSelection(tall, Selection(CellRect{0, 0, 4, 1}), Transform::RotateClockwise);
    CHECK(t.selection.bounds() == CellRect{0, 0, 1, 2});
    const Canvas tout = applied(tall, t.edit);
    CHECK(row(tout, 0) == U"a   ");
    CHECK(row(tout, 1) == U"b...");
}

TEST_CASE("moving a selection leaves blank cells behind, as one edit") {
    const Canvas cv = canvasOf({U"ab..", U"cd..", U"...."});
    const SelectionEdit e = moveSelection(cv, Selection(CellRect{0, 0, 2, 2}), 1, 1);
    checkWellFormed(cv, e.edit);
    CHECK(e.selection.bounds() == CellRect{1, 1, 2, 2});
    const Canvas out = applied(cv, e.edit);
    CHECK(row(out, 0) == U"  ..");
    CHECK(row(out, 1) == U" ab.");
    CHECK(row(out, 2) == U".cd.");
    Canvas back = out;
    e.edit.revert(back);
    CHECK(back == cv);

    // under mode sees the cells the move cleared as blank
    const SelectionEdit u = moveSelection(cv, Selection(CellRect{0, 0, 2, 2}), 1, 0, PasteMode::Under);
    const Canvas uout = applied(cv, u.edit);
    CHECK(row(uout, 0) == U" a..");  // b's cell was cleared by the lift, so a lands; . is drawn
    CHECK(row(uout, 1) == U" c..");

    // off the canvas: only the clear
    const SelectionEdit off = moveSelection(cv, Selection(CellRect{0, 0, 2, 2}), 10, 0);
    CHECK(off.selection.empty());
    CHECK(row(applied(cv, off.edit), 0) == U"  ..");
}

TEST_CASE("a floating block lifted from a selection stamps as one edit") {
    const Canvas cv = canvasOf({U"ab..", U"...."});
    const Selection from(CellRect{0, 0, 2, 1});
    const CellBlock b = copyBlock(cv, from);
    const CellEdit e = stampBlock(cv, b, 2, 1, PasteMode::Normal, &from);
    checkWellFormed(cv, e);
    const Canvas out = applied(cv, e);
    CHECK(row(out, 0) == U"  ..");
    CHECK(row(out, 1) == U"..ab");
    // stamped back where it came from: nothing changes
    CHECK(stampBlock(cv, b, 0, 0, PasteMode::Normal, &from).empty());
}

TEST_CASE("mirror columns are integer reflections") {
    for (int w = 1; w <= 161; ++w) {
        int fixed = 0;
        for (int x = 0; x < w; ++x) {
            const int m = mirrorColumn(w, x);
            CHECK(m >= 0);
            CHECK(m < w);
            CHECK(x + m == w - 1);
            CHECK(mirrorColumn(w, m) == x);
            if (m == x) {
                ++fixed;
                CHECK(x == w / 2);
            }
        }
        // odd widths have one centre column, even widths none
        CHECK(fixed == w % 2);
    }
    CHECK(mirrorColumn(80, 39) == 40);
    CHECK(mirrorColumn(80, 0) == 79);
    CHECK(mirrorColumn(81, 40) == 40);
}

TEST_CASE("mirror mode repeats an edit at the mirrored column with mirrored glyphs") {
    SECTION("even width") {
        const Canvas cv(80, 2);
        const Pen pen{Color::indexed(Red), Color::indexed(Blue)};
        const CellEdit e = mirrorEdit(cv, typeGlyph(cv, {10, 1}, U'▌', pen).edit);
        checkWellFormed(cv, e);
        REQUIRE(e.changes.size() == 2);
        const Canvas out = applied(cv, e);
        CHECK(out.at(10, 1).ch == U'▌');
        CHECK(out.at(69, 1).ch == U'▐');
        CHECK(out.at(69, 1).fg == Color::indexed(Red));
        CHECK(out.at(69, 1).bg == Color::indexed(Blue));
        // the two middle columns mirror each other
        const Canvas mid = applied(cv, mirrorEdit(cv, typeGlyph(cv, {39, 0}, U'/', pen).edit));
        CHECK(mid.at(39, 0).ch == U'/');
        CHECK(mid.at(40, 0).ch == U'\\');
        Canvas back = out;
        e.revert(back);
        CHECK(back == cv);
    }
    SECTION("odd width: the centre column is its own mirror") {
        const Canvas cv(81, 1);
        const CellEdit e = mirrorEdit(cv, typeGlyph(cv, {40, 0}, U'▌', Pen{}).edit);
        REQUIRE(e.changes.size() == 1);
        CHECK(e.changes[0].x == 40);
        CHECK(e.changes[0].after.ch == U'▌');
        const CellEdit side = mirrorEdit(cv, typeGlyph(cv, {39, 0}, U'(', Pen{}).edit);
        REQUIRE(side.changes.size() == 2);
        CHECK(side.changes[1].x == 41);
        CHECK(side.changes[1].after.ch == U')');
    }
    SECTION("width 1") {
        const Canvas cv(1, 1);
        CHECK(mirrorEdit(cv, typeGlyph(cv, {0, 0}, U'A', Pen{}).edit).changes.size() == 1);
    }
    SECTION("an edit spanning both halves keeps its own cells") {
        const Canvas cv = canvasOf({U"......"});
        CellBlock b(4, 1);
        for (int x = 0; x < 4; ++x)
            b.at(x, 0) = glyph(U"ab▌d"[x]);
        const CellEdit e = mirrorEdit(cv, pasteBlock(cv, b, 0, 0, PasteMode::Normal));
        checkWellFormed(cv, e);
        // own: abcd at 0-3; mirrored only where the paste didn't reach: 4 <- 1, 5 <- 0
        CHECK(row(applied(cv, e), 0) == U"ab▌dba");
    }
    SECTION("changes off the canvas are dropped") {
        const Canvas cv(4, 1);
        CellEdit e;
        e.changes.push_back({7, 0, Cell{}, glyph(U'x')});
        CHECK(mirrorEdit(cv, e).empty());
    }
}

TEST_CASE("mirrored strokes leave the canvas symmetric") {
    // Every glyph a stroke writes off the centre comes back flipped on the
    // other side, so the canvas equals its own mirror image there.
    const std::u32string glyphs = U"▀▄▌▐/\\()┌┐└┘├┤╔╗╒╕AB░▓█";
    for (const int w : {1, 2, 3, 7, 8, 79, 80, 81}) {
        Canvas cv(w, 3);
        int i = 0;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < w; x += 2) {
                const Pen pen{Color::indexed(std::uint8_t(i % 16)), Color::indexed(std::uint8_t((i / 3) % 8))};
                mirrorEdit(cv, typeGlyph(cv, {x, y}, glyphs[i % glyphs.size()], pen).edit).apply(cv);
                ++i;
            }
        INFO("width " << w);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < w; ++x) {
                const int m = mirrorColumn(w, x);
                if (m == x)
                    continue;
                Cell flipped = cv.at(m, y);
                flipped.ch = flipGlyph(flipped.ch, Flip::Horizontal);
                CHECK(cv.at(x, y) == flipped);
            }
    }
}

TEST_CASE("flips swap « and » sideways and ⌠ and ⌡ upside down") {
    CHECK(flipGlyph(U'«', Flip::Horizontal) == U'»');
    CHECK(flipGlyph(U'»', Flip::Horizontal) == U'«');
    CHECK(flipGlyph(U'«', Flip::Vertical) == U'«');
    CHECK(flipGlyph(U'⌠', Flip::Vertical) == U'⌡');
    CHECK(flipGlyph(U'⌡', Flip::Vertical) == U'⌠');
    CHECK(flipGlyph(U'⌠', Flip::Horizontal) == U'⌠');
}

TEST_CASE("a half turn is both flips for every CP437 glyph") {
    // #35: (o) turned twice gave )o( glyph by glyph, where both flips give (o)
    for (int b = 0; b < 256; ++b) {
        const char32_t ch = cp437ToUnicode(static_cast<std::uint8_t>(b));
        INFO(b);
        const char32_t h = flipGlyph(ch, Flip::Horizontal), v = flipGlyph(ch, Flip::Vertical);
        CHECK(flipGlyph(h, Flip::Horizontal) == ch);
        CHECK(flipGlyph(v, Flip::Vertical) == ch);
        CHECK(flipGlyph(v, Flip::Horizontal) == flipGlyph(h, Flip::Vertical));
        const char32_t half = rotateGlyph(rotateGlyph(ch, true), true);
        CHECK(half == flipGlyph(v, Flip::Horizontal));
        CHECK(rotateGlyph(rotateGlyph(ch, false), false) == half);
        CHECK(rotateGlyph(rotateGlyph(half, true), true) == ch);
        CHECK(rotateGlyph(rotateGlyph(ch, true), false) == ch);
        // every remap stays in CP437
        CHECK(unicodeToCp437(h));
        CHECK(unicodeToCp437(v));
        CHECK(unicodeToCp437(rotateGlyph(ch, true)));
    }
    // the block: (o) a half turn round is (o) again
    CellBlock b(3, 1);
    b.at(0, 0) = glyph(U'(');
    b.at(1, 0) = glyph(U'o');
    b.at(2, 0) = glyph(U')');
    const CellBlock half = transformed(transformed(b, Transform::RotateClockwise), Transform::RotateClockwise);
    CHECK(half == b);
    CHECK(half == transformed(transformed(b, Transform::FlipHorizontal), Transform::FlipVertical));
}

TEST_CASE("the lost-blink mark goes with its cell through flips, rotations, moves and pastes") {
    // #82: Document::encode counts the marked cells, so every edit that
    // carries a cell carries its mark, and a hole left behind has none
    Canvas cv = canvasOf({U"<AB", U"CDE"});
    cv.at(0, 0).lostBlink = true;
    const auto marks = [](const Canvas& c) {
        std::vector<std::pair<int, int>> at;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                if (c.at(x, y).lostBlink)
                    at.emplace_back(x, y);
        return at;
    };
    using At = std::vector<std::pair<int, int>>;

    SECTION("a flip moves it with the cell and its glyph") {
        const Canvas out = applied(cv, transformSelection(cv, Selection(CellRect{0, 0, 3, 2}),
                                                          Transform::FlipHorizontal).edit);
        CHECK(out.at(2, 0).ch == U'>');
        CHECK(marks(out) == At{{2, 0}});
    }
    SECTION("a rotation") {
        const Canvas out = applied(cv, transformSelection(cv, Selection(CellRect{0, 0, 2, 2}),
                                                          Transform::RotateClockwise).edit);
        CHECK(marks(out) == At{{1, 0}});
    }
    SECTION("a move leaves an unmarked hole") {
        const Canvas out = applied(cv, moveSelection(cv, Selection(CellRect{0, 0, 1, 1}), 1, 1).edit);
        CHECK(marks(out) == At{{1, 1}});
        CHECK(out.at(0, 0) == Cell{});
    }
    SECTION("a move onto a cell that looks the same still moves the mark") {
        Canvas twin = canvasOf({U"AA"});
        twin.at(0, 0).lostBlink = true;
        const CellEdit e = moveSelection(twin, Selection(CellRect{0, 0, 1, 1}), 1, 0).edit;
        checkWellFormed(twin, e);
        const Canvas out = applied(twin, e);
        CHECK(out.at(1, 0) == twin.at(1, 0));
        CHECK(marks(out) == At{{1, 0}});
    }
    SECTION("a copy pasted elsewhere carries a mark of its own") {
        const CellBlock block = copyBlock(cv, Selection(CellRect{0, 0, 1, 1}));
        CHECK(block.at(0, 0).lostBlink);
        const Canvas out = applied(cv, pasteBlock(cv, block, 2, 1, PasteMode::Normal));
        CHECK(marks(out) == At{{0, 0}, {2, 1}});
    }
    SECTION("a cell cleared or pasted over from scratch loses it") {
        CHECK(marks(applied(cv, clearSelection(cv, Selection(CellRect{0, 0, 1, 1})))).empty());
        CellBlock fresh(1, 1);
        fresh.at(0, 0) = glyph(U'Z', Grey, Blue);
        CHECK(marks(applied(cv, pasteBlock(cv, fresh, 0, 0, PasteMode::Normal))).empty());
    }
}

// A pasted half pixel is drawn over the cell under it: where that changes the
// cell's look, the cell drops the gradient's dither mark (Cell::dithered).
TEST_CASE("a half pixel pasted over a dithered cell drops its mark", "[dither]") {
    Canvas cv(1, 1);
    Cell marked{U'\u2591', Color::indexed(Grey), Color::indexed(DarkGrey), 0, false};
    marked.dithered = true;
    cv.at(0, 0) = marked;
    CellBlock b(1, 1);
    b.pixelLayer = true;
    b.at(0, 0) = pixelLayerCell({std::uint8_t(Red), std::nullopt});
    for (const bool pixelLayer : {false, true}) {
        INFO("onto a pixel layer: " << pixelLayer);
        const CellEdit e = pasteBlock(cv, b, 0, 0, PasteMode::Normal, PastePixels{pixelLayer, true});
        REQUIRE(e.changes.size() == 1);
        CHECK_FALSE(e.changes[0].after == marked);
        CHECK_FALSE(e.changes[0].after.dithered);
    }
}
