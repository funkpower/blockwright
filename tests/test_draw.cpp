// The drawing tools' core: the selection as a brush mask, half-block
// pixels (resolve, plot, pencil strokes, fill, eyedropper), the shape
// rasterisers and the shade, glyph and colorize brushes.

#include "blockwright/brush.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"
#include "blockwright/shapes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <climits>
#include <set>
#include <string>

using namespace bw;

namespace {

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

std::set<std::pair<int, int>> asSet(const Pixels& p) { return {p.begin(), p.end()}; }

// A shape drawn as rows of '#' and '.', for the exact small shapes.
std::vector<std::string> picture(const Pixels& p, int w, int h) {
    std::vector<std::string> rows(static_cast<size_t>(h), std::string(static_cast<size_t>(w), '.'));
    for (const auto& [x, y] : p)
        rows[static_cast<size_t>(y)][static_cast<size_t>(x)] = '#';
    return rows;
}

Pixels mirrored(const Pixels& p, int w, int h, bool horizontal) {
    Pixels out;
    for (const auto& [x, y] : p)
        out.emplace_back(horizontal ? w - 1 - x : x, horizontal ? y : h - 1 - y);
    return out;
}

} // namespace

TEST_CASE("selectionMask: nothing selected paints anywhere, else only the selected cells", "[draw][mask]") {
    CHECK_FALSE(selectionMask(Selection{}, 10, 5));

    const auto m = selectionMask(Selection(CellRect{2, 1, 3, 2}), 10, 5);
    REQUIRE(m);
    CHECK(m->width() == 10);
    CHECK(m->height() == 5);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 10; ++x)
            CHECK(m->contains(x, y) == (x >= 2 && x <= 4 && y >= 1 && y <= 2));

    // Partly off the canvas: clipped. Wholly off: a mask that paints nothing.
    const auto edge = selectionMask(Selection(CellRect{8, 3, 5, 5}), 10, 5);
    REQUIRE(edge);
    CHECK(edge->contains(9, 4));
    CHECK_FALSE(edge->contains(7, 4));
    const auto off = selectionMask(Selection(CellRect{20, 20, 2, 2}), 10, 5);
    REQUIRE(off);
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 10; ++x)
            CHECK_FALSE(off->contains(x, y));
}

TEST_CASE("selectionMask limits a stroke to the selection", "[draw][mask]") {
    Canvas c(6, 3);
    const GlyphBrush brush(cell(kFullBlock, White, Black));
    StrokeOptions o;
    o.selection = selectionMask(Selection(CellRect{1, 1, 2, 1}), 6, 3);
    const CellEdit e = paintStroke(c, brush, {}, {Dab{0, 1}, Dab{5, 1}}, o);
    REQUIRE(e.changes.size() == 2);
    CHECK(e.changes[0].x == 1);
    CHECK(e.changes[1].x == 2);
}

TEST_CASE("halfBlocks reads the two colours of pixel cells only", "[draw][halfblock]") {
    CHECK(halfBlocks(Cell{}) == HalfPair{Black, Black});
    CHECK(halfBlocks(cell(U' ', Grey, Blue)) == HalfPair{Blue, Blue});
    CHECK(halfBlocks(cell(U'\0', Grey, Red)) == HalfPair{Red, Red});
    CHECK(halfBlocks(cell(U' ', Grey, Red)) == HalfPair{Red, Red});
    CHECK(halfBlocks(cell(kFullBlock, Yellow, Blue)) == HalfPair{Yellow, Yellow});
    CHECK(halfBlocks(cell(kUpperHalf, Yellow, Blue)) == HalfPair{Yellow, Blue});
    CHECK(halfBlocks(cell(kLowerHalf, Yellow, Blue)) == HalfPair{Blue, Yellow});
    CHECK_FALSE(halfBlocks(cell(U'A', Yellow, Blue)));
    CHECK_FALSE(halfBlocks(cell(kShadeMedium, Yellow, Blue)));

    // RGB colours are quantised to the palette
    Cell rgb = cell(kUpperHalf, Grey, Black);
    rgb.fg = Color::fromRgb({0xfe, 0xfe, 0x50});
    CHECK(halfBlocks(rgb) == HalfPair{Yellow, Black});
}

TEST_CASE("halfBlockCell writes pixels the canonical way", "[draw][halfblock]") {
    // one dark colour: a space on it; black is the blank cell
    CHECK(halfBlockCell({Black, Black}) == Cell{});
    CHECK(halfBlockCell({Blue, Blue}) == cell(U' ', Grey, Blue));
    // one bright colour: a full block on black
    CHECK(halfBlockCell({Yellow, Yellow}) == cell(kFullBlock, Yellow, Black));
    // two colours: ▀ unless only ▄ keeps the background dark
    CHECK(halfBlockCell({Red, Blue}) == cell(kUpperHalf, Red, Blue));
    CHECK(halfBlockCell({Yellow, Blue}) == cell(kUpperHalf, Yellow, Blue));
    CHECK(halfBlockCell({Blue, Yellow}) == cell(kLowerHalf, Yellow, Blue));
    // two bright: a bright background either way
    CHECK(halfBlockCell({White, Yellow}) == cell(kUpperHalf, White, Yellow));

    // every pair reads back as itself, and only two bright colours put a
    // bright colour in the background
    for (std::uint8_t t = 0; t < 16; ++t)
        for (std::uint8_t b = 0; b < 16; ++b) {
            const Cell c = halfBlockCell({t, b});
            CHECK(halfBlocks(c) == HalfPair{t, b});
            CHECK((c.bg.index >= 8) == (t >= 8 && b >= 8 && t != b));
        }
}

TEST_CASE("setHalfBlock sets one half and keeps the other", "[draw][halfblock]") {
    CHECK(setHalfBlock(Cell{}, false, Red) == cell(kUpperHalf, Red, Black));
    CHECK(setHalfBlock(Cell{}, true, Red) == cell(kUpperHalf, Black, Red));
    CHECK(setHalfBlock(cell(kUpperHalf, Red, Black), true, Red) == cell(U' ', Grey, Red));
    CHECK(setHalfBlock(cell(kUpperHalf, Red, Blue), false, Black) == cell(kUpperHalf, Black, Blue));
    CHECK(setHalfBlock(cell(kUpperHalf, Red, Blue), false, Blue) == cell(U' ', Grey, Blue));
    CHECK(setHalfBlock(Cell{}, true, Yellow) == cell(kLowerHalf, Yellow, Black));
    // a colour already there leaves the cell exactly as written
    const Cell odd = cell(kUpperHalf, Blue, Blue);
    CHECK(setHalfBlock(odd, false, Blue) == odd);
    // text or a shade becomes pixels, the other half its background
    CHECK(setHalfBlock(cell(U'A', White, Green), false, Red) == cell(kUpperHalf, Red, Green));
    CHECK(setHalfBlock(cell(kShadeLight, White, Green), true, Green) == cell(U' ', Grey, Green));
}

TEST_CASE("pickColour picks the half under the pointer, or a cell's colours", "[draw][halfblock]") {
    Canvas c(3, 2);
    c.at(0, 0) = cell(kUpperHalf, Yellow, Blue);
    c.at(1, 0) = cell(U'A', White, Green);
    CHECK(pickColour(c, 0, 0, false) == Color::indexed(Yellow));
    CHECK(pickColour(c, 0, 1, false) == Color::indexed(Blue));
    CHECK(pickColour(c, 0, 1, true) == Color::indexed(Blue));
    CHECK(pickColour(c, 1, 0, false) == Color::indexed(White));
    CHECK(pickColour(c, 1, 1, true) == Color::indexed(Green));
    CHECK_FALSE(pickColour(c, 3, 0, false));
    CHECK_FALSE(pickColour(c, 0, 4, false));
    CHECK_FALSE(pickColour(c, 0, -1, false));
    CHECK_FALSE(pickColour(c, 0, INT_MIN, false));
}

TEST_CASE("plotPixels: two pixels of one cell make one change", "[draw][halfblock]") {
    Canvas c(4, 2);
    const CellEdit e = plotPixels(c, {{1, 2}, {1, 3}, {9, 0}, {0, -1}}, LightRed);
    REQUIRE(e.changes.size() == 1);
    CHECK(e.changes[0].x == 1);
    CHECK(e.changes[0].y == 1);
    CHECK(e.changes[0].after == cell(kFullBlock, LightRed, Black));
    CHECK(e.changes[0].before == Cell{});
}

TEST_CASE("plotPixels respects the selection and the transparent lock", "[draw][halfblock]") {
    Canvas c(4, 2);
    c.at(2, 0).transparent = true;
    StrokeOptions o;
    o.selection = selectionMask(Selection(CellRect{0, 0, 3, 1}), 4, 2);
    o.lockTransparent = true;
    const CellEdit e = plotPixels(c, {{0, 0}, {2, 0}, {3, 0}, {0, 2}}, Red, o);
    REQUIRE(e.changes.size() == 1);
    CHECK(e.changes[0].x == 0);
    CHECK(e.changes[0].y == 0);
}

TEST_CASE("HalfBlockStroke joins points with a 4-connected line, one edit", "[draw][halfblock]") {
    Canvas c(8, 4);
    HalfBlockStroke s(c, White, {});
    s.addPoint(0, 0);
    s.addPoint(3, 3);
    // a fresh segment isn't joined
    s.addPoint(7, 7, false);
    Canvas after = c;
    s.edit().apply(after);
    CHECK(after == s.preview());
    std::set<std::pair<int, int>> lit;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            if (pickColour(after, x, y, false) == Color::indexed(White))
                lit.insert({x, y});
    const auto line = line4(0, 0, 3, 3);
    std::set<std::pair<int, int>> want(line.begin(), line.end());
    want.insert({7, 7});
    CHECK(lit == want);
    // a drag from far off the canvas plots only the part on it
    HalfBlockStroke far(c, White, {});
    far.addPoint(-1000000, 2);
    far.addPoint(INT_MAX, 2);
    CHECK(far.edit().changes.size() == 8);
}

TEST_CASE("fillPixels floods same-coloured pixels, bounded by text and the selection", "[draw][halfblock][fill]") {
    // a 4 × 2 canvas: 8 pixel rows. A red line across pixel row 3, a text
    // cell at (3, 3).
    Canvas c(4, 4);
    for (int x = 0; x < 4; ++x)
        c.at(x, 1) = cell(kLowerHalf, Red, Black);
    c.at(3, 3) = cell(U'A', White, Black);

    // top region: pixel rows 0-2 of every column
    const auto top = floodPixels(c, 0, 0);
    CHECK(top.size() == 12);
    // the red row
    CHECK(floodPixels(c, 2, 3).size() == 4);
    // the bottom: rows 4-7, less the text cell's two pixels
    CHECK(floodPixels(c, 0, 7).size() == 14);
    // text isn't pixels: no fill
    CHECK(floodPixels(c, 3, 6).empty());
    CHECK(floodPixels(c, -1, 0).empty());

    const CellEdit e = fillPixels(c, 0, 0, Blue);
    Canvas after = c;
    e.apply(after);
    CHECK(after.at(0, 0) == cell(U' ', Grey, Blue));
    CHECK(after.at(2, 1) == cell(kUpperHalf, Blue, Red));
    CHECK(after.at(0, 2) == Cell{});
    // filling with the same colour changes nothing
    CHECK(fillPixels(c, 0, 0, Black).empty());

    // the selection bounds the flood
    StrokeOptions o;
    o.selection = selectionMask(Selection(CellRect{0, 0, 2, 1}), 4, 4);
    CHECK(floodPixels(c, 0, 0, o).size() == 4);
    CHECK(floodPixels(c, 3, 0, o).empty());
}

TEST_CASE("lineShape: the same pixels from either end, mirror-symmetric across", "[draw][shapes]") {
    const int w = 23, h = 17;
    for (int x0 = 0; x0 < w; x0 += 3)
        for (int y0 = 0; y0 < h; y0 += 4)
            for (int x1 = 0; x1 < w; x1 += 5)
                for (int y1 = 0; y1 < h; y1 += 3) {
                    const auto a = lineShape(x0, y0, x1, y1, w, h);
                    CHECK(asSet(a) == asSet(lineShape(x1, y1, x0, y0, w, h)));
                    CHECK(asSet(mirrored(a, w, h, true)) ==
                          asSet(lineShape(w - 1 - x0, y0, w - 1 - x1, y1, w, h)));
                    CHECK(std::find(a.begin(), a.end(), std::pair{x0, y0}) != a.end());
                    CHECK(std::find(a.begin(), a.end(), std::pair{x1, y1}) != a.end());
                }
    // clipped to the grid
    const auto clipped = lineShape(-5, 2, 30, 2, 10, 4);
    CHECK(clipped.size() == 10);
    CHECK(lineShape(-5, -5, -1, -1, 10, 4).empty());
}

TEST_CASE("rectShape: border or filled, clipped", "[draw][shapes]") {
    CHECK(picture(rectShape(3, 2, 0, 0, false, 5, 4), 5, 4) ==
          std::vector<std::string>{"####.", "#..#.", "####.", "....."});
    CHECK(picture(rectShape(0, 0, 2, 1, true, 4, 3), 4, 3) == std::vector<std::string>{"###.", "###.", "...."});
    CHECK(rectShape(1, 1, 1, 1, false, 3, 3) == Pixels{{1, 1}});
    // half off the grid: only the border pixels on it
    CHECK(picture(rectShape(-2, 1, 2, 9, false, 4, 4), 4, 4) ==
          std::vector<std::string>{"....", "###.", "..#.", "..#."});
    CHECK(rectShape(-5, -5, -1, -1, true, 4, 4).empty());
    // a huge box costs no more than the grid
    CHECK(rectShape(INT_MIN, INT_MIN, INT_MAX, INT_MAX, true, 4, 4).size() == 16);
}

TEST_CASE("ellipseShape: tuned small circles", "[draw][shapes]") {
    CHECK(picture(ellipseShape(0, 0, 0, 0, false, 1, 1), 1, 1) == std::vector<std::string>{"#"});
    CHECK(picture(ellipseShape(0, 0, 2, 2, true, 3, 3), 3, 3) == std::vector<std::string>{"###", "###", "###"});
    CHECK(picture(ellipseShape(0, 0, 4, 4, true, 5, 5), 5, 5) ==
          std::vector<std::string>{".###.", "#####", "#####", "#####", ".###."});
    CHECK(picture(ellipseShape(0, 0, 4, 4, false, 5, 5), 5, 5) ==
          std::vector<std::string>{".###.", "##.##", "#...#", "##.##", ".###."});
    CHECK(picture(ellipseShape(0, 0, 6, 6, true, 7, 7), 7, 7) ==
          std::vector<std::string>{"..###..", ".#####.", "#######", "#######", "#######", ".#####.", "..###.."});
    // one pixel wide: a line
    CHECK(picture(ellipseShape(1, 0, 1, 3, false, 3, 4), 3, 4) == std::vector<std::string>{".#.", ".#.", ".#.", ".#."});
}

TEST_CASE("ellipseShape and rectShape are symmetric both ways", "[draw][shapes]") {
    const int w = 30, h = 24;
    for (int x0 = 0; x0 < w; x0 += 4)
        for (int y0 = 0; y0 < h; y0 += 5)
            for (int x1 = 1; x1 < w; x1 += 6)
                for (int y1 = 2; y1 < h; y1 += 4)
                    for (bool filled : {false, true}) {
                        const auto e = ellipseShape(x0, y0, x1, y1, filled, w, h);
                        CHECK(asSet(e) == asSet(ellipseShape(x1, y1, x0, y0, filled, w, h)));
                        CHECK(asSet(mirrored(e, w, h, true)) ==
                              asSet(ellipseShape(w - 1 - x0, y0, w - 1 - x1, y1, filled, w, h)));
                        CHECK(asSet(mirrored(e, w, h, false)) ==
                              asSet(ellipseShape(x0, h - 1 - y0, x1, h - 1 - y1, filled, w, h)));
                        const auto r = rectShape(x0, y0, x1, y1, filled, w, h);
                        CHECK(asSet(mirrored(r, w, h, false)) ==
                              asSet(rectShape(x0, h - 1 - y0, x1, h - 1 - y1, filled, w, h)));
                    }
}

TEST_CASE("ellipseShape outline is the filled shape's edge, 4-connected", "[draw][shapes]") {
    const int w = 40, h = 30;
    for (const auto& [x1, y1] : {std::pair{39, 29}, {20, 9}, {5, 25}, {12, 12}}) {
        const auto filled = asSet(ellipseShape(0, 0, x1, y1, true, w, h));
        const auto outline = asSet(ellipseShape(0, 0, x1, y1, false, w, h));
        for (const auto& p : outline)
            CHECK(filled.count(p));
        // every outline pixel has a 4-neighbour in the outline
        for (const auto& [x, y] : outline) {
            const bool joined = outline.count({x - 1, y}) || outline.count({x + 1, y}) || outline.count({x, y - 1}) ||
                                outline.count({x, y + 1});
            CHECK(joined);
        }
        // the inside is the filled shape less the outline: nothing inside
        // touches the outside
        for (const auto& [x, y] : filled)
            if (!outline.count({x, y}))
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        CHECK(filled.count({x + dx, y + dy}));
    }
}

TEST_CASE("ShadeBrush builds up and reduces along space ░ ▒ ▓ █", "[draw][brush]") {
    CHECK(shadeLevel(U' ') == 0);
    CHECK(shadeLevel(kShadeLight) == 1);
    CHECK(shadeLevel(kFullBlock) == 4);
    CHECK_FALSE(shadeLevel(U'A'));

    Canvas c(5, 1);
    c.at(1, 0) = cell(kShadeLight, Red, Black);
    c.at(2, 0) = cell(kFullBlock, Red, Black);
    c.at(3, 0) = cell(U'A', White, Black);
    const Pen pen{Color::indexed(Yellow), Color::indexed(Blue)};
    const ShadeBrush up(pen, false), down(pen, true);
    const std::vector<Dab> across{{0, 0}, {4, 0}};

    Canvas built = c;
    paintStroke(c, up, {}, across).apply(built);
    CHECK(built.at(0, 0) == cell(kShadeLight, Yellow, Blue));
    CHECK(built.at(1, 0) == cell(kShadeMedium, Yellow, Blue));
    CHECK(built.at(2, 0) == cell(kFullBlock, Yellow, Blue));
    // text isn't a step: left alone (N4)
    CHECK(built.at(3, 0) == c.at(3, 0));

    Canvas reduced = c;
    paintStroke(c, down, {}, across).apply(reduced);
    CHECK(reduced.at(0, 0) == Cell{});
    CHECK(reduced.at(1, 0) == cell(U' ', Yellow, Blue));
    CHECK(reduced.at(2, 0) == cell(kShadeDark, Yellow, Blue));
    CHECK(reduced.at(3, 0) == c.at(3, 0));

    // one step per stroke, however often it passes a cell
    Canvas twice = c;
    paintStroke(c, up, {}, {{0, 0}, {4, 0}, {0, 0}}).apply(twice);
    CHECK(twice.at(0, 0).ch == kShadeLight);
}

TEST_CASE("GlyphBrush writes its cell; ColorizeBrush keeps glyphs", "[draw][brush]") {
    Canvas c(3, 1);
    c.at(0, 0) = cell(U'A', White, Black);
    c.at(1, 0) = cell(kShadeMedium, Red, Blue);

    const Cell star = cell(U'*', LightGreen, Black);
    Canvas g = c;
    paintStroke(c, GlyphBrush(star), {}, {{0, 0}, {2, 0}}).apply(g);
    for (int x = 0; x < 3; ++x)
        CHECK(g.at(x, 0) == star);

    Canvas k = c;
    paintStroke(c, ColorizeBrush(Color::indexed(Yellow), std::nullopt), {}, {{0, 0}, {2, 0}}).apply(k);
    CHECK(k.at(0, 0) == cell(U'A', Yellow, Black));
    CHECK(k.at(1, 0) == cell(kShadeMedium, Yellow, Blue));
    CHECK(k.at(2, 0) == cell(U' ', Yellow, Black));
    Canvas b = c;
    paintStroke(c, ColorizeBrush(std::nullopt, Color::indexed(Green)), {}, {{1, 0}}).apply(b);
    CHECK(b.at(1, 0) == cell(kShadeMedium, Red, Green));
    CHECK(b.at(0, 0) == c.at(0, 0));
}

TEST_CASE("ShadeBrush leaves half-block pixels alone in both directions", "[draw][brush]") {
    // N4: a shade pass over ▀ ▄ would lose one of the two colours
    Canvas c(3, 1);
    c.at(0, 0) = cell(kUpperHalf, Yellow, Blue);
    c.at(1, 0) = cell(kLowerHalf, Red, Green);
    c.at(2, 0) = cell(U'A', White, Black);
    const Pen pen{Color::indexed(White), Color::indexed(Black)};
    for (const bool reduce : {false, true}) {
        INFO("reduce " << reduce);
        CHECK(paintStroke(c, ShadeBrush(pen, reduce), {}, {{0, 0}, {2, 0}}).changes.empty());
    }
}

namespace {

Cell blinking(Cell c) {
    c.attrs |= attr::Blink;
    return c;
}

} // namespace

TEST_CASE("half-block pixels read the colours a cell shows under the iCE setting", "[draw][halfblock][ice]") {
    // a bright background stored with iCE off shows as its dark colour
    const Cell brightBg = cell(U' ', Grey, Yellow);
    CHECK(halfBlocks(brightBg, true) == HalfPair{Yellow, Yellow});
    CHECK(halfBlocks(brightBg, false) == HalfPair{Brown, Brown});
    CHECK(halfBlocks(cell(kUpperHalf, White, LightBlue), false) == HalfPair{White, Blue});
    // and a blinking dark background shows bright with iCE on
    CHECK(halfBlocks(blinking(cell(kLowerHalf, Yellow, Red)), true) == HalfPair{LightRed, Yellow});
    CHECK(halfBlocks(blinking(cell(kLowerHalf, Yellow, Red)), false) == HalfPair{Red, Yellow});

    Canvas c(2, 1);
    c.at(0, 0) = brightBg;
    c.at(1, 0) = cell(U' ', Grey, Brown);
    CHECK(pickColour(c, 0, 0, false, false) == Color::indexed(Brown));
    CHECK(pickColour(c, 0, 0, false, true) == Color::indexed(Yellow));
    CHECK(pickColour(c, 0, 1, true, false) == Color::indexed(Brown));
    // fill compares shown colours: both cells show brown with iCE off
    StrokeOptions off;
    off.ice = false;
    CHECK(floodPixels(c, 0, 0, off).size() == 4);
    CHECK(floodPixels(c, 0, 0).size() == 2);
    // setHalfBlock: the other half keeps the colour it showed
    CHECK(halfBlocks(setHalfBlock(brightBg, false, Red, false), false) == HalfPair{Red, Brown});
}

TEST_CASE("iCE off: the bright colour goes in the foreground, so the untouched half keeps its colour",
          "[draw][halfblock][ice]") {
    // R4's case: yellow on the top of a solid white cell
    const Cell white = cell(kFullBlock, White, Black);
    const Cell on = setHalfBlock(white, false, Yellow, true);
    CHECK(halfBlocks(on, true) == HalfPair{Yellow, White});
    const Cell off = setHalfBlock(white, false, Yellow, false);
    // white stays white below; yellow can only show as brown beside it
    CHECK(halfBlocks(off, false) == HalfPair{Brown, White});
    CHECK(off.bg.paletteIndex() < 8);
    CHECK(off.attrs == 0);
    // one bright colour and a dark one: the bright one is the foreground
    for (const bool bottom : {false, true}) {
        for (const std::uint8_t dark : {Black, Red, Blue}) {
            const Cell c = setHalfBlock(cell(U' ', Grey, dark), bottom, LightCyan, false);
            INFO("bottom " << bottom << " dark " << int(dark));
            CHECK(c.fg == Color::indexed(LightCyan));
            CHECK(c.bg == Color::indexed(dark));
            CHECK(halfBlocks(c, false) == (bottom ? HalfPair{dark, LightCyan} : HalfPair{LightCyan, dark}));
        }
    }
    // a dark colour on a bright one keeps the bright one too
    CHECK(halfBlocks(setHalfBlock(white, true, Blue, false), false) == HalfPair{White, Blue});
    // two different brights can't show: halfBlockCell keeps the top
    CHECK(halfBlocks(halfBlockCell(HalfPair{Yellow, White}, false), false) == HalfPair{Yellow, Grey});
    // both halves painted come out in the colour whatever was there
    CHECK(setHalfBlocks(white, Yellow, false) == cell(kFullBlock, Yellow, Black));
    Canvas k(1, 1);
    k.at(0, 0) = white;
    StrokeOptions iceOff;
    iceOff.ice = false;
    const CellEdit both = plotPixels(k, {{0, 0}, {0, 1}}, Yellow, iceOff);
    REQUIRE(both.changes.size() == 1);
    CHECK(both.changes[0].after == cell(kFullBlock, Yellow, Black));
}

TEST_CASE("half-block writes keep the attributes the colours don't use", "[draw][halfblock][ice]") {
    // iCE off: blink is the cell's own and stays
    const Cell blink = blinking(cell(kUpperHalf, Red, Blue));
    const Cell off = setHalfBlock(blink, true, Green, false);
    CHECK(off.attrs == attr::Blink);
    CHECK(halfBlocks(off, false) == HalfPair{Red, Green});
    CHECK(setHalfBlocks(blink, Green, false).attrs == attr::Blink);
    // iCE on: blink is the bright background, which the write replaces
    const Cell on = setHalfBlock(blink, true, Green, true);
    CHECK(halfBlocks(blink, true) == HalfPair{Red, LightBlue});
    CHECK(halfBlocks(on, true) == HalfPair{Red, Green});
    CHECK(on.attrs == 0);
    // a bright background stored with iCE off shows as blink: it keeps it
    CHECK(setHalfBlock(cell(U' ', Grey, LightRed), false, Yellow, false).attrs == attr::Blink);
}

TEST_CASE("painting a transparent cell makes it opaque", "[draw][halfblock]") {
    Cell clear;
    clear.transparent = true;
    // even when the colour painted is the one it shows already
    for (const bool ice : {true, false}) {
        CHECK_FALSE(setHalfBlock(clear, false, Black, ice).transparent);
        CHECK_FALSE(setHalfBlock(clear, true, Red, ice).transparent);
        CHECK_FALSE(setHalfBlocks(clear, Black, ice).transparent);
    }
    Canvas c(1, 1);
    c.at(0, 0) = clear;
    const CellEdit e = plotPixels(c, {{0, 0}}, Black);
    REQUIRE(e.changes.size() == 1);
    CHECK(e.changes[0].after == Cell{});
    // the transparent lock still leaves it alone
    StrokeOptions lock;
    lock.lockTransparent = true;
    CHECK(plotPixels(c, {{0, 0}}, Black, lock).changes.empty());
}

TEST_CASE("mirrored pixels land in the mirror cell's own half", "[draw][halfblock][mirror]") {
    // R1: the mirror cell keeps its other half
    Canvas c(10, 2);
    c.at(8, 0) = cell(kLowerHalf, Red, Black);  // bottom red, top black
    const CellEdit e = plotPixels(c, {{1, 0}}, Yellow, {}, true);
    Canvas after = c;
    e.apply(after);
    CHECK(halfBlocks(after.at(1, 0)) == HalfPair{Yellow, Black});
    CHECK(halfBlocks(after.at(8, 0)) == HalfPair{Yellow, Red});
    // the middle of an odd width is its own mirror
    Canvas odd(5, 1);
    const CellEdit mid = plotPixels(odd, {{2, 1}}, Green, {}, true);
    REQUIRE(mid.changes.size() == 1);
    CHECK(halfBlocks(mid.changes[0].after) == HalfPair{Black, Green});
    // a fill's mirror is the region's mirror image, not a second flood
    Canvas f(6, 1);
    f.at(0, 0) = cell(kUpperHalf, White, Black);
    f.at(1, 0) = cell(kUpperHalf, White, Black);
    const CellEdit fill = fillPixels(f, 0, 0, Blue, {}, true);
    Canvas filled = f;
    fill.apply(filled);
    for (const int x : {0, 1, 4, 5})
        CHECK(halfBlocks(filled.at(x, 0)) == HalfPair{Blue, Black});
    CHECK(filled.at(2, 0) == Cell{});
    CHECK(filled.at(3, 0) == Cell{});
}

TEST_CASE("mirrored pixels stay inside the selection", "[draw][halfblock][mirror]") {
    // C3: the mask is checked for the mirrored pixel too
    Canvas c(10, 1);
    StrokeOptions o;
    o.selection = selectionMask(Selection(CellRect{0, 0, 5, 1}), 10, 1);
    const CellEdit e = plotPixels(c, {{1, 0}, {2, 1}}, Yellow, o, true);
    for (const CellChange& ch : e.changes)
        CHECK(ch.x < 5);
    CHECK(e.changes.size() == 2);
}

TEST_CASE("PixelPainter keeps only a window around the cells it plots", "[draw][halfblock]") {
    // Codex P2 (R3): a shape redrawn on every move must not touch the whole
    // canvas. A small filled rectangle on 400x2000, with mirror, the
    // selection and the transparent lock, stores at most twice the span of
    // the cells it plots each way, not the canvas.
    const Canvas c(400, 2000);
    StrokeOptions o;
    o.selection = selectionMask(Selection(CellRect{0, 0, 400, 2000}), 400, 2000);
    o.lockTransparent = true;
    for (const bool mirror : {false, true}) {
        INFO(mirror);
        for (const auto& to : {std::pair{3, 3}, std::pair{4, 3}, std::pair{3, 3}, std::pair{6, 7}}) {
            PixelPainter p(c, Yellow, o, mirror);
            for (const auto& [x, py] : rectShape(2, 2, to.first, to.second, true, 400, 4000))
                p.plot(x, py);
            const CellEdit e = p.edit();
            const int rows = to.second / 2;
            const int cols = to.first - 1;
            CHECK(e.changes.size() == static_cast<size_t>((mirror ? 2 : 1) * cols * rows));
            // the cells' bounding box, the mirror included
            const int spanX = mirror ? 400 - 2 * 2 : cols;  // columns 2 to 397
            CHECK(p.storedWidth() <= 2 * spanX);
            CHECK(p.storedHeight() <= 2 * rows);
        }
    }
    // nothing plotted, nothing kept; the whole canvas, the canvas exactly
    const PixelPainter none(c, Yellow, o);
    CHECK(none.storedCells() == 0);
    PixelPainter all(c, Yellow, o);
    for (const auto& [x, py] : rectShape(0, 0, 399, 3999, true, 400, 4000))
        all.plot(x, py);
    CHECK(all.storedCells() == 400u * 2000u);
    CHECK(all.edit().changes.size() == 400u * 2000u);
}

TEST_CASE("PixelPainter's window stays within twice the plotted span in any order", "[draw][halfblock]") {
    // Codex P3 (R3): growing by the window's own size let a window grow
    // past twice the span when cells land on alternate sides.
    const Canvas c(400, 2000);
    SECTION("a diagonal plotted out of order: 6x6 cells") {
        PixelPainter p(c, Yellow);
        for (const auto& [x, py] : {std::pair{100, 200}, {99, 198}, {98, 196}, {101, 202}, {96, 192}})
            p.plot(x, py);
        CHECK(p.storedWidth() <= 2 * 6);
        CHECK(p.storedHeight() <= 2 * 6);
        CHECK(p.edit().changes.size() == 5);
    }
    SECTION("one row, alternating sides: 34 cells") {
        PixelPainter p(c, Yellow);
        for (const int x : {100, 101, 99, 102, 97, 106, 89, 122})
            p.plot(x, 0);
        CHECK(p.storedWidth() <= 2 * 34);
        CHECK(p.storedHeight() == 1);
        CHECK(p.edit().changes.size() == 8);
    }
}

TEST_CASE("Painters share the selection mask rather than copy it", "[draw][halfblock]") {
    // Codex P2 (R3): a shape tool builds a PixelPainter from its options on
    // every move, and a copied mask is canvas-sized (800,000 cells here).
    const Canvas c(400, 2000);
    StrokeOptions o;
    o.selection = selectionMask(Selection(CellRect{10, 10, 20, 20}), 400, 2000);
    REQUIRE(o.selection);
    const StrokeOptions copy = o;
    CHECK(copy.selection.get() == o.selection.get());
    const PixelPainter p(c, Yellow, o);
    CHECK(p.selection() == o.selection.get());
    const PixelPainter moved(c, Yellow, StrokeOptions(o));
    CHECK(moved.selection() == o.selection.get());
    // and the shared mask still clips
    CHECK(plotPixels(c, {{5, 5}, {15, 30}}, Yellow, o).changes.size() == 1);
}

TEST_CASE("PixelPainter gives the same edit whatever order it plots in", "[draw][halfblock]") {
    // the window grows left and up as well as right and down
    Canvas c(30, 12);
    c.at(4, 3) = cell(kLowerHalf, Red, Black);
    c.at(25, 3) = cell(kUpperHalf, Green, Blue);
    for (const bool mirror : {false, true}) {
        INFO(mirror);
        Pixels px = ellipseShape(3, 2, 17, 21, false, 30, 24);
        const Pixels line = lineShape(0, 23, 13, 0, 30, 24);
        px.insert(px.end(), line.begin(), line.end());
        const CellEdit forward = plotPixels(c, px, Yellow, {}, mirror);
        std::reverse(px.begin(), px.end());
        const CellEdit backward = plotPixels(c, px, Yellow, {}, mirror);
        REQUIRE(forward.changes.size() == backward.changes.size());
        for (size_t i = 0; i < forward.changes.size(); ++i) {
            CHECK(forward.changes[i].x == backward.changes[i].x);
            CHECK(forward.changes[i].y == backward.changes[i].y);
            CHECK(forward.changes[i].after == backward.changes[i].after);
        }
        // and cellAt agrees with the edit
        PixelPainter p(c, Yellow, {}, mirror);
        for (const auto& [x, py] : px)
            p.plot(x, py);
        for (const CellChange& ch : forward.changes)
            CHECK(p.cellAt(ch.x, ch.y) == ch.after);
    }
}

TEST_CASE("the selection clips a pixel edit and its mirror each on its own", "[draw][halfblock][mirror]") {
    // R6: a pixel outside the selection still paints its mirror inside it,
    // and a pixel inside paints itself but not a mirror that falls outside
    const Canvas c(10, 1);
    StrokeOptions o;
    o.selection = selectionMask(Selection(CellRect{7, 0, 3, 1}), 10, 1);
    auto cells = [](const CellEdit& e) {
        std::set<int> xs;
        for (const CellChange& ch : e.changes)
            xs.insert(ch.x);
        return xs;
    };
    // plotPixels, as the half-block brush, line, rectangle and ellipse plot
    CHECK(cells(plotPixels(c, {{1, 0}}, Yellow, o, true)) == std::set<int>{8});
    CHECK(cells(plotPixels(c, {{8, 0}}, Yellow, o, true)) == std::set<int>{8});
    CHECK(plotPixels(c, {{1, 0}}, Yellow, o, false).changes.empty());
    // the pencil
    HalfBlockStroke s(c, Yellow, o, true);
    s.addPoint(0, 0);
    s.addPoint(2, 1);
    CHECK(cells(s.edit()) == std::set<int>{7, 8, 9});
    // fill floods inside the selection only, so its source is always
    // inside; its mirror image is painted where it falls inside too, over
    // a cell the flood didn't reach (the text at 5 bounds the flood and is
    // replaced by the mirror's pixels), and dropped where it falls outside
    Canvas t(10, 1);
    t.at(5, 0) = cell(U'x', White, Black);
    StrokeOptions wide;
    wide.selection = selectionMask(Selection(CellRect{2, 0, 8, 1}), 10, 1);
    CHECK(cells(fillPixels(t, 3, 0, Blue, wide, true)) == std::set<int>{2, 3, 4, 5, 6, 7});
    CHECK(cells(fillPixels(c, 8, 0, Blue, o, true)) == std::set<int>{7, 8, 9});
}
