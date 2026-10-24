// The compositor (PLAN §5): the compositing rules, their properties, the
// dirty-rect recomposite, Phase 3's done-when (shade under a pixel layer
// composites with no specks) and timings.

#include "blockwright/composite.h"
#include "blockwright/ansi.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"
#include "blockwright/shapes.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <climits>
#include <cstdio>
#include <random>

using namespace bw;

namespace {

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg, std::uint8_t attrs = 0) {
    return Cell{ch, Color::indexed(fg), Color::indexed(bg), attrs, false};
}

Layer filledLayer(LayerKind kind, int w, int h, const Cell& c, int x = 0, int y = 0) {
    Layer l = makeLayer(kind, w, h, {}, x, y);
    for (int cy = 0; cy < h; ++cy)
        for (int cx = 0; cx < w; ++cx)
            l.cells.at(cx, cy) = c;
    return l;
}

Layer pixelLayer(int w, int h, int x = 0, int y = 0) { return makeLayer(LayerKind::Pixels, w, h, {}, x, y); }

void setPixel(Layer& l, int x, int py, std::optional<std::uint8_t> colour) {
    Cell& c = l.cells.at(x, py >> 1);
    c = setLayerPixel(c, (py & 1) != 0, colour);
}

LayerStack stackOf(int w, int h, std::vector<Layer> layers) {
    LayerStack s(w, h);
    for (Layer& l : layers)
        s.add(std::move(l));
    return s;
}

// ---- random stacks, for the properties ----

struct Rng {
    std::mt19937 g;
    explicit Rng(unsigned seed) : g(seed) {}
    int range(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(g); }
    bool chance(int percent) { return range(0, 99) < percent; }
};

Color randomColour(Rng& r) {
    if (r.chance(10))
        return Color::fromRgb(Rgb{std::uint8_t(r.range(0, 255)), std::uint8_t(r.range(0, 255)),
                                  std::uint8_t(r.range(0, 255))});
    return Color::indexed(std::uint8_t(r.range(0, 15)));
}

Cell randomOpaqueCell(Rng& r) {
    static const char32_t glyphs[] = {U' ', U'█', U'▀', U'▄', U'░', U'▒', U'▓', U'A', U'z', U'▌', U'·', U'\0'};
    Cell c;
    c.ch = glyphs[r.range(0, int(std::size(glyphs)) - 1)];
    c.fg = randomColour(r);
    c.bg = randomColour(r);
    c.attrs = r.chance(10) ? attr::Blink : 0;
    return c;
}

Cell randomCell(Rng& r, LayerKind kind) {
    if (kind == LayerKind::Pixels) {
        LayerPixels p;
        if (r.chance(60))
            p.top = std::uint8_t(r.range(0, 15));
        if (r.chance(60))
            p.bottom = std::uint8_t(r.range(0, 15));
        return pixelLayerCell(p);
    }
    if (r.chance(40))
        return transparentCell();
    return randomOpaqueCell(r);
}

Layer randomLayer(Rng& r, int cw, int ch) {
    static const LayerKind kinds[] = {LayerKind::Cells, LayerKind::Pixels, LayerKind::Text, LayerKind::Cells,
                                      LayerKind::Pixels, LayerKind::Text, LayerKind::Generator,
                                      LayerKind::Reference};
    const LayerKind kind = kinds[r.range(0, int(std::size(kinds)) - 1)];
    const int w = r.range(1, cw + 6), h = r.range(1, ch + 4);
    Layer l = makeLayer(kind, w, h, {}, r.range(-w, cw), r.range(-h, ch));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            l.cells.at(x, y) = randomCell(r, kind);
    l.props.visible = r.chance(85);
    l.props.locked = r.chance(20);
    l.props.alphaLocked = r.chance(20);
    l.props.mode = static_cast<LayerMode>(r.range(0, 2));
    return l;
}

LayerStack randomStack(Rng& r, int w, int h, int maxLayers = 6) {
    LayerStack s(w, h);
    const int n = r.range(1, maxLayers);
    for (int i = 0; i < n; ++i)
        s.add(randomLayer(r, w, h));
    return s;
}

bool sameOn(const Canvas& a, const Canvas& b, CellRect rect) {
    for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x)
            if (!(a.at(x, y) == b.at(x, y)))
                return false;
    return true;
}

} // namespace

TEST_CASE("an empty stack composites to the ground", "[layers][composite]") {
    CHECK(composite(LayerStack(3, 2)) == Canvas(3, 2));
    CompositeOptions o;
    o.ground = cell(U' ', Grey, Blue);
    const Canvas c = composite(LayerStack(3, 2), o);
    CHECK(c.at(2, 1) == o.ground);
}

TEST_CASE("transparent cells show what's below", "[layers][composite]") {
    const Cell a = cell(U'a', Red, Blue);
    Layer top = makeLayer(LayerKind::Cells, 4, 3);
    top.cells.at(2, 1) = cell(U'b', White, Black);
    const Canvas c = composite(stackOf(4, 3, {filledLayer(LayerKind::Cells, 4, 3, a), top}));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 4; ++x)
            CHECK(c.at(x, y) == (x == 2 && y == 1 ? cell(U'b', White, Black) : a));
    // a transparent pixel half shows the half below
    Layer px = pixelLayer(4, 3);
    setPixel(px, 0, 0, Yellow);
    const Canvas p = composite(stackOf(4, 3, {filledLayer(LayerKind::Cells, 4, 3, cell(U'▀', Green, Blue)), px}));
    CHECK(halfBlocks(p.at(0, 0)) == HalfPair{Yellow, Blue});
    CHECK(p.at(1, 0) == cell(U'▀', Green, Blue));
}

TEST_CASE("a pixel layer over a shade replaces only the half it covers, with no fallback speck",
          "[layers][composite][pixels]") {
    // B░A shows mostly A, B▓A mostly B, ▒ half each
    const auto over = [](const Cell& below, bool bottom, std::uint8_t colour) {
        Layer px = pixelLayer(1, 1);
        setPixel(px, 0, bottom ? 1 : 0, colour);
        return composite(stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, below), px})).at(0, 0);
    };
    const Cell light = cell(kShadeLight, White, Blue), medium = cell(kShadeMedium, White, Blue),
               dark = cell(kShadeDark, White, Blue);
    CHECK(halfBlocks(over(light, false, Red)) == HalfPair{Red, Blue});
    CHECK(halfBlocks(over(light, true, Red)) == HalfPair{Blue, Red});
    CHECK(halfBlocks(over(dark, false, Red)) == HalfPair{Red, White});
    CHECK(halfBlocks(over(dark, true, Red)) == HalfPair{White, Red});
    // ▒: the darker colour on top, the lighter below, whichever is the foreground
    CHECK(halfBlocks(over(medium, true, Red)) == HalfPair{Blue, Red});
    CHECK(halfBlocks(over(medium, false, Red)) == HalfPair{Red, White});
    CHECK(halfBlocks(over(cell(kShadeMedium, Blue, White), true, Red)) == HalfPair{Blue, Red});
    // re-picked to keep the background dark: Red over White is ▄, White on top
    const Cell d = over(dark, false, Red);
    CHECK(d.ch == kLowerHalf);
    CHECK(d.fg == Color::indexed(White));
    CHECK(d.bg == Color::indexed(Red));
    // the old way (plotting into the shade) leaves the background, a speck in a ▓ band
    CHECK(halfBlocks(setHalfBlock(dark, false, Red)) == HalfPair{Red, Blue});
    // both halves: the shade is gone
    Layer both = pixelLayer(1, 1);
    setPixel(both, 0, 0, Red);
    setPixel(both, 0, 1, Red);
    CHECK(composite(stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, dark), both})).at(0, 0) ==
          halfBlockCell(HalfPair{Red, Red}));
    CHECK(halvesUnder(light) == HalfPair{Blue, Blue});
    CHECK(halvesUnder(dark) == HalfPair{White, White});
}

TEST_CASE("a pixel layer over a text cell of a cells layer replaces the half it covers", "[layers][composite]") {
    Layer px = pixelLayer(1, 1);
    setPixel(px, 0, 0, Yellow);
    const Cell c = composite(stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, cell(U'A', White, Blue)), px})).at(0, 0);
    CHECK(c == cell(kUpperHalf, Yellow, Blue));
}

TEST_CASE("plot does not erase text", "[layers][composite][text]") {
    const Cell h = cell(U'H', White, Blue), i = cell(U'I', White, Blue);
    Layer text = makeLayer(LayerKind::Text, 2, 1, "title", 1, 0);
    text.cells.at(0, 0) = h;
    text.cells.at(1, 0) = i;
    Layer px = pixelLayer(4, 1);
    for (int x = 0; x < 4; ++x)
        for (int py : {0, 1})
            setPixel(px, x, py, LightRed);
    Layer top = px;  // a pixel covering only the top half
    for (int x = 0; x < 4; ++x)
        setPixel(top, x, 1, std::nullopt);

    for (const Layer& pixels : {px, top}) {
        // pixels above the text
        Canvas c = composite(stackOf(4, 1, {text, pixels}));
        CHECK(c.at(1, 0) == h);
        CHECK(c.at(2, 0) == i);
        CHECK(halfBlocks(c.at(0, 0)).value().top == LightRed);
        CHECK(halfBlocks(c.at(3, 0)).value().top == LightRed);
        // and below it
        c = composite(stackOf(4, 1, {pixels, text}));
        CHECK(c.at(1, 0) == h);
        CHECK(c.at(2, 0) == i);
    }
    // a cells layer above the text still covers it, and then it is cells
    const Cell x = cell(U'x', Green, Black);
    Layer cover = makeLayer(LayerKind::Cells, 1, 1, {}, 1, 0);
    cover.cells.at(0, 0) = x;
    Canvas c = composite(stackOf(4, 1, {text, cover}));
    CHECK(c.at(1, 0) == x);
    c = composite(stackOf(4, 1, {text, cover, px}));
    CHECK(halfBlocks(c.at(1, 0)) == HalfPair{LightRed, LightRed});
    CHECK(c.at(2, 0) == i);
    // chars-only text keeps the colours under it and is still text
    Layer chars = text;
    chars.props.mode = LayerMode::CharsOnly;
    c = composite(stackOf(4, 1, {filledLayer(LayerKind::Cells, 4, 1, cell(U'.', Red, Green)), chars, px}));
    CHECK(c.at(1, 0) == cell(U'H', Red, Green));
    // a chars-only cells layer over text replaces its glyph, so the cell is
    // no longer text and the pixels cover it
    {
        Layer t = makeLayer(LayerKind::Text, 1, 1);
        t.cells.at(0, 0) = cell(U'H', White, Blue);
        Layer shadeChars = filledLayer(LayerKind::Cells, 1, 1, cell(kShadeDark, Grey, Black));
        shadeChars.props.mode = LayerMode::CharsOnly;
        Layer red = pixelLayer(1, 1);
        setPixel(red, 0, 0, Red);
        CHECK(composite(stackOf(1, 1, {t, shadeChars, red})).at(0, 0) == cell(kLowerHalf, White, Red));
        Layer t2 = makeLayer(LayerKind::Text, 1, 1);
        t2.cells.at(0, 0) = cell(U'A', White, Blue);
        Layer medium = filledLayer(LayerKind::Cells, 1, 1, cell(kShadeMedium, Grey, Black));
        medium.props.mode = LayerMode::CharsOnly;
        Layer both = pixelLayer(1, 1);
        setPixel(both, 0, 0, LightRed);
        setPixel(both, 0, 1, LightRed);
        CHECK(halfBlocks(composite(stackOf(1, 1, {t2, medium, both})).at(0, 0)) == HalfPair{LightRed, LightRed});
        // and a chars-only text layer over a cells layer's glyph makes it text
        Layer textChars = t;
        textChars.props.mode = LayerMode::CharsOnly;
        CHECK(composite(stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, cell(U'.', Red, Green)), textChars, both}))
                  .at(0, 0) == cell(U'H', Red, Green));
    }
    // colours-only text writes no text, so the pixels cover it
    Layer colours = text;
    colours.props.mode = LayerMode::ColoursOnly;
    c = composite(stackOf(4, 1, {colours, px}));
    CHECK(halfBlocks(c.at(1, 0)) == HalfPair{LightRed, LightRed});
    // the text layer's transparent cells let the pixels through
    Layer holey = makeLayer(LayerKind::Text, 4, 1);
    holey.cells.at(2, 0) = i;
    c = composite(stackOf(4, 1, {holey, px}));
    CHECK(c.at(2, 0) == i);
    CHECK(halfBlocks(c.at(1, 0)) == HalfPair{LightRed, LightRed});
}

TEST_CASE("chars-only and colours-only modes", "[layers][composite]") {
    const Cell under = cell(U'a', Red, Blue, attr::Blink);
    Layer l = makeLayer(LayerKind::Cells, 2, 1);
    l.cells.at(0, 0) = cell(U'Z', White, Green);
    l.props.mode = LayerMode::CharsOnly;
    const Layer base = filledLayer(LayerKind::Cells, 2, 1, under);
    Canvas c = composite(stackOf(2, 1, {base, l}));
    CHECK(c.at(0, 0) == cell(U'Z', Red, Blue, attr::Blink));
    CHECK(c.at(1, 0) == under);
    l.props.mode = LayerMode::ColoursOnly;
    c = composite(stackOf(2, 1, {base, l}));
    CHECK(c.at(0, 0) == cell(U'a', White, Green));
    CHECK(c.at(1, 0) == under);
    // RGB colours pass through a colours-only layer as they are
    l.cells.at(0, 0).fg = Color::fromRgb({1, 2, 3});
    c = composite(stackOf(2, 1, {base, l}));
    CHECK(c.at(0, 0).fg == Color::fromRgb({1, 2, 3}));
    // a pixel layer ignores the mode
    Layer px = pixelLayer(2, 1);
    setPixel(px, 0, 0, Yellow);
    setPixel(px, 1, 1, LightBlue);
    const Canvas normal = composite(stackOf(2, 1, {base, px}));
    for (LayerMode m : {LayerMode::CharsOnly, LayerMode::ColoursOnly}) {
        px.props.mode = m;
        CHECK(composite(stackOf(2, 1, {base, px})) == normal);
    }
}

TEST_CASE("layers are placed at their offsets and clipped to the canvas", "[layers][composite]") {
    const Cell x = cell(U'x', White, Black);
    const auto at = [&](int ox, int oy, int w, int h) {
        return composite(stackOf(6, 4, {filledLayer(LayerKind::Cells, w, h, x, ox, oy)}));
    };
    auto covered = [&](const Canvas& c, CellRect r) {
        for (int y = 0; y < 4; ++y)
            for (int cx = 0; cx < 6; ++cx)
                if ((c.at(cx, y) == x) != r.contains(cx, y))
                    return false;
        return true;
    };
    CHECK(covered(at(-1, -1, 3, 3), {0, 0, 2, 2}));
    CHECK(covered(at(5, 3, 3, 3), {5, 3, 1, 1}));
    CHECK(covered(at(2, 1, 2, 2), {2, 1, 2, 2}));
    CHECK(covered(at(-10, -10, 30, 30), {0, 0, 6, 4}));
    CHECK(covered(at(6, 0, 3, 3), {}));
    CHECK(covered(at(0, -3, 3, 3), {}));
    CHECK(covered(at(INT_MAX - 1, 0, 3, 3), {}));
    CHECK(covered(at(INT_MIN, INT_MIN, 3, 3), {}));
    // compositeCell agrees, and is the ground off the canvas
    const LayerStack s = stackOf(6, 4, {filledLayer(LayerKind::Cells, 3, 3, x, -1, -1)});
    CHECK(compositeCell(s, 1, 1) == x);
    CHECK(compositeCell(s, 2, 1) == Cell{});
    CHECK(compositeCell(s, -1, -1) == Cell{});
}

TEST_CASE("pixel layers composite by the iCE setting", "[layers][composite][ice]") {
    const auto over = [](const Cell& below, int py, std::uint8_t colour, bool ice) {
        Layer px = pixelLayer(1, 1);
        setPixel(px, 0, py, colour);
        CompositeOptions o;
        o.ice = ice;
        return composite(stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, below), px}), o).at(0, 0);
    };
    const Cell white = cell(kFullBlock, White, Black);
    // iCE on: two bright colours, a bright background
    Cell c = over(white, 1, LightRed, true);
    CHECK(c == cell(kUpperHalf, White, LightRed));
    // iCE off: the pixel keeps its colour, the half under shows dark (the
    // layer on top wins; plotting flat keeps the half already there)
    c = over(white, 1, LightRed, false);
    CHECK(halfBlocks(c, false) == HalfPair{Grey, LightRed});
    CHECK(halfBlocks(setHalfBlock(white, true, LightRed, false), false) == HalfPair{White, Red});
    // iCE off reads a bright background as its dark colour, blinking: the
    // blink stays with the half under, which is the foreground here
    const Cell brightBg = cell(kUpperHalf, White, Yellow);
    c = over(brightBg, 0, Red, false);
    CHECK(halfBlocks(c, false) == HalfPair{Red, Brown});
    CHECK(c.attrs == 0);
    c = over(brightBg, 0, Red, true);
    CHECK(halfBlocks(c, true) == HalfPair{Red, Yellow});
    CHECK(c.attrs == 0);
    // a layer setting both halves to two bright colours: the top keeps it with iCE off
    Layer both = pixelLayer(1, 1);
    setPixel(both, 0, 0, LightRed);
    setPixel(both, 0, 1, Yellow);
    CompositeOptions off;
    off.ice = false;
    CHECK(halfBlocks(composite(stackOf(1, 1, {both})).at(0, 0)) == HalfPair{LightRed, Yellow});
    CHECK(halfBlocks(composite(stackOf(1, 1, {both}), off).at(0, 0), false) == HalfPair{LightRed, Brown});
}

TEST_CASE("a pixel half over a pixel cell keeps its colour and the half under", "[layers][composite][ice]") {
    // every pixel cell, blinking or not, both iCE settings, every colour in
    // either half: the pixel shows its colour, the other half the colour
    // under it (its dark counterpart with iCE off where two bright colours
    // would share the cell), and the cell blinks only where the half under
    // was blinking (the foreground, a different colour from the background)
    // and the pixel's colour can be the background (dark, with iCE off):
    // then the half under stays the foreground. Without those two cases it
    // is what the half-block tools paint.
    int same = 0;
    for (bool ice : {true, false})
        for (char32_t ch : {U' ', kFullBlock, kUpperHalf, kLowerHalf})
            for (int fg = 0; fg < 16; ++fg)
                for (int bg = 0; bg < 16; ++bg)
                    for (std::uint8_t a : {std::uint8_t(0), attr::Blink}) {
                        const Cell below = cell(ch, std::uint8_t(fg), std::uint8_t(bg), a);
                        const LayerStack base = stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, below)});
                        for (int py : {0, 1})
                            for (int colour = 0; colour < 16; ++colour) {
                                LayerStack s = base;
                                Layer px = pixelLayer(1, 1);
                                setPixel(px, 0, py, std::uint8_t(colour));
                                s.add(px);
                                CompositeOptions o;
                                o.ice = ice;
                                const Cell got = composite(s, o).at(0, 0);
                                const HalfPair under = *halfBlocks(below, ice);
                                std::uint8_t other = py ? under.top : under.bottom;
                                const Cell shownBelow = iceCell(below, ice);
                                const std::uint8_t fgBelow = shownBelow.fg.paletteIndex();
                                const bool blinkingHalf = (shownBelow.attrs & attr::Blink) && other == fgBelow &&
                                                          fgBelow != shownBelow.bg.paletteIndex();
                                const bool clash = !ice && colour >= 8 && other >= 8 && other != colour;
                                if (clash)
                                    other &= 7;
                                const HalfPair want = py ? HalfPair{other, std::uint8_t(colour)}
                                                         : HalfPair{std::uint8_t(colour), other};
                                const bool blinkUnder = iceCell(below, ice).attrs & attr::Blink;
                                const bool keepsBlink = blinkingHalf && !clash && colour != other && colour < 8;
                                const std::uint8_t wantAttrs = keepsBlink ? attr::Blink : 0;
                                const bool fgUnder = got.ch == (py ? kUpperHalf : kLowerHalf);
                                if (!clash && !blinkUnder) {
                                    const Cell tool = setHalfBlock(below, py == 1, std::uint8_t(colour), ice);
                                    CHECK(halfBlocks(tool, ice) == want);
                                    ++same;
                                }
                                if (halfBlocks(got, ice) != want || iceCell(got, ice).attrs != wantAttrs ||
                                    (keepsBlink && !fgUnder)) {
                                    INFO("ice " << ice << " ch " << int(ch) << " fg " << fg << " bg " << bg
                                                << " blink " << int(a) << " py " << py << " colour " << colour);
                                    FAIL();
                                }
                            }
                    }
    CHECK(same > 10000);
}

TEST_CASE("with iCE off a bright pixel over a shade keeps its colour", "[layers][composite][ice]") {
    // the shade's other colour shows dark with iCE off, so it takes the
    // free half instead of darkening the pixel
    const auto over = [](const Cell& below, int py, std::uint8_t colour) {
        Layer px = pixelLayer(1, 1);
        setPixel(px, 0, py, colour);
        CompositeOptions o;
        o.ice = false;
        return composite(stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, below), px}), o).at(0, 0);
    };
    CHECK(halfBlocks(over(cell(kShadeDark, DarkGrey, Black), 0, Yellow), false) == HalfPair{Yellow, Black});
    CHECK(halfBlocks(over(cell(kShadeMedium, DarkGrey, Black), 0, LightRed), false) == HalfPair{LightRed, Black});
    // no clash: the shade's main colour as with iCE on
    CHECK(halfBlocks(over(cell(kShadeDark, DarkGrey, Black), 0, Red), false) == HalfPair{Red, DarkGrey});
    CHECK(halfBlocks(over(cell(kShadeLight, White, Blue), 1, Yellow), false) == HalfPair{Blue, Yellow});
}

TEST_CASE("with iCE off a pixel doesn't take the blink of the cell under it", "[layers][composite][ice]") {
    const auto over = [](const Cell& below, int py, std::uint8_t colour) {
        Layer px = pixelLayer(1, 1);
        setPixel(px, 0, py, colour);
        CompositeOptions o;
        o.ice = false;
        return composite(stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, below), px}), o).at(0, 0);
    };
    // a bright background shows dark and blinking; the pixel becomes the foreground
    Cell c = over(cell(U' ', Grey, Yellow), 0, LightRed);
    CHECK(halfBlocks(c, false) == HalfPair{LightRed, Brown});
    CHECK(c.attrs == 0);
    c = over(cell(kShadeLight, White, Blue, attr::Blink), 0, LightRed);
    CHECK(halfBlocks(c, false) == HalfPair{LightRed, Blue});
    CHECK(c.attrs == 0);
    // a blinking half under that stays the foreground keeps its blink
    c = over(cell(kUpperHalf, Red, Blue, attr::Blink), 1, Green);
    CHECK(c == cell(kUpperHalf, Red, Green, attr::Blink));
    // and is written as the foreground to keep it: ▄ rather than ▀ Green/Red
    c = over(cell(kLowerHalf, Red, Blue, attr::Blink), 0, Green);
    CHECK(c == cell(kLowerHalf, Red, Green, attr::Blink));
    c = over(cell(kShadeDark, Red, Black, attr::Blink), 0, Green);
    CHECK(c == cell(kLowerHalf, Red, Green, attr::Blink));
    c = over(cell(kFullBlock, Cyan, Black, attr::Blink), 1, Blue);
    CHECK(c == cell(kUpperHalf, Cyan, Blue, attr::Blink));
    // ▒ shows its darker colour on top: Red, its foreground, blinks
    c = over(cell(kShadeMedium, Red, Grey, attr::Blink), 1, Green);
    CHECK(c == cell(kUpperHalf, Red, Green, attr::Blink));
    // a bright pixel can only be the foreground with iCE off: the blink goes
    c = over(cell(kLowerHalf, Red, Blue, attr::Blink), 0, Yellow);
    CHECK(c == cell(kUpperHalf, Yellow, Red));
    // a half under that showed the background wasn't blinking and doesn't start
    c = over(cell(kShadeLight, Red, Blue, attr::Blink), 1, Green);
    CHECK(c == cell(kUpperHalf, Blue, Green));
    c = over(cell(kLowerHalf, Red, Blue, attr::Blink), 1, Green);
    CHECK(c == cell(kUpperHalf, Blue, Green));
    c = over(cell(U'a', Red, Blue, attr::Blink), 1, Green);
    CHECK(c == cell(kUpperHalf, Blue, Green));
    // nor does a blink that showed nothing, the foreground the background's colour
    c = over(cell(kLowerHalf, Red, Red, attr::Blink), 0, Green);
    CHECK(c == cell(kUpperHalf, Green, Red));
    // a pixel of the half under's colour fills the cell, which doesn't blink
    c = over(cell(kLowerHalf, Red, Blue, attr::Blink), 0, Red);
    CHECK(halfBlocks(c, false) == HalfPair{Red, Red});
    CHECK(c.attrs == 0);
}

TEST_CASE("hidden layers add nothing; locked ones composite as any other", "[layers][composite]") {
    const Cell under = cell(U'.', Grey, Black), x = cell(U'x', White, Red);
    const Layer base = filledLayer(LayerKind::Cells, 2, 2, under);
    Layer l = filledLayer(LayerKind::Cells, 1, 1, x);
    const Canvas shown = composite(stackOf(2, 2, {base, l}));
    CHECK(shown.at(0, 0) == x);
    l.props.locked = true;
    l.props.alphaLocked = true;
    CHECK(composite(stackOf(2, 2, {base, l})) == shown);
    l.props.visible = false;
    CHECK(composite(stackOf(2, 2, {base, l})) == composite(stackOf(2, 2, {base})));
    // generator and reference layers aren't composited (nor exported)
    for (LayerKind k : {LayerKind::Generator, LayerKind::Reference}) {
        Layer r = filledLayer(k, 2, 2, x);
        CHECK(flatten(stackOf(2, 2, {base, r})) == composite(stackOf(2, 2, {base})));
    }
}

TEST_CASE("24-bit cells pass through unchanged", "[layers][composite][rgb]") {
    Cell rgb = cell(U'R', 0, 0, attr::Blink);
    rgb.fg = Color::fromRgb({10, 200, 30});
    rgb.bg = Color::fromRgb({250, 250, 0});
    for (bool ice : {true, false}) {
        CompositeOptions o;
        o.ice = ice;
        const Layer base = filledLayer(LayerKind::Cells, 2, 1, rgb);
        CHECK(composite(stackOf(2, 1, {base}), o).at(0, 0) == rgb);
        // a transparent pixel layer over it changes nothing
        CHECK(composite(stackOf(2, 1, {base, pixelLayer(2, 1)}), o).at(0, 0) == rgb);
    }
    // a pixel half over it reads its background as a palette colour, as the tools do
    Layer px = pixelLayer(2, 1);
    setPixel(px, 0, 0, Blue);
    const Cell c = composite(stackOf(2, 1, {filledLayer(LayerKind::Cells, 2, 1, rgb), px})).at(0, 0);
    CHECK(halfBlocks(c) == HalfPair{Blue, Yellow});
}

// ---- properties ----

TEST_CASE("compositing one opaque full-canvas cells layer is the identity", "[layers][composite][property]") {
    Rng r(11);
    for (int trial = 0; trial < 20; ++trial) {
        Canvas c(r.range(1, 30), r.range(1, 20));
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                c.at(x, y) = randomOpaqueCell(r);
        for (bool ice : {true, false}) {
            CompositeOptions o;
            o.ice = ice;
            CHECK(composite(LayerStack::fromCanvas(c), o) == c);
        }
    }
}

TEST_CASE("hiding a layer equals removing it", "[layers][composite][property]") {
    Rng r(12);
    for (int trial = 0; trial < 200; ++trial) {
        const LayerStack s = randomStack(r, 24, 12);
        for (std::size_t i = 0; i < s.size(); ++i) {
            LayerStack hidden = s, removed = s;
            hidden[i].props.visible = false;
            removed.remove(s[i].props.id);
            CompositeOptions o;
            o.ice = r.chance(50);
            REQUIRE(composite(hidden, o) == composite(removed, o));
        }
    }
}

TEST_CASE("moving a layer equals compositing it at the new offset", "[layers][composite][property]") {
    Rng r(13);
    for (int trial = 0; trial < 300; ++trial) {
        LayerStack s = randomStack(r, 24, 12);
        const std::size_t i = std::size_t(r.range(0, int(s.size()) - 1));
        const int dx = r.range(-15, 15), dy = r.range(-10, 10);
        LayerStack moved = s;
        moved[i].props.x += dx;
        moved[i].props.y += dy;
        // the same cells written into a canvas-sized layer at their new place
        LayerStack placed = s;
        Layer& p = placed[i];
        Layer flat = makeLayer(p.props.kind, 24, 12);
        flat.props = p.props;
        flat.props.x = flat.props.y = 0;
        for (int y = 0; y < p.height(); ++y)
            for (int x = 0; x < p.width(); ++x) {
                const int cx = p.props.x + dx + x, cy = p.props.y + dy + y;
                if (cx >= 0 && cy >= 0 && cx < 24 && cy < 12)
                    flat.cells.at(cx, cy) = p.cells.at(x, y);
            }
        p = flat;
        CompositeOptions o;
        o.ice = r.chance(50);
        REQUIRE(composite(moved, o) == composite(placed, o));
    }
}

TEST_CASE("order matters exactly where layers overlap", "[layers][composite][property]") {
    Rng r(14);
    int differed = 0;
    for (int trial = 0; trial < 300; ++trial) {
        LayerStack s = randomStack(r, 24, 12);
        if (s.size() < 2)
            continue;
        const std::size_t i = std::size_t(r.range(0, int(s.size()) - 2));
        LayerStack swapped = s;
        swapped.move(s[i].props.id, i + 1);
        const CellRect overlap = intersect(s[i].bounds(), s[i + 1].bounds());
        const Canvas a = composite(s), b = composite(swapped);
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 24; ++x)
                if (!overlap.contains(x, y))
                    REQUIRE(a.at(x, y) == b.at(x, y));
        differed += !(a == b);
    }
    CHECK(differed > 0);
    // two opaque layers: the top one shows on the overlap only
    const Cell p = cell(U'p', Red, Black), q = cell(U'q', Blue, Black);
    const Layer lp = filledLayer(LayerKind::Cells, 3, 3, p, 0, 0), lq = filledLayer(LayerKind::Cells, 3, 3, q, 2, 1);
    const Canvas pq = composite(stackOf(6, 4, {lp, lq})), qp = composite(stackOf(6, 4, {lq, lp}));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x)
            CHECK((pq.at(x, y) == qp.at(x, y)) == !CellRect{2, 1, 1, 2}.contains(x, y));
}

TEST_CASE("a dirty-rect recomposite equals a full composite on that rect", "[layers][composite][dirty]") {
    Rng r(15);
    for (int trial = 0; trial < 300; ++trial) {
        LayerStack s = randomStack(r, 24, 12);
        CompositeOptions o;
        o.ice = r.chance(50);
        Canvas shown = composite(s, o);
        // a few edits in a row, each recomposited by what it changed
        for (int step = 0; step < 4; ++step) {
            const std::size_t i = std::size_t(r.range(0, int(s.size()) - 1));
            Layer& l = s[i];
            CellRect dirty;
            switch (r.range(0, 3)) {
            case 0: {  // a cell edit in canvas cells, as a tool makes it
                CellEdit e;
                const int n = r.range(1, 8);
                for (int k = 0; k < n; ++k) {
                    const int x = r.range(-2, 25), y = r.range(-2, 13);
                    const Cell* before = l.cellAt(x, y);
                    e.changes.push_back({x, y, before ? *before : Cell{}, randomCell(r, l.props.kind)});
                }
                const CellEdit le = toLayerEdit(l, e);
                le.apply(l.cells);
                dirty = dirtyRect(l, le);
                break;
            }
            case 1: {  // a move
                const CellRect from = l.bounds();
                l.props.x += r.range(-5, 5);
                l.props.y += r.range(-5, 5);
                dirty = unite(from, l.bounds(), s.canvasRect());
                break;
            }
            case 2:  // visibility or mode
                if (r.chance(50))
                    l.props.visible = !l.props.visible;
                else
                    l.props.mode = static_cast<LayerMode>(r.range(0, 2));
                dirty = l.bounds();
                break;
            default:  // a layer moved in the stack changes only what it covers
                dirty = l.bounds();
                s.move(l.props.id, std::size_t(r.range(0, int(s.size()) - 1)));
                break;
            }
            recomposite(s, shown, dirty, o);
            REQUIRE(shown == composite(s, o));
        }
        // any rect of a canvas holding anything: that rect composited, the rest left alone
        Canvas junk(24, 12);
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 24; ++x)
                junk.at(x, y) = randomOpaqueCell(r);
        const Canvas before = junk;
        const CellRect rect{r.range(-5, 20), r.range(-5, 10), r.range(0, 30), r.range(0, 20)};
        recomposite(s, junk, rect, o);
        const Canvas full = composite(s, o);
        const CellRect on = intersect(rect, s.canvasRect());
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 24; ++x) {
                REQUIRE(junk.at(x, y) == (on.contains(x, y) ? full.at(x, y) : before.at(x, y)));
                REQUIRE(compositeCell(s, x, y, o) == full.at(x, y));
            }
    }
    Canvas wrong(3, 3);
    CHECK_THROWS_AS(recomposite(LayerStack(4, 4), wrong, {0, 0, 1, 1}), LayerError);
}

TEST_CASE("a move to a far offset recomposites the cells it left", "[layers][composite][dirty]") {
    for (int far : {INT_MIN, INT_MAX, INT_MIN + 1, INT_MAX - 1}) {
        LayerStack s = stackOf(1, 1, {filledLayer(LayerKind::Cells, 1, 1, cell(U'X', White, Black))});
        Canvas out = composite(s);
        REQUIRE(out.at(0, 0).ch == U'X');
        const CellRect from = s[0].bounds();
        s[0].props.x = far;
        recomposite(s, out, unite(from, s[0].bounds(), s.canvasRect()));
        CHECK(out == composite(s));
        CHECK(out.at(0, 0) == Cell{});
        // and back
        const CellRect away = s[0].bounds();
        s[0].props.x = 0;
        recomposite(s, out, unite(away, s[0].bounds(), s.canvasRect()));
        CHECK(out.at(0, 0).ch == U'X');
    }
}

// ---- Phase 3's done-when ----

namespace {

int luma(std::uint8_t i) {
    const Rgb& c = kPalette[i & 15];
    return 299 * c.r + 587 * c.g + 114 * c.b;
}

// What the free half of a cut cell should show, written from the rule for
// shade cells and their fallbacks rather than from the compositor: the
// band's main colour, low on ░, high on ▓, and on ▒ low on top and high
// below; a pixel cell's own half. With iCE off a cell shows one bright
// colour only, so where that colour and the pixel are two bright colours
// the free half shows dark: a shade's background (always dark with iCE
// off), a pixel cell's dark counterpart.
std::uint8_t expectedFreeHalf(const Cell& under, bool bottom, std::uint8_t pixel, bool ice) {
    const Cell shown = iceCell(under, ice);
    const std::uint8_t fg = shown.fg.paletteIndex(), bg = shown.bg.paletteIndex();
    const auto pixelCell = halfBlocks(under, ice);
    std::uint8_t want = bg;
    if (pixelCell)
        want = bottom ? pixelCell->bottom : pixelCell->top;
    else if (shown.ch == kShadeDark)
        want = fg;
    else if (shown.ch == kShadeMedium)
        want = (luma(fg) < luma(bg)) == !bottom ? fg : bg;
    if (!ice && want >= 8 && pixel >= 8 && want != pixel)
        want = pixelCell ? std::uint8_t(want & 7) : bg;
    return want;
}

// Cells a pixel layer covers in one half only where either half shows the
// wrong colour: the free half not the colour above (a fallback speck), or
// the covered half not its pixel's colour (a speck on the shape's edge).
// The ladder lint can't see these (a half block is off the ladder, and
// pixel noise skips pixels beside shade cells), so the test counts
// them itself, as the rule for "no fallback specks" reads.
int cutCellSpecks(const Canvas& field, const Layer& px, const Canvas& out, bool ice) {
    int n = 0;
    for (int y = 0; y < field.height(); ++y)
        for (int x = 0; x < field.width(); ++x) {
            const LayerPixels p = layerPixels(px.cells.at(x, y));
            if (bool(p.top) == bool(p.bottom))
                continue;
            const auto halves = halfBlocks(out.at(x, y), ice);
            REQUIRE(halves);
            const bool freeBottom = bool(p.top);
            const std::uint8_t pixel = p.top ? *p.top : *p.bottom;
            const std::uint8_t covered = freeBottom ? halves->top : halves->bottom;
            const std::uint8_t free = freeBottom ? halves->bottom : halves->top;
            n += covered != pixel || free != expectedFreeHalf(field.at(x, y), freeBottom, pixel, ice);
        }
    return n;
}

} // namespace

TEST_CASE("shade under a pixel layer composites with no specks", "[layers][composite][golden]") {
    const Ramp steel = *findRamp("steel");
    constexpr int W = 80, H = 26;
    // a ramp-shaded field, as the ramp shade brush writes it: rungs 0-12,
    // two cells to a rung, down the canvas (a sky) and across it (a wall)
    for (bool across : {false, true}) {
        Canvas field(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int rung = across ? std::min(steel.topRung(), (x % 40) / 2) : y / 2;
                field.at(x, y) = stepRung(steel, Cell{}, rung).value_or(Cell{});
            }
        const auto clean = lint::checkLadder(field, steel);
        for (const auto& f : clean.findings)
            UNSCOPED_INFO("field: " << f.message);
        REQUIRE(clean.clean());

        // the shapes: a black silhouette, a yellow moon, a red bar, their
        // edges on odd pixel rows so they cut the shade cells in half. Off
        // the ramp but black, so the ladder judges the field round them,
        // not them (a White moon in a dark sky is a rung skip of its own)
        Layer px = pixelLayer(W, H);
        const auto put = [&](const Pixels& pixels, std::uint8_t colour) {
            for (const auto& [x, py] : pixels)
                setPixel(px, x, py, colour);
        };
        put(ellipseShape(5, 21, 30, 51, true, W, 2 * H), Black);
        put(ellipseShape(50, 5, 61, 16, true, W, 2 * H), Yellow);
        put(rectShape(34, 13, 45, 30, true, W, 2 * H), LightRed);
        put(lineShape(62, 41, 79, 27, W, 2 * H), Black);
        int halfCut = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const LayerPixels p = layerPixels(px.cells.at(x, y));
                halfCut += bool(p.top) != bool(p.bottom) && field.at(x, y).ch != U' ' &&
                           field.at(x, y).ch != kFullBlock;
            }
        REQUIRE(halfCut > 30);  // the test cuts plenty of shade cells

        for (bool ice : {true, false}) {
            INFO((across ? "across" : "down") << ", iCE " << (ice ? "on" : "off"));
            LayerStack s = LayerStack::fromCanvas(field);
            s.add(px);
            CompositeOptions o;
            o.ice = ice;
            const Canvas out = composite(s, o);
            // the ladder and speckle checks and pixel noise
            const auto rep = lint::checkLadder(out, steel);
            for (const auto& f : rep.findings)
                UNSCOPED_INFO(f.message);
            CHECK(rep.clean());
            CHECK(rep.noise <= 0.02);
            // and no speck on either side where a shape cuts a cell
            CHECK(cutCellSpecks(field, px, out, ice) == 0);

            // plotting the same pixels into the shaded canvas, as a flat
            // canvas would, leaves fallback specks: the test can see them
            Canvas flat = field;
            StrokeOptions so;
            so.ice = ice;
            for (std::uint8_t colour : {Black, Yellow, LightRed}) {
                std::vector<std::pair<int, int>> pts;
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                        for (int half : {0, 1}) {
                            const LayerPixels p = layerPixels(px.cells.at(x, y));
                            const auto& v = half ? p.bottom : p.top;
                            if (v && *v == colour)
                                pts.emplace_back(x, 2 * y + half);
                        }
                plotPixels(flat, pts, colour, so).apply(flat);
            }
            CHECK(cutCellSpecks(field, px, flat, ice) > 0);
        }
    }
}

// ---- timings ----

TEST_CASE("compositing 160x100 with 8 layers, and a dirty rect, is interactive", "[layers][composite][perf]") {
    Rng r(16);
    constexpr int W = 160, H = 100;
    LayerStack s(W, H);
    // a full background, then seven layers of every kind, mostly large
    s.add(bake(filledLayer(LayerKind::Cells, W, H, cell(kShadeMedium, White, Grey))));
    for (int i = 0; i < 7; ++i) {
        const LayerKind kind = i % 3 == 0 ? LayerKind::Pixels : i % 3 == 1 ? LayerKind::Cells : LayerKind::Text;
        Layer l = makeLayer(kind, W - 10 * i, H - 5 * i, {}, 5 * i, 2 * i);
        for (int y = 0; y < l.height(); ++y)
            for (int x = 0; x < l.width(); ++x)
                l.cells.at(x, y) = randomCell(r, kind);
        s.add(std::move(l));
    }
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

    Canvas out = composite(s);
    constexpr int kFull = 50;
    auto t0 = clock::now();
    for (int i = 0; i < kFull; ++i)
        out = composite(s);
    const double full = ms(clock::now() - t0) / kFull;

    // a brush dab's worth: 8x8 cells, recomposited after each edit
    constexpr int kDabs = 2000;
    t0 = clock::now();
    for (int i = 0; i < kDabs; ++i) {
        const CellRect dab{(i * 7) % (W - 8), (i * 3) % (H - 8), 8, 8};
        recomposite(s, out, dab);
    }
    const double dab = ms(clock::now() - t0) / kDabs;
    std::printf("composite 160x100, 8 layers: %.3f ms full, %.4f ms per 8x8 dirty rect\n", full, dab);
    CHECK(out == composite(s));
    // generous bounds (a debug or loaded machine), far under a frame
    CHECK(full < 50.0);
    CHECK(dab < 1.0);
}
