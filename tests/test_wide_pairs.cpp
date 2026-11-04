// Wide pairs (PLAN §16, slice 2): a wide glyph's lead and the continuation
// right of it are one thing to every edit, transform and the compositor.
// Each path has a test with a pair and one with a lone wide cell, a wide
// glyph with no continuation, which is an ordinary glyph to every rule.
// The tests build pairs with Glyph::continuation().

#include "blockwright/brush.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/composite.h"
#include "blockwright/edit.h"
#include "blockwright/glyph.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"
#include "blockwright/selection.h"
#include "blockwright/style_draw.h"
#include "blockwright/text_style.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace bw;

namespace {

const Glyph kCont = Glyph::continuation();

Cell cell(Glyph g, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = g;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

// A canvas of `w` by `h` spaces, grey on black.
Canvas blank(int w, int h = 1) { return Canvas(w, h); }

// Puts 中 at (x, y) and its continuation at (x + 1, y), both fg on bg.
void putPair(Canvas& c, int x, int y, std::uint8_t fg = Yellow, std::uint8_t bg = Blue) {
    c.at(x, y) = cell(U'中', fg, bg);
    c.at(x + 1, y) = cell(kCont, fg, bg);
}

// One change of `c`'s cell (x, y) to `after`.
CellEdit change(const Canvas& c, int x, int y, const Cell& after) {
    CellEdit e;
    e.changes.push_back({x, y, c.at(x, y), after});
    return e;
}

Canvas applied(Canvas c, const CellEdit& e) {
    e.apply(c);
    return c;
}

// The canvas with the edit settled (settleWide) and applied.
Canvas settled(const Canvas& c, const CellEdit& e) { return applied(c, settleWide(c, e)); }

std::string glyphs(const Canvas& c, int y = 0) {
    std::string s;
    for (int x = 0; x < c.width(); ++x) {
        const Glyph g = c.at(x, y).ch;
        if (g.isContinuation())
            s += '+';
        else if (g == Glyph(U'中'))
            s += 'W';
        else if (const auto cp = g.codePoint(); cp && *cp < 0x80)
            s += char(*cp);
        else
            s += '?';
    }
    return s;
}

bool sameColours(const Cell& a, const Cell& b) { return a.fg == b.fg && a.bg == b.bg && a.attrs == b.attrs; }

// Every continuation has a lead on its left, in the lead's colours.
bool pairsWhole(const Canvas& c) {
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (c.at(x, y).ch.isContinuation()) {
                if (x == 0 || c.at(x - 1, y).ch.isContinuation())
                    return false;
                if (!sameColours(c.at(x - 1, y), c.at(x, y)))
                    return false;
            }
    return true;
}

LayerStack twoLayers(int w, int h = 1) {
    LayerStack s(w, h);
    s.add(makeLayer(LayerKind::Cells, w, h, "bottom"));
    s.add(makeLayer(LayerKind::Cells, w, h, "top"));
    // the top layer starts clear
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            const_cast<Layer&>(s.layers()[1]).cells.at(x, y) = transparentCell();
    return s;
}

Layer& layer(LayerStack& s, std::size_t i) { return const_cast<Layer&>(s.layers()[i]); }

} // namespace

// ---- the settle ------------------------------------------------------------

TEST_CASE("settleWide: overwriting a pair's lead leaves its continuation a space in the pair's colours", "[wide][settle]") {
    Canvas c = blank(6);
    putPair(c, 2, 0, Yellow, Blue);
    const Canvas out = settled(c, change(c, 2, 0, cell(U'A', White, Red)));
    CHECK(glyphs(out) == "  A   ");
    CHECK(out.at(3, 0).fg == Color::indexed(Yellow));
    CHECK(out.at(3, 0).bg == Color::indexed(Blue));
}

TEST_CASE("settleWide: overwriting a pair's continuation leaves its lead a space in the pair's colours", "[wide][settle]") {
    Canvas c = blank(6);
    putPair(c, 2, 0, Yellow, Blue);
    const Canvas out = settled(c, change(c, 3, 0, cell(U'B', White, Red)));
    CHECK(glyphs(out) == "   B  ");
    CHECK(out.at(2, 0).fg == Color::indexed(Yellow));
    CHECK(out.at(2, 0).bg == Color::indexed(Blue));
}

TEST_CASE("settleWide: clearing one half of a pair breaks it", "[wide][settle]") {
    Canvas c = blank(6);
    putPair(c, 1, 0, Yellow, Blue);
    const Canvas out = settled(c, clearSelection(c, Selection({2, 0, 1, 1})));
    CHECK(glyphs(out) == "      ");
    CHECK(out.at(1, 0).bg == Color::indexed(Blue));
}

TEST_CASE("settleWide: a stray continuation becomes a space in its own colours", "[wide][settle]") {
    SECTION("in column 0") {
        Canvas c = blank(4);
        const Canvas out = settled(c, change(c, 0, 0, cell(kCont, Red, Green)));
        CHECK(glyphs(out) == "    ");
        CHECK(out.at(0, 0).fg == Color::indexed(Red));
        CHECK(out.at(0, 0).bg == Color::indexed(Green));
    }
    SECTION("right of another continuation") {
        Canvas c = blank(6);
        putPair(c, 1, 0);
        const Canvas out = settled(c, change(c, 3, 0, cell(kCont, Red, Green)));
        CHECK(glyphs(out) == " W+   ");
        CHECK(out.at(3, 0).fg == Color::indexed(Red));
        CHECK(out.at(3, 0).bg == Color::indexed(Green));
    }
}

TEST_CASE("settleWide: a colour, attribute or mark changed on one half is changed on both", "[wide][settle]") {
    Canvas c = blank(6);
    putPair(c, 2, 0, Yellow, Blue);
    SECTION("the lead recoloured") {
        Cell lead = c.at(2, 0);
        lead.fg = Color::indexed(LightRed);
        lead.attrs = attr::Blink;
        const Canvas out = settled(c, change(c, 2, 0, lead));
        CHECK(glyphs(out) == "  W+  ");
        CHECK(out.at(3, 0).fg == Color::indexed(LightRed));
        CHECK(out.at(3, 0).bg == Color::indexed(Blue));
        CHECK(out.at(3, 0).attrs == attr::Blink);
    }
    SECTION("the continuation recoloured") {
        Cell cont = c.at(3, 0);
        cont.bg = Color::indexed(Magenta);
        const Canvas out = settled(c, change(c, 3, 0, cont));
        CHECK(glyphs(out) == "  W+  ");
        CHECK(out.at(2, 0).bg == Color::indexed(Magenta));
        CHECK(out.at(2, 0).fg == Color::indexed(Yellow));
    }
    SECTION("a mark set on the lead") {
        Cell lead = c.at(2, 0);
        lead.dithered = true;
        const Canvas out = settled(c, change(c, 2, 0, lead));
        CHECK(out.at(3, 0).dithered);
    }
    SECTION("both halves written differently: the lead's values win") {
        Cell lead = c.at(2, 0), cont = c.at(3, 0);
        lead.fg = Color::indexed(White);
        cont.fg = Color::indexed(Black);
        CellEdit e = change(c, 2, 0, lead);
        e.changes.push_back({3, 0, c.at(3, 0), cont});
        const Canvas out = settled(c, e);
        CHECK(glyphs(out) == "  W+  ");
        CHECK(out.at(3, 0).fg == Color::indexed(White));
    }
}

TEST_CASE("settleWide: a new pair written whole stays whole", "[wide][settle]") {
    Canvas c = blank(6);
    CellEdit e = change(c, 1, 0, cell(U'中', Yellow, Blue));
    e.changes.push_back({2, 0, c.at(2, 0), cell(kCont, Yellow, Blue)});
    const Canvas out = settled(c, e);
    CHECK(glyphs(out) == " W+   ");
    CHECK(pairsWhole(out));
}

TEST_CASE("settleWide: a lone wide cell is an ordinary glyph", "[wide][settle][lone]") {
    Canvas c = blank(6);
    c.at(2, 0) = cell(U'中', Yellow, Blue);
    for (const int x : {1, 3}) {
        const CellEdit e = change(c, x, 0, cell(U'x', White, Red));
        const CellEdit s = settleWide(c, e);
        CHECK(s == e);
        CHECK(applied(c, s).at(2, 0) == c.at(2, 0));
    }
}

TEST_CASE("settleWide changes nothing on a canvas without pairs", "[wide][settle]") {
    Canvas c = blank(6, 2);
    c.at(1, 0) = cell(U'A', White, Red);
    CellEdit e = change(c, 1, 0, cell(U'B', Yellow, Blue));
    e.changes.push_back({4, 1, c.at(4, 1), cell(U'░', Red, Black)});
    CHECK(settleWide(c, e) == e);
}

TEST_CASE("settleWide keeps an edit's own flags and undoes exactly", "[wide][settle]") {
    Canvas c = blank(6);
    putPair(c, 2, 0);
    CellEdit e = change(c, 2, 0, cell(U'A', White, Red));
    e.moves = true;
    const CellEdit s = settleWide(c, e);
    CHECK(s.moves);
    Canvas out = applied(c, s);
    s.revert(out);
    CHECK(out == c);
}

// ---- brushes ---------------------------------------------------------------

TEST_CASE("a brush over one half of a pair: the settle breaks or recolours the pair", "[wide][brush]") {
    Canvas c = blank(6);
    putPair(c, 2, 0, Yellow, Blue);
    const BrushSettings s{1, Tip::Square, 1.0, PressureTo::Nothing};
    SECTION("a glyph on the continuation") {
        const CellEdit e = paintStroke(c, GlyphBrush(cell(U'#', White, Red)), s, {Dab{3, 0}});
        const Canvas out = settled(c, e);
        CHECK(glyphs(out) == "   #  ");
        CHECK(out.at(2, 0).bg == Color::indexed(Blue));
    }
    SECTION("colours on the lead") {
        const CellEdit e =
            paintStroke(c, ColorizeBrush(Color::indexed(LightGreen), Color::indexed(Red)), s, {Dab{2, 0}});
        const Canvas out = settled(c, e);
        CHECK(glyphs(out) == "  W+  ");
        CHECK(out.at(3, 0).fg == Color::indexed(LightGreen));
        CHECK(out.at(3, 0).bg == Color::indexed(Red));
    }
}

TEST_CASE("a brush beside a lone wide cell leaves it alone", "[wide][brush][lone]") {
    Canvas c = blank(6);
    c.at(2, 0) = cell(U'中', Yellow, Blue);
    const BrushSettings s{1, Tip::Square, 1.0, PressureTo::Nothing};
    const CellEdit e = paintStroke(c, GlyphBrush(cell(U'#', White, Red)), s, {Dab{3, 0}});
    const Canvas out = settled(c, e);
    CHECK(glyphs(out) == "  W#  ");
}

// ---- pixel layers ----------------------------------------------------------

TEST_CASE("a pixel layer can't hold a wide cell", "[wide][pixels]") {
    const Layer px = makeLayer(LayerKind::Pixels, 6, 1);
    Canvas c = layerCanvas(px, 6, 1);
    SECTION("a pair") {
        CellEdit e = change(c, 1, 0, cell(U'中', Yellow, Blue));
        e.changes.push_back({2, 0, c.at(2, 0), cell(kCont, Yellow, Blue)});
        CHECK_FALSE(fitLayerEdit(px, e));
    }
    SECTION("a continuation alone") { CHECK_FALSE(fitLayerEdit(px, change(c, 2, 0, cell(kCont, Yellow, Blue)))); }
    SECTION("a lone wide cell") { CHECK_FALSE(fitLayerEdit(px, change(c, 1, 0, cell(U'中', Yellow, Blue)))); }
}

// ---- selections, edges and transforms --------------------------------------

TEST_CASE("copyBlock copies a cut half of a pair as a space in the pair's colours", "[wide][copy]") {
    Canvas c = blank(6);
    putPair(c, 2, 0, Yellow, Blue);
    SECTION("the lead alone") {
        const CellBlock b = copyBlock(c, Selection({0, 0, 3, 1}));
        CHECK(b.at(2, 0).ch == Glyph(U' '));
        CHECK(b.at(2, 0).bg == Color::indexed(Blue));
        CHECK(b.at(2, 0).fg == Color::indexed(Yellow));
    }
    SECTION("the continuation alone") {
        const CellBlock b = copyBlock(c, Selection({3, 0, 3, 1}));
        CHECK(b.at(0, 0).ch == Glyph(U' '));
        CHECK(b.at(0, 0).bg == Color::indexed(Blue));
    }
    SECTION("both halves") {
        const CellBlock b = copyBlock(c, Selection({2, 0, 2, 1}));
        CHECK(b.at(0, 0).ch == Glyph(U'中'));
        CHECK(b.at(1, 0).ch.isContinuation());
    }
}

TEST_CASE("copyBlock keeps a lone wide cell", "[wide][copy][lone]") {
    Canvas c = blank(6);
    c.at(2, 0) = cell(U'中', Yellow, Blue);
    CHECK(copyBlock(c, Selection({2, 0, 1, 1})).at(0, 0).ch == Glyph(U'中'));
}

namespace {

CellBlock pairBlock() {
    CellBlock b(3, 1);
    b.at(0, 0) = cell(U'中', Yellow, Blue);
    b.at(1, 0) = cell(kCont, Yellow, Blue);
    b.at(2, 0) = cell(U'A', White, Red);
    return b;
}

} // namespace

TEST_CASE("a paste clipped at the right edge writes a cut pair's lead as a space", "[wide][paste]") {
    const Canvas c = blank(6);
    SECTION("paste") {
        const Canvas out = applied(c, pasteBlock(c, pairBlock(), 5, 0, PasteMode::Normal));
        CHECK(glyphs(out) == "      ");
        CHECK(out.at(5, 0).bg == Color::indexed(Blue));
    }
    SECTION("stamp") {
        const Canvas out = applied(c, stampBlock(c, pairBlock(), 5, 0, PasteMode::Normal));
        CHECK(glyphs(out) == "      ");
        CHECK(out.at(5, 0).bg == Color::indexed(Blue));
    }
    SECTION("whole, a cell further left") {
        const Canvas out = applied(c, pasteBlock(c, pairBlock(), 3, 0, PasteMode::Normal));
        CHECK(glyphs(out) == "   W+A");
        CHECK(pairsWhole(out));
    }
    SECTION("its continuation alone, off the left edge") {
        const Canvas out = applied(c, pasteBlock(c, pairBlock(), -1, 0, PasteMode::Normal));
        CHECK(glyphs(out) == " A    ");
        CHECK(out.at(0, 0).bg == Color::indexed(Blue));
    }
}

TEST_CASE("a paste that writes one half of a pair writes it as a space", "[wide][paste]") {
    // Under mode writes only blank cells of the canvas: here the lead's.
    Canvas c = blank(6);
    c.at(3, 0) = cell(U'Z', White, Red);
    const Canvas out = applied(c, pasteBlock(c, pairBlock(), 2, 0, PasteMode::Under));
    CHECK(glyphs(out) == "   ZA ");
    CHECK(out.at(2, 0).bg == Color::indexed(Blue));
}

TEST_CASE("a lone wide cell pastes at the right edge as itself", "[wide][paste][lone]") {
    const Canvas c = blank(6);
    CellBlock b(1, 1);
    b.at(0, 0) = cell(U'中', Yellow, Blue);
    CHECK(glyphs(applied(c, pasteBlock(c, b, 5, 0, PasteMode::Normal))) == "     W");
}

TEST_CASE("a move keeps a pair whole, and clips it at the edge as a paste does", "[wide][move]") {
    Canvas c = blank(8);
    putPair(c, 1, 0, Yellow, Blue);
    SECTION("inside the canvas") {
        const SelectionEdit r = moveSelection(c, Selection({1, 0, 2, 1}), 3, 0);
        CHECK(glyphs(applied(c, r.edit)) == "    W+  ");
    }
    SECTION("to the right edge") {
        const SelectionEdit r = moveSelection(c, Selection({1, 0, 2, 1}), 6, 0);
        const Canvas out = applied(c, r.edit);
        CHECK(glyphs(out) == "        ");
        CHECK(out.at(7, 0).bg == Color::indexed(Blue));
    }
    SECTION("half the pair selected") {
        const SelectionEdit r = moveSelection(c, Selection({2, 0, 1, 1}), 3, 0);
        const Canvas out = settled(c, r.edit);
        CHECK(glyphs(out) == "        ");
        CHECK(out.at(5, 0).bg == Color::indexed(Blue));
        CHECK(out.at(1, 0).bg == Color::indexed(Blue));
    }
}

TEST_CASE("a lone wide cell survives a move", "[wide][move][lone]") {
    Canvas c = blank(8);
    c.at(1, 0) = cell(U'中', Yellow, Blue);
    const SelectionEdit r = moveSelection(c, Selection({1, 0, 1, 1}), 6, 0);
    CHECK(glyphs(settled(c, r.edit)) == "       W");
}

TEST_CASE("a horizontal flip keeps a pair's lead on the left", "[wide][flip]") {
    SECTION("transformed") {
        const CellBlock b = transformed(pairBlock(), Transform::FlipHorizontal);
        CHECK(b.at(0, 0).ch == Glyph(U'A'));
        CHECK(b.at(1, 0).ch == Glyph(U'中'));
        CHECK(b.at(2, 0).ch.isContinuation());
    }
    SECTION("in place") {
        Canvas c = blank(6);
        putPair(c, 1, 0);
        c.at(3, 0) = cell(U'/', White, Red);
        const SelectionEdit r = transformSelection(c, Selection({0, 0, 4, 1}), Transform::FlipHorizontal);
        const Canvas out = applied(c, r.edit);
        CHECK(glyphs(out) == "\\W+   ");
        CHECK(pairsWhole(out));
    }
    SECTION("flipped twice is the block again") {
        CHECK(transformed(transformed(pairBlock(), Transform::FlipHorizontal), Transform::FlipHorizontal) ==
              pairBlock());
    }
}

TEST_CASE("a vertical flip keeps pairs whole", "[wide][flip]") {
    Canvas c = blank(4, 2);
    putPair(c, 1, 0);
    const SelectionEdit r = transformSelection(c, Selection({0, 0, 4, 2}), Transform::FlipVertical);
    const Canvas out = applied(c, r.edit);
    CHECK(glyphs(out, 0) == "    ");
    CHECK(glyphs(out, 1) == " W+ ");
    CHECK(pairsWhole(out));
}

TEST_CASE("a lone wide cell survives both flips", "[wide][flip][lone]") {
    Canvas c = blank(4, 2);
    c.at(0, 0) = cell(U'中', Yellow, Blue);
    const Canvas h = applied(c, transformSelection(c, Selection({0, 0, 4, 2}), Transform::FlipHorizontal).edit);
    CHECK(glyphs(h, 0) == "   W");
    const Canvas v = applied(c, transformSelection(c, Selection({0, 0, 4, 2}), Transform::FlipVertical).edit);
    CHECK(glyphs(v, 1) == "W   ");
}

TEST_CASE("rotation refuses a selection that holds either half of a pair, all or nothing", "[wide][rotate]") {
    Canvas c = blank(6, 4);
    putPair(c, 2, 1);
    c.at(0, 0) = cell(U'A', White, Red);
    for (const Transform t : {Transform::RotateClockwise, Transform::RotateCounterClockwise})
        for (const CellRect r : {CellRect{0, 0, 4, 3}, CellRect{0, 0, 3, 3}, CellRect{3, 0, 2, 3}}) {
            INFO("rect " << r.x << "," << r.y << " " << r.width << "x" << r.height);
            const Selection sel(r);
            CHECK(selectionHoldsPair(c, sel));
            const SelectionEdit e = transformSelection(c, sel, t);
            CHECK(e.edit.empty());
            CHECK(e.selection == sel);
        }
    CHECK(blockHoldsPair(pairBlock()));
    CHECK_FALSE(selectionHoldsPair(c, Selection({0, 0, 2, 4})));
}

TEST_CASE("a lone wide cell survives a rotation", "[wide][rotate][lone]") {
    Canvas c = blank(4, 4);
    c.at(1, 0) = cell(U'中', Yellow, Blue);
    const Selection sel({0, 0, 2, 1});
    CHECK_FALSE(selectionHoldsPair(c, sel));
    const SelectionEdit e = transformSelection(c, sel, Transform::RotateClockwise);
    const Canvas out = applied(c, e.edit);
    CHECK(out.at(0, 1).ch == Glyph(U'中'));
    CellBlock b(1, 1);
    b.at(0, 0) = c.at(1, 0);
    CHECK_FALSE(blockHoldsPair(b));
}

// ---- mirror mode -----------------------------------------------------------

namespace {

CellEdit writePair(const Canvas& c, int x, std::uint8_t fg = Yellow, std::uint8_t bg = Blue) {
    CellEdit e = change(c, x, 0, cell(U'中', fg, bg));
    e.changes.push_back({x + 1, 0, c.at(x + 1, 0), cell(kCont, fg, bg)});
    return e;
}

} // namespace

TEST_CASE("mirror mode puts the mirrored pair's lead at mirrorColumn(x + 1)", "[wide][mirror]") {
    for (const int w : {9, 10}) {
        INFO("width " << w);
        const Canvas c = blank(w);
        int unmirrored = -1;
        const Canvas out = applied(c, mirrorEdit(c, writePair(c, 1), &unmirrored));
        CHECK(out.at(mirrorColumn(w, 2), 0).ch == Glyph(U'中'));
        CHECK(out.at(mirrorColumn(w, 1), 0).ch.isContinuation());
        CHECK(pairsWhole(out));
        CHECK(unmirrored == 0);
    }
}

TEST_CASE("mirror mode on an odd width gives a pair over its own image no copy, and counts it", "[wide][mirror]") {
    const Canvas c = blank(9);  // the centre column is 4
    for (const int x : {3, 4}) {
        INFO("lead at " << x);
        int unmirrored = 0;
        const Canvas out = applied(c, mirrorEdit(c, writePair(c, x), &unmirrored));
        CHECK(unmirrored == 1);
        CHECK(out.at(x, 0).ch == Glyph(U'中'));
        CHECK(out.at(x + 1, 0).ch.isContinuation());
        CHECK(pairsWhole(out));
        int n = 0;
        for (int i = 0; i < 9; ++i)
            n += out.at(i, 0).ch == Glyph(U'中');
        CHECK(n == 1);
    }
}

TEST_CASE("mirror mode on an even width: a pair across the axis is its own image", "[wide][mirror]") {
    const Canvas c = blank(10);  // the axis is between 4 and 5
    int unmirrored = -1;
    const Canvas out = applied(c, mirrorEdit(c, writePair(c, 4), &unmirrored));
    CHECK(glyphs(out) == "    W+    ");
    CHECK(unmirrored == 0);
}

TEST_CASE("mirror mode mirrors a recoloured pair whole", "[wide][mirror]") {
    Canvas c = blank(10);
    putPair(c, 1, 0, Yellow, Blue);
    putPair(c, 7, 0, Yellow, Blue);
    Cell lead = c.at(1, 0);
    lead.bg = Color::indexed(Red);
    const Canvas out = applied(c, mirrorEdit(c, change(c, 1, 0, lead)));
    CHECK(glyphs(out) == " W+    W+ ");
    for (const int x : {1, 2, 7, 8})
        CHECK(out.at(x, 0).bg == Color::indexed(Red));
}

TEST_CASE("mirror mode mirrors a lone wide cell as a narrow one", "[wide][mirror][lone]") {
    const Canvas c = blank(9);
    int unmirrored = -1;
    const Canvas out = applied(c, mirrorEdit(c, change(c, 1, 0, cell(U'中', Yellow, Blue)), &unmirrored));
    CHECK(glyphs(out) == " W     W ");
    CHECK(unmirrored == 0);
}

// ---- the eyedropper --------------------------------------------------------

TEST_CASE("the eyedropper on either half of a pair picks the pair's colours", "[wide][eyedropper]") {
    Canvas c = blank(4);
    c.at(1, 0) = cell(U'中', Yellow, Blue);
    c.at(2, 0) = cell(kCont, Red, Green);  // not settled: the lead's are the pair's
    for (const int x : {1, 2}) {
        INFO("column " << x);
        CHECK(pickColour(c, x, 0, false) == Color::indexed(Yellow));
        CHECK(pickColour(c, x, 1, true) == Color::indexed(Blue));
    }
}

TEST_CASE("the eyedropper on a lone wide cell picks its own colours", "[wide][eyedropper][lone]") {
    Canvas c = blank(4);
    c.at(1, 0) = cell(U'中', Yellow, Blue);
    c.at(2, 0) = cell(U'x', Red, Green);
    CHECK(pickColour(c, 1, 0, false) == Color::indexed(Yellow));
    CHECK(pickColour(c, 2, 0, false) == Color::indexed(Red));
}

// ---- compositing -----------------------------------------------------------

TEST_CASE("composite: a pair shows only where both halves show", "[wide][composite]") {
    LayerStack s = twoLayers(6);
    putPair(layer(s, 0).cells, 2, 0, Yellow, Blue);
    SECTION("nothing over it") {
        const Canvas out = composite(s);
        CHECK(glyphs(out) == "  W+  ");
    }
    SECTION("its continuation covered") {
        layer(s, 1).cells.at(3, 0) = cell(U'X', White, Red);
        const Canvas out = composite(s);
        CHECK(glyphs(out) == "   X  ");
        CHECK(out.at(2, 0).fg == Color::indexed(Yellow));
        CHECK(out.at(2, 0).bg == Color::indexed(Blue));
    }
    SECTION("its lead covered") {
        layer(s, 1).cells.at(2, 0) = cell(U'X', White, Red);
        const Canvas out = composite(s);
        CHECK(glyphs(out) == "  X   ");
        CHECK(out.at(3, 0).bg == Color::indexed(Blue));
    }
    SECTION("a chars-only layer over one half") {
        layer(s, 1).props.mode = LayerMode::CharsOnly;
        layer(s, 1).cells.at(3, 0) = cell(U'X', White, Red);
        const Canvas out = composite(s);
        CHECK(glyphs(out) == "   X  ");
        CHECK(out.at(3, 0).bg == Color::indexed(Blue));
    }
    SECTION("another pair over it, a cell to the right") {
        putPair(layer(s, 1).cells, 3, 0, White, Red);
        const Canvas out = composite(s);
        CHECK(glyphs(out) == "   W+ ");
        CHECK(out.at(2, 0).bg == Color::indexed(Blue));
        CHECK(pairsWhole(out));
    }
    SECTION("its lead on the last column, its continuation off the canvas") {
        LayerStack t(4, 1);
        Layer wide = makeLayer(LayerKind::Cells, 6, 1);
        putPair(wide.cells, 3, 0, Yellow, Blue);
        t.add(std::move(wide));
        const Canvas out = composite(t);
        CHECK(glyphs(out) == "    ");
        CHECK(out.at(3, 0).bg == Color::indexed(Blue));
    }
}

TEST_CASE("composite: a colours-only layer over one half of a pair recolours both", "[wide][composite]") {
    for (const int x : {2, 3}) {
        INFO("over column " << x);
        LayerStack s = twoLayers(6);
        putPair(layer(s, 0).cells, 2, 0, Yellow, Blue);
        layer(s, 1).props.mode = LayerMode::ColoursOnly;
        layer(s, 1).cells.at(x, 0) = cell(U'X', White, Red);
        const Canvas out = composite(s);
        CHECK(glyphs(out) == "  W+  ");
        for (const int i : {2, 3}) {
            CHECK(out.at(i, 0).fg == Color::indexed(White));
            CHECK(out.at(i, 0).bg == Color::indexed(Red));
        }
    }
}

TEST_CASE("composite: a lone wide cell shows whatever covers the cell beside it", "[wide][composite][lone]") {
    LayerStack s = twoLayers(6);
    layer(s, 0).cells.at(2, 0) = cell(U'中', Yellow, Blue);
    layer(s, 1).cells.at(3, 0) = cell(U'X', White, Red);
    CHECK(glyphs(composite(s)) == "  WX  ");
}

TEST_CASE("compositeCell and compositeCellBelow agree with composite beside a pair", "[wide][composite]") {
    LayerStack s = twoLayers(6);
    putPair(layer(s, 0).cells, 2, 0, Yellow, Blue);
    s.add(makeLayer(LayerKind::Cells, 6, 1, "third"));
    for (int x = 0; x < 6; ++x)
        layer(s, 2).cells.at(x, 0) = transparentCell();
    layer(s, 1).cells.at(3, 0) = cell(U'X', White, Red);
    const Canvas all = composite(s);
    for (int x = 0; x < 6; ++x) {
        INFO("column " << x);
        CHECK(identical(compositeCell(s, x, 0), all.at(x, 0)));
        CHECK(identical(compositeCellBelow(s, 2, x, 0), all.at(x, 0)));
    }
    CHECK(compositeCellBelow(s, 1, 2, 0).ch == Glyph(U'中'));
}

TEST_CASE("recomposite takes in a column each side of its rect", "[wide][composite]") {
    LayerStack s = twoLayers(6);
    putPair(layer(s, 0).cells, 2, 0, Yellow, Blue);
    Canvas out = composite(s);
    REQUIRE(glyphs(out) == "  W+  ");
    layer(s, 1).cells.at(3, 0) = cell(U'X', White, Red);
    recomposite(s, out, {3, 0, 1, 1});
    CHECK(glyphs(out) == "   X  ");
    CHECK(identical(out.at(2, 0), composite(s).at(2, 0)));
    layer(s, 1).cells.at(3, 0) = transparentCell();
    recomposite(s, out, {3, 0, 1, 1});
    CHECK(glyphs(out) == "  W+  ");
}

TEST_CASE("compositeRect of part of a row agrees with composite beside a pair", "[wide][composite]") {
    LayerStack s = twoLayers(8, 2);
    putPair(layer(s, 0).cells, 2, 0, Yellow, Blue);
    putPair(layer(s, 0).cells, 5, 1, Yellow, Blue);
    layer(s, 1).cells.at(3, 0) = cell(U'X', White, Red);
    const Canvas all = composite(s);
    for (int x0 = 0; x0 < 8; ++x0)
        for (int w = 1; x0 + w <= 8; ++w) {
            const Canvas part = compositeRect(s, {x0, 0, w, 2});
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < w; ++x)
                    CHECK(identical(part.at(x, y), all.at(x0 + x, y)));
        }
}

TEST_CASE("a styled layer's cells are settled after its style is drawn", "[wide][composite][style]") {
    for (const EffectSettings& fx :
         {EffectSettings{FillEffect{FillMode::Flat, LightRed, "fire"}}, EffectSettings{OutlineEffect{OutlineForm::Cells, Blue}},
          EffectSettings{ShadowEffect{ShadowForm::Drop, Compass::W, 1, DarkGrey}},
          EffectSettings{ShadowEffect{ShadowForm::Drop, Compass::E, 1, DarkGrey}},
          EffectSettings{PlateEffect{Blue, 1}}, EffectSettings{FillEffect{FillMode::Columns, Grey, "fire"}},
          EffectSettings{FillEffect{FillMode::Rainbow, Grey, "fire"}}, EffectSettings{GlowEffect{"fire", 2, 3}},
          EffectSettings{ShadowEffect{ShadowForm::Extrude, Compass::W, 2, DarkGrey}},
          EffectSettings{ShadeEffect{"fire", Compass::W}}, EffectSettings{BevelEffect{"fire", Compass::E}}}) {
        INFO("effect " << fx.index());
        Layer l = makeLayer(LayerKind::Cells, 10, 3);
        for (int x = 0; x < 10; ++x)
            for (int y = 0; y < 3; ++y)
                l.cells.at(x, y) = transparentCell();
        l.cells.at(2, 1) = cell(U'█', White, Black);
        putPair(l.cells, 3, 1, Yellow, Black);
        l.cells.at(5, 1) = cell(U'█', White, Black);
        l.props.style = TextStyle{{Effect{true, fx}}};
        REQUIRE(styledAtComposite(l.props));
        const Canvas styled = styledCells(l, true);
        CHECK(pairsWhole(styled));
        LayerStack s(10, 3);
        s.add(l);
        CHECK(pairsWhole(composite(s)));
    }
}
