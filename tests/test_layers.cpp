// Layer model: kinds, properties, the stack, pixel layer cells and edits
// (PLAN §5).

#include "blockwright/layer.h"
#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/drawing.h"
#include "blockwright/edit.h"
#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <climits>

using namespace bw;

TEST_CASE("layer kinds and modes have stable names", "[layers]") {
    for (LayerKind k : {LayerKind::Cells, LayerKind::Pixels, LayerKind::Text, LayerKind::Generator,
                        LayerKind::Reference})
        CHECK(layerKindFromName(layerKindName(k)) == k);
    for (LayerMode m : {LayerMode::Normal, LayerMode::CharsOnly, LayerMode::ColoursOnly})
        CHECK(layerModeFromName(layerModeName(m)) == m);
    CHECK(layerKindName(LayerKind::Pixels) == "pixels");
    CHECK(layerModeName(LayerMode::ColoursOnly) == "colours-only");
    CHECK_FALSE(layerKindFromName("group"));
    CHECK_FALSE(layerModeFromName("Normal"));
}

TEST_CASE("a new layer is transparent, its size, at its offset", "[layers]") {
    const Layer l = makeLayer(LayerKind::Pixels, 5, 3, "moon", -2, 7);
    CHECK(l.width() == 5);
    CHECK(l.height() == 3);
    CHECK(l.bounds() == CellRect{-2, 7, 5, 3});
    CHECK(l.props.id == 0);
    CHECK(l.props.visible);
    CHECK_FALSE(l.props.locked);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x)
            CHECK(l.cells.at(x, y).transparent);
    CHECK(l.cellAt(-2, 7) == &l.cells.at(0, 0));
    CHECK(l.cellAt(2, 9) == &l.cells.at(4, 2));
    CHECK(l.cellAt(3, 9) == nullptr);
    CHECK(l.cellAt(-3, 7) == nullptr);
    CHECK(l.cellAt(-2, 10) == nullptr);
}

TEST_CASE("the stack adds, finds, moves and removes layers by id", "[layers]") {
    LayerStack s(20, 10);
    const LayerId a = s.add(makeLayer(LayerKind::Cells, 20, 10, "a"));
    const LayerId b = s.add(makeLayer(LayerKind::Pixels, 4, 4, "b"));
    const LayerId c = s.add(makeLayer(LayerKind::Text, 8, 1, "c"), 0);
    REQUIRE(s.size() == 3);
    CHECK(a != b);
    CHECK(b != c);
    // bottom first: c was put at the bottom
    CHECK(s[0].props.id == c);
    CHECK(s[1].props.id == a);
    CHECK(s[2].props.id == b);
    CHECK(s.find(b)->props.name == "b");
    CHECK(s.indexOf(a) == 1u);
    CHECK(s.find(0) == nullptr);
    CHECK(s.find(99) == nullptr);

    CHECK(s.move(c, 99));  // clamped to the top
    CHECK(s[2].props.id == c);
    CHECK(s[0].props.id == a);
    CHECK(s.move(c, 1));
    CHECK(s[0].props.id == a);
    CHECK(s[1].props.id == c);
    CHECK(s[2].props.id == b);
    CHECK_FALSE(s.move(42, 0));

    const auto removed = s.remove(c);
    REQUIRE(removed);
    CHECK(removed->props.name == "c");
    CHECK_FALSE(s.remove(c));
    CHECK(s.size() == 2);
    // an id is never given again
    const LayerId d = s.add(makeLayer(LayerKind::Cells, 1, 1));
    CHECK(d != a);
    CHECK(d != b);
    CHECK(d != c);
}

TEST_CASE("ids from a file are kept, and new ids go above them", "[layers]") {
    LayerStack s(4, 4);
    Layer l = makeLayer(LayerKind::Cells, 4, 4);
    l.props.id = 7;
    CHECK(s.add(l) == 7);
    CHECK_THROWS_AS(s.add(l), LayerError);
    CHECK(s.size() == 1);
    CHECK(s.add(makeLayer(LayerKind::Cells, 1, 1)) == 8);
    Layer low = makeLayer(LayerKind::Cells, 1, 1);
    low.props.id = 3;
    CHECK(s.add(low) == 3);
    CHECK(s.nextId() == 9);
    // the last id there is leaves none to give
    Layer last = makeLayer(LayerKind::Cells, 1, 1);
    last.props.id = UINT32_MAX;
    CHECK(s.add(last) == UINT32_MAX);
    CHECK_THROWS_AS(s.add(makeLayer(LayerKind::Cells, 1, 1)), LayerError);
}

TEST_CASE("once the last id is taken, ids stay exhausted", "[layers]") {
    const auto withId = [](LayerId id) {
        Layer l = makeLayer(LayerKind::Cells, 1, 1);
        l.props.id = id;
        return l;
    };
    {
        // 2, the last id, then 1: the automatic id must not be 2 again
        LayerStack s(1, 1);
        s.add(withId(2));
        s.add(withId(UINT32_MAX));
        s.add(withId(1));
        CHECK(s.nextId() == 0);
        CHECK_THROWS_AS(s.add(makeLayer(LayerKind::Cells, 1, 1)), LayerError);
        CHECK(s.size() == 3);
    }
    {
        // the last id, then 5: nextId doesn't drop below a held id
        LayerStack s(1, 1);
        s.add(withId(UINT32_MAX));
        s.add(withId(5));
        CHECK(s.nextId() == 0);
        CHECK_THROWS_AS(s.add(makeLayer(LayerKind::Cells, 1, 1)), LayerError);
        // removing the last id doesn't give it again either
        s.remove(UINT32_MAX);
        CHECK_THROWS_AS(s.add(makeLayer(LayerKind::Cells, 1, 1)), LayerError);
    }
    {
        // an explicit id below nextId never moves it back
        LayerStack s(1, 1);
        s.add(withId(10));
        s.add(withId(3));
        CHECK(s.nextId() == 11);
        CHECK(s.add(makeLayer(LayerKind::Cells, 1, 1)) == 11);
    }
}

TEST_CASE("a saved nextId is restored forwards only", "[layers]") {
    LayerStack s(1, 1);
    s.add(makeLayer(LayerKind::Cells, 1, 1));  // 1
    s.reserveIds(40);
    CHECK(s.nextId() == 40);
    s.reserveIds(7);
    CHECK(s.nextId() == 40);
    CHECK(s.add(makeLayer(LayerKind::Cells, 1, 1)) == 40);
    s.reserveIds(0);  // a file whose ids ran out
    CHECK(s.nextId() == 0);
    s.reserveIds(50);
    CHECK(s.nextId() == 0);
}

TEST_CASE("fromCanvas is one opaque background layer", "[layers]") {
    Canvas c(3, 2);
    c.at(1, 1).ch = U'x';
    c.at(2, 0).transparent = true;
    const LayerStack s = LayerStack::fromCanvas(c);
    REQUIRE(s.size() == 1);
    CHECK(s.width() == 3);
    CHECK(s.height() == 2);
    CHECK(s[0].props.kind == LayerKind::Cells);
    CHECK(s[0].props.name == "Background");
    CHECK(s[0].cells.at(1, 1).ch == U'x');
    CHECK_FALSE(s[0].cells.at(2, 0).transparent);
}

TEST_CASE("bake turns a text layer into cells, keeping everything else", "[layers]") {
    Layer t = makeLayer(LayerKind::Text, 3, 1, "title", 4, 5);
    t.cells.at(0, 0) = Cell{U'H', Color::indexed(White), Color::indexed(Blue), 0, false};
    t.props.id = 9;
    t.props.mode = LayerMode::CharsOnly;
    const Layer b = bake(t);
    CHECK(b.props.kind == LayerKind::Cells);
    LayerProperties want = t.props;
    want.kind = LayerKind::Cells;
    CHECK(b.props == want);
    CHECK(b.cells == t.cells);
    const Layer p = makeLayer(LayerKind::Pixels, 2, 2);
    CHECK(bake(p) == p);
}

TEST_CASE("a tool may paint a layer's cells unless it is locked, and opaque cells only under the alpha lock",
          "[layers]") {
    Layer l = makeLayer(LayerKind::Cells, 2, 1, "", 3, 0);
    l.cells.at(1, 0) = Cell{};
    CHECK(layerPaintable(l, 3, 0));
    CHECK(layerPaintable(l, 4, 0));
    CHECK_FALSE(layerPaintable(l, 5, 0));
    CHECK_FALSE(layerPaintable(l, 2, 0));
    l.props.alphaLocked = true;
    CHECK_FALSE(layerPaintable(l, 3, 0));
    CHECK(layerPaintable(l, 4, 0));
    l.props.locked = true;
    CHECK_FALSE(layerPaintable(l, 4, 0));
}

TEST_CASE("the alpha lock on a pixel layer is per pixel", "[layers][pixels]") {
    Layer px = makeLayer(LayerKind::Pixels, 3, 1);
    px.cells.at(0, 0) = pixelLayerCell({Red, std::nullopt});
    px.cells.at(1, 0) = pixelLayerCell({Red, Red});
    px.props.alphaLocked = true;
    // a top-only cell: its set top is paintable, its empty bottom isn't
    CHECK(layerPixelPaintable(px, 0, 0));
    CHECK_FALSE(layerPixelPaintable(px, 0, 1));
    CHECK(layerPixelPaintable(px, 1, 0));
    CHECK(layerPixelPaintable(px, 1, 1));
    CHECK_FALSE(layerPixelPaintable(px, 2, 0));
    CHECK(layerPaintable(px, 0, 0));  // the cell has a pixel to paint
    CHECK_FALSE(layerPaintable(px, 2, 0));
    // the stroke options carry the lock and say the layer holds pixels, so
    // a painter locks per pixel and doesn't refuse a half-set cell's set half
    CHECK(layerStrokeOptions(px).lockTransparent);
    CHECK(layerStrokeOptions(px).pixelLayer);
    px.props.alphaLocked = false;
    CHECK(layerPixelPaintable(px, 0, 1));
    CHECK(layerPixelPaintable(px, 2, 0));
    CHECK_FALSE(layerPixelPaintable(px, 3, 0));
    CHECK_FALSE(layerPixelPaintable(px, 0, 2));
    CHECK_FALSE(layerPixelPaintable(px, 0, -1));
    px.props.locked = true;
    CHECK_FALSE(layerPixelPaintable(px, 0, 0));
}

TEST_CASE("the alpha lock is the stroke's transparent lock", "[layers]") {
    Layer l = makeLayer(LayerKind::Cells, 2, 2);
    StrokeOptions base;
    base.ice = false;
    CHECK_FALSE(layerStrokeOptions(l, base).lockTransparent);
    l.props.alphaLocked = true;
    const StrokeOptions o = layerStrokeOptions(l, base);
    CHECK(o.lockTransparent);
    CHECK_FALSE(o.ice);
}

TEST_CASE("a pixel layer cell holds two pixels, each a colour or transparent", "[layers][pixels]") {
    CHECK(layerPixels(transparentCell()) == LayerPixels{});
    for (int t = -1; t < 16; ++t)
        for (int b = -1; b < 16; ++b) {
            LayerPixels p;
            if (t >= 0)
                p.top = std::uint8_t(t);
            if (b >= 0)
                p.bottom = std::uint8_t(b);
            const Cell c = pixelLayerCell(p);
            INFO("top " << t << " bottom " << b);
            CHECK(layerPixels(c) == p);
            CHECK(c.transparent == !(p.top && p.bottom));
            // both set: the cell halfblock.height writes, so it reads the same
            if (p.top && p.bottom)
                CHECK(halfBlocks(c) == HalfPair{*p.top, *p.bottom});
        }
    // set and clear one half, keeping the other
    Cell c = transparentCell();
    c = setLayerPixel(c, false, LightRed);
    CHECK(layerPixels(c) == LayerPixels{LightRed, std::nullopt});
    c = setLayerPixel(c, true, Blue);
    CHECK(layerPixels(c) == LayerPixels{LightRed, Blue});
    CHECK_FALSE(c.transparent);
    c = setLayerPixel(c, false, std::nullopt);
    CHECK(layerPixels(c) == LayerPixels{std::nullopt, Blue});
    c = setLayerPixel(c, true, std::nullopt);
    CHECK(layerPixels(c) == LayerPixels{});
    // an opaque cell that isn't pixels reads as its background
    const Cell shade{kShadeMedium, Color::indexed(White), Color::indexed(Grey), 0, false};
    CHECK(layerPixels(shade) == LayerPixels{Grey, Grey});
}

TEST_CASE("edits move between canvas and layer cells by the offset", "[layers]") {
    Layer l = makeLayer(LayerKind::Cells, 4, 3, "", 10, -1);
    const Cell x{U'x', Color::indexed(White), Color::indexed(Black), 0, false};
    // built against the layer's own cells (transparent)
    CellEdit canvasEdit;
    canvasEdit.changes = {
        {10, 0, transparentCell(), x},   // layer (0, 1)
        {13, 1, transparentCell(), x},   // layer (3, 2)
        {14, 1, transparentCell(), x},   // past its right edge
        {11, -2, transparentCell(), x},  // above it
        {9, 0, transparentCell(), x},    // left of it
    };
    const CellEdit e = toLayerEdit(l, canvasEdit);
    REQUIRE(e.changes.size() == 2);
    CHECK(e.changes[0] == CellChange{0, 1, transparentCell(), x});
    CHECK(e.changes[1] == CellChange{3, 2, transparentCell(), x});
    const CellEdit back = toCanvasEdit(l, e);
    REQUIRE(back.changes.size() == 2);
    CHECK(back.changes[0] == canvasEdit.changes[0]);
    CHECK(back.changes[1] == canvasEdit.changes[1]);
    CHECK(dirtyRect(l, e) == CellRect{10, 0, 4, 2});
    CHECK(dirtyRect(l, CellEdit{}).empty());
    // applied to the layer's own cells
    e.apply(l.cells);
    CHECK(l.cells.at(0, 1) == x);
    CHECK(l.cells.at(3, 2) == x);
}

TEST_CASE("a layer edit's before is the layer's cell, so undo restores it", "[layers]") {
    // a tool builds its edit against the composite it shows: the layer's
    // transparent cell shows the background's ░ there
    Layer l = makeLayer(LayerKind::Cells, 2, 1, "", 1, 0);
    const Layer original = l;
    const Cell shownThere{kShadeLight, Color::indexed(White), Color::indexed(Blue), 0, false};
    const Cell a{U'A', Color::indexed(White), Color::indexed(Black), 0, false};
    CellEdit canvasEdit;
    canvasEdit.changes = {{1, 0, shownThere, a}, {2, 0, shownThere, transparentCell()}};
    const CellEdit e = toLayerEdit(l, canvasEdit);
    // the second change leaves the layer's cell as it is: dropped
    REQUIRE(e.changes.size() == 1);
    CHECK(e.changes[0].before == transparentCell());
    e.apply(l.cells);
    CHECK(l.cells.at(0, 0) == a);
    e.revert(l.cells);
    CHECK(l == original);
    CHECK(l.cells.at(0, 0).transparent);
}

TEST_CASE("a layer edit names each cell once, its last change", "[layers]") {
    Layer l = makeLayer(LayerKind::Cells, 3, 1, "", 0, 0);
    const Layer original = l;
    const Cell a{U'A', Color::indexed(White), Color::indexed(Black), 0, false};
    const Cell b{U'B', Color::indexed(Yellow), Color::indexed(Blue), 0, false};
    CellEdit canvasEdit;
    canvasEdit.changes = {
        {0, 0, transparentCell(), a}, {1, 0, transparentCell(), a}, {2, 0, transparentCell(), a},
        {0, 0, a, transparentCell()},  // (0, 0) back as it was: nothing to do there
        {1, 0, a, b},                  // (1, 0) ends as b
    };
    const CellEdit e = toLayerEdit(l, canvasEdit);
    REQUIRE(e.changes.size() == 2);
    CHECK(e.changes[0] == CellChange{1, 0, transparentCell(), b});
    CHECK(e.changes[1] == CellChange{2, 0, transparentCell(), a});
    e.apply(l.cells);
    CHECK(l.cells.at(0, 0).transparent);
    CHECK(l.cells.at(1, 0) == b);
    CHECK(l.cells.at(2, 0) == a);
    e.revert(l.cells);
    CHECK(l == original);
}

TEST_CASE("rectangles intersect and unite", "[layers]") {
    CHECK(intersect({0, 0, 4, 4}, {2, 2, 4, 4}) == CellRect{2, 2, 2, 2});
    CHECK(intersect({0, 0, 4, 4}, {4, 0, 4, 4}).empty());
    CHECK(intersect({0, 0, 4, 4}, {}).empty());
    CHECK(unite({0, 0, 1, 1}, {3, 2, 1, 1}) == CellRect{0, 0, 4, 3});
    CHECK(unite({}, {3, 2, 1, 1}) == CellRect{3, 2, 1, 1});
    CHECK(unite({3, 2, 1, 1}, {}) == CellRect{3, 2, 1, 1});
    // far offsets don't overflow
    CHECK(intersect({INT_MAX - 1, 0, 5, 1}, {0, 0, 10, 1}).empty());
    CHECK(intersect({INT_MIN, 0, INT_MAX, 1}, {-5, 0, 10, 1}) == CellRect{-5, 0, 4, 1});
    // saturates when the union is wider than int: clip to the canvas first
    CHECK(unite({INT_MIN, 0, 1, 1}, {INT_MAX - 1, 0, 1, 1}).width == INT_MAX);
    CHECK(unite({INT_MIN, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 4, 4}) == CellRect{0, 0, 1, 1});
    CHECK(unite({INT_MIN, 0, 1, 1}, {INT_MAX - 1, 0, 1, 1}, {0, 0, 4, 4}).empty());
    CHECK(unite({-3, -3, 5, 5}, {3, 3, 5, 5}, {0, 0, 4, 4}) == CellRect{0, 0, 4, 4});
    CHECK(intersect({-3, 0, 5, 1}, {0, 0, 10, 1}) == CellRect{0, 0, 2, 1});
}

// ---- editing a layer (Phase 3b) --------------------------------------------

namespace {

Cell glyphCell(char32_t ch, std::uint8_t fg = White, std::uint8_t bg = Black) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

} // namespace

TEST_CASE("a cells layer stores every transparent cell as the plain one", "[layers]") {
    const Layer l = makeLayer(LayerKind::Cells, 3, 1);
    Cell odd = glyphCell(U'x', Red);
    odd.transparent = true;
    const auto e = fitLayerEdit(l, CellEdit{{{0, 0, Cell{}, odd}, {1, 0, Cell{}, glyphCell(U'a')}}});
    REQUIRE(e);
    // the odd transparent cell is the layer's own cell already: dropped
    REQUIRE(e->changes.size() == 1);
    CHECK(e->changes[0].x == 1);
    CHECK(e->changes[0].before == transparentCell());
    CHECK(e->changes[0].after == glyphCell(U'a'));
}

TEST_CASE("a pixel layer takes pixels and refuses glyphs", "[layers][pixels]") {
    const Layer px = makeLayer(LayerKind::Pixels, 3, 1);
    // a half block written by a painter is stored as its pixels
    const auto e = fitLayerEdit(px, CellEdit{{{0, 0, Cell{}, glyphCell(kUpperHalf, Red, Blue)}}});
    REQUIRE(e);
    REQUIRE(e->changes.size() == 1);
    CHECK(layerPixels(e->changes[0].after) == LayerPixels{Red, Blue});
    // a half pixel (transparent, one half set) is kept as it is
    const Cell half = setLayerPixel(transparentCell(), true, Yellow);
    const auto h = fitLayerEdit(px, CellEdit{{{1, 0, Cell{}, half}}});
    REQUIRE(h);
    CHECK(layerPixels(h->changes[0].after) == LayerPixels{std::nullopt, Yellow});
    // a shade, a letter or a sideways half block can't be held
    CHECK_FALSE(fitLayerEdit(px, CellEdit{{{0, 0, Cell{}, glyphCell(kShadeMedium)}}}));
    CHECK_FALSE(fitLayerEdit(px, CellEdit{{{0, 0, Cell{}, glyphCell(U'a')}}}));
    CHECK_FALSE(fitLayerEdit(px, CellEdit{{{0, 0, Cell{}, glyphCell(U'▌')}}}));
    // erasing to a transparent cell is fine
    const auto erase = fitLayerEdit(px, CellEdit{{{2, 0, Cell{}, transparentCell()}}});
    REQUIRE(erase);
    CHECK(erase->empty());
}

TEST_CASE("a half pixel on a cells layer goes over what shows below it", "[layers][pixels]") {
    Layer l = makeLayer(LayerKind::Cells, 3, 1);
    l.props.x = 1;
    const Cell half = setLayerPixel(transparentCell(), false, Red);
    // `below` is asked for the canvas cell, here the layer's offset plus 1
    std::vector<std::pair<int, int>> asked;
    const auto blue = [&](int x, int y) {
        asked.emplace_back(x, y);
        return pixelLayerCell({Blue, Blue});
    };
    const auto e = fitLayerEdit(l, CellEdit{{{2, 0, Cell{}, half}}}, blue);
    REQUIRE(e);
    REQUIRE(e->changes.size() == 1);
    CHECK(asked == std::vector<std::pair<int, int>>{{2, 0}});
    CHECK_FALSE(e->changes[0].after.transparent);
    CHECK(layerPixels(e->changes[0].after) == LayerPixels{Red, Blue});
    // with nothing below, over the blank ground
    const auto ground = fitLayerEdit(l, CellEdit{{{2, 0, Cell{}, half}}});
    REQUIRE(ground);
    CHECK(layerPixels(ground->changes[0].after) == LayerPixels{Red, Black});
    // a hole is still the plain transparent cell, and below isn't asked
    asked.clear();
    Cell hole = glyphCell(U'x', Red);
    hole.transparent = true;
    l.cells.at(0, 0) = glyphCell(U'a');
    const auto cleared = fitLayerEdit(l, CellEdit{{{1, 0, Cell{}, hole}}}, blue);
    REQUIRE(cleared);
    CHECK(cleared->changes[0].after == transparentCell());
    CHECK(asked.empty());
}

TEST_CASE("an alpha-locked layer keeps every edit off its transparency", "[layers][pixels]") {
    Layer l = makeLayer(LayerKind::Cells, 3, 1);
    l.cells.at(0, 0) = glyphCell(U'a');
    l.props.alphaLocked = true;
    const auto e = fitLayerEdit(l, CellEdit{{{0, 0, Cell{}, transparentCell()},
                                             {1, 0, Cell{}, glyphCell(U'b')},
                                             {2, 0, Cell{}, setLayerPixel(transparentCell(), false, Red)}}});
    REQUIRE(e);
    CHECK(e->empty());
    // opaque over opaque goes through
    const auto recolour = fitLayerEdit(l, CellEdit{{{0, 0, Cell{}, glyphCell(U'a', Red)}}});
    REQUIRE(recolour);
    REQUIRE(recolour->changes.size() == 1);
    CHECK(recolour->changes[0].after == glyphCell(U'a', Red));

    // a pixel layer: per pixel
    Layer px = makeLayer(LayerKind::Pixels, 2, 1);
    px.cells.at(0, 0) = pixelLayerCell({Blue, std::nullopt});
    px.props.alphaLocked = true;
    const auto p = fitLayerEdit(px, CellEdit{{{0, 0, Cell{}, pixelLayerCell({Red, std::nullopt})},
                                              {1, 0, Cell{}, pixelLayerCell({Red, std::nullopt})}}});
    REQUIRE(p);
    REQUIRE(p->changes.size() == 1);  // the set pixel repainted, the clear cell left
    CHECK(layerPixels(p->changes[0].after) == LayerPixels{Red, std::nullopt});
    const auto both = fitLayerEdit(px, CellEdit{{{0, 0, Cell{}, pixelLayerCell({Red, Red})}}});
    REQUIRE(both);
    CHECK(both->empty());
    const auto clear = fitLayerEdit(px, CellEdit{{{0, 0, Cell{}, transparentCell()}}});
    REQUIRE(clear);
    CHECK(clear->empty());
}

TEST_CASE("a layer's canvas is its cells in place, transparent round them", "[layers]") {
    Layer l = makeLayer(LayerKind::Cells, 2, 2, "", 1, 1);
    l.cells.at(0, 0) = glyphCell(U'a');
    l.cells.at(1, 1) = glyphCell(U'b');
    const Canvas c = layerCanvas(l, 4, 3);
    CHECK(c.width() == 4);
    CHECK(c.height() == 3);
    CHECK(c.at(1, 1) == glyphCell(U'a'));
    CHECK(c.at(2, 2) == glyphCell(U'b'));
    CHECK(c.at(2, 1) == transparentCell());
    CHECK(c.at(0, 0) == transparentCell());
    CHECK(c.at(3, 2) == transparentCell());
}

TEST_CASE("painting a pixel layer sets one pixel and keeps the other clear", "[layers][pixels]") {
    Layer px = makeLayer(LayerKind::Pixels, 3, 2);
    const Canvas cv = layerCanvas(px, 3, 2);
    const CellEdit e = plotPixels(cv, {{0, 0}, {1, 3}}, LightGreen, layerStrokeOptions(px));
    const auto fit = fitLayerEdit(px, e);
    REQUIRE(fit);
    fit->apply(px.cells);
    CHECK(layerPixels(px.cells.at(0, 0)) == LayerPixels{LightGreen, std::nullopt});
    CHECK(layerPixels(px.cells.at(1, 1)) == LayerPixels{std::nullopt, LightGreen});
    CHECK(px.cells.at(0, 0).transparent);
    CHECK(px.cells.at(2, 0) == transparentCell());

    // under the alpha lock only the set pixels take paint
    px.props.alphaLocked = true;
    const Canvas locked = layerCanvas(px, 3, 2);
    const CellEdit l = plotPixels(locked, {{0, 0}, {0, 1}, {2, 0}}, Red, layerStrokeOptions(px));
    const auto lf = fitLayerEdit(px, l);
    REQUIRE(lf);
    lf->apply(px.cells);
    CHECK(layerPixels(px.cells.at(0, 0)) == LayerPixels{Red, std::nullopt});
    CHECK(px.cells.at(2, 0) == transparentCell());
}

TEST_CASE("a fill on a pixel layer fills the clear pixels as a region", "[layers][pixels]") {
    // a 3x1 layer: a bar of set pixels down the middle column splits it
    Layer px = makeLayer(LayerKind::Pixels, 3, 1);
    px.cells.at(1, 0) = pixelLayerCell({Blue, Blue});
    const Canvas cv = layerCanvas(px, 3, 1);
    const StrokeOptions o = layerStrokeOptions(px);
    auto region = floodPixels(cv, 0, 0, o);
    std::sort(region.begin(), region.end());
    CHECK(region == std::vector<std::pair<int, int>>{{0, 0}, {0, 1}});
    auto bar = floodPixels(cv, 1, 1, o);
    std::sort(bar.begin(), bar.end());
    CHECK(bar == std::vector<std::pair<int, int>>{{1, 0}, {1, 1}});
}

TEST_CASE("erasing leaves the cell it is given", "[layers][edit]") {
    Canvas c(3, 1);
    c.at(0, 0) = glyphCell(U'a');
    c.at(1, 0) = glyphCell(U'b');
    const Cell clear = transparentCell();
    const EditResult del = deleteCell(c, {0, 0}, clear);
    REQUIRE(del.edit.changes.size() == 1);
    CHECK(del.edit.changes[0].after == clear);
    const EditResult back = backspace(c, {2, 0}, clear);
    REQUIRE(back.edit.changes.size() == 1);
    CHECK(back.edit.changes[0].x == 1);
    CHECK(back.edit.changes[0].after == clear);
    // the default is a blank cell, as before layers
    CHECK(deleteCell(c, {0, 0}).edit.changes[0].after == Cell{});
}

TEST_CASE("selections clear to the cell given and copy holes as transparent cells", "[layers][selection]") {
    Canvas c(3, 1);
    c.at(0, 0) = glyphCell(U'a');
    c.at(1, 0) = glyphCell(U'b');
    const CellEdit cleared = clearSelection(c, Selection(CellRect{0, 0, 2, 1}), transparentCell());
    REQUIRE(cleared.changes.size() == 2);
    for (const CellChange& ch : cleared.changes)
        CHECK(ch.after == transparentCell());
    const CellEdit plain = clearSelection(c, Selection(CellRect{0, 0, 1, 1}));
    CHECK(plain.changes[0].after == Cell{});
}

TEST_CASE("a pasted half pixel writes, a hole doesn't", "[layers][selection][pixels]") {
    const Cell under = glyphCell(U'a');
    const Cell half = setLayerPixel(transparentCell(), false, Red);
    // over a letter it replaces it, the letter's background below
    CHECK(pasteCell(half, under, PasteMode::Normal) == pixelLayerCell({Red, under.bg.paletteIndex()}));
    CHECK(pasteCell(half, under, PasteMode::Under) == std::nullopt);  // the cell under isn't blank
    CHECK_FALSE(pasteCell(half, under, PasteMode::Transparent));
    CHECK_FALSE(pasteCell(transparentCell(), under, PasteMode::Normal));
    // over a hole it is the half pixel alone
    CHECK(pasteCell(half, transparentCell(), PasteMode::Normal) == half);
    CHECK(pasteCell(half, transparentCell(), PasteMode::Under) == half);
}

TEST_CASE("a pasted half pixel keeps the pixel under its clear half", "[layers][selection][pixels]") {
    const Cell top = setLayerPixel(transparentCell(), false, Red);
    const Cell bottom = setLayerPixel(transparentCell(), true, Yellow);
    const Cell blue = pixelLayerCell({Blue, Blue});
    CHECK(layerPixels(*pasteCell(top, blue, PasteMode::Normal)) == LayerPixels{Red, Blue});
    CHECK(layerPixels(*pasteCell(bottom, blue, PasteMode::Normal)) == LayerPixels{Blue, Yellow});
    // over another half pixel the two make a whole cell
    CHECK(layerPixels(*pasteCell(top, bottom, PasteMode::Normal)) == LayerPixels{Red, Yellow});
    CHECK(layerPixels(*pasteCell(top, top, PasteMode::Normal)) == LayerPixels{Red, std::nullopt});
    // a blank cell under it is pixels too (black ones): Under writes into it
    const auto under = pasteCell(top, Cell{}, PasteMode::Under);
    REQUIRE(under);
    CHECK_FALSE(under->transparent);
    CHECK(layerPixels(*under) == LayerPixels{Red, Black});
    // a whole pixel cell replaces the cell under it, as any opaque cell does
    CHECK(pasteCell(pixelLayerCell({Red, Red}), blue, PasteMode::Normal) == pixelLayerCell({Red, Red}));
}

TEST_CASE("an opaque pixel cell pasted onto a cells layer is written as it shows, under the iCE setting",
          "[layers][selection][pixels]") {
    // light red over yellow, as a pixel layer stores it: ▀ fg 12 bg 14
    const Cell both = pixelLayerCell({LightRed, Yellow});
    REQUIRE(both.bg.paletteIndex() == Yellow);
    const Cell letter = glyphCell(U'a');
    const PastePixels off{false, false}, on{false, true};
    // iCE off: as the compositor shows it, the bottom dark, so no blink
    const auto dark = pasteCell(both, letter, PasteMode::Normal, off, true);
    REQUIRE(dark);
    CHECK(*dark == halfBlockCell({LightRed, Yellow}, false));
    CHECK(dark->bg.paletteIndex() < 8);
    CHECK_FALSE(iceCell(*dark, false).attrs & attr::Blink);
    // over a hole (a clear cell of an upper layer) it is the same whole cell
    CHECK(pasteCell(both, transparentCell(), PasteMode::Normal, off, true) == dark);
    // iCE on, or onto a pixel layer: as it is stored
    CHECK(pasteCell(both, letter, PasteMode::Normal, on, true) == both);
    CHECK(pasteCell(both, letter, PasteMode::Normal, PastePixels{true, false}, true) == both);
    // a cell not from a pixel layer is written as it is
    CHECK(pasteCell(both, letter, PasteMode::Normal, off) == both);
    // turned a quarter it holds no pixels: the glyph stays, the background goes dark
    const Cell turned{U'▐', Color::indexed(LightRed), Color::indexed(Yellow), 0, false};
    const auto t = pasteCell(turned, letter, PasteMode::Normal, off, true);
    REQUIRE(t);
    CHECK(t->ch == U'▐');
    CHECK(t->fg.paletteIndex() == LightRed);
    CHECK(t->bg.paletteIndex() == Brown);
    // a block keeps the mark through flips and turns
    CellBlock b(1, 1);
    b.at(0, 0) = both;
    b.pixelLayer = true;
    CHECK(transformed(b, Transform::FlipVertical).pixelLayer);
    CHECK(transformed(b, Transform::RotateClockwise).pixelLayer);
}

TEST_CASE("a pasted half pixel is plotted as the half-block tools plot it, under the iCE setting",
          "[layers][selection][pixels]") {
    const Cell top = setLayerPixel(transparentCell(), false, LightRed);
    // a yellow bottom pixel on a cells layer: ▄ yellow on black
    const Cell yellow = halfBlockCell({Black, Yellow}, false);
    // iCE off: the bright red would need a bright background, so it goes dark
    const PastePixels off{false, false};
    const auto dark = pasteCell(top, yellow, PasteMode::Normal, off);
    REQUIRE(dark);
    CHECK(*dark == setHalfBlock(yellow, false, LightRed, false));
    CHECK(dark->bg.paletteIndex() < 8);
    CHECK(halfBlocks(*dark, false) == HalfPair{Red, Yellow});
    // iCE on: both bright colours stay
    const auto bright = pasteCell(top, yellow, PasteMode::Normal, PastePixels{false, true});
    REQUIRE(bright);
    CHECK(halfBlocks(*bright, true) == HalfPair{LightRed, Yellow});
    // a blinking letter on blue shows light blue with iCE on: that is the clear half
    const Cell letter{U'A', Color::indexed(White), Color::indexed(Blue), attr::Blink, false};
    const Cell red = setLayerPixel(transparentCell(), false, Red);
    const auto over = pasteCell(red, letter, PasteMode::Normal, PastePixels{false, true});
    REQUIRE(over);
    CHECK(halfBlocks(iceCell(*over, true), true) == HalfPair{Red, LightBlue});
    // a pixel layer stores its pixels with iCE on, whatever the document's setting
    CHECK(pasteCell(top, yellow, PasteMode::Normal, PastePixels{true, false}) == setLayerPixel(yellow, false, LightRed));
    // over a hole it stays the half pixel, on either kind of layer
    CHECK(pasteCell(top, transparentCell(), PasteMode::Normal, off) == top);
    CHECK(pasteCell(top, transparentCell(), PasteMode::Normal, PastePixels{true, false}) == top);

    // on a clear cell of a cells layer it goes over what shows below the same way
    Layer l = makeLayer(LayerKind::Cells, 1, 1);
    const auto below = [&](int, int) { return yellow; };
    const auto fit = fitLayerEdit(l, CellEdit{{{0, 0, Cell{}, top}}}, below, false);
    REQUIRE(fit);
    REQUIRE(fit->changes.size() == 1);
    CHECK(fit->changes[0].after == setHalfBlock(yellow, false, LightRed, false));
    CHECK_FALSE(iceCell(fit->changes[0].after, false).attrs & attr::Blink);
}

TEST_CASE("under the alpha lock a move that would clear a cell is refused whole", "[layers][selection]") {
    // b c . moved one right: b lifted (cleared), b onto c, c onto the clear cell
    Layer l = makeLayer(LayerKind::Cells, 3, 1);
    l.cells.at(0, 0) = glyphCell(U'b');
    l.cells.at(1, 0) = glyphCell(U'c');
    l.props.alphaLocked = true;
    CellEdit move{{{0, 0, Cell{}, transparentCell()}, {1, 0, Cell{}, glyphCell(U'b')}, {2, 0, Cell{}, glyphCell(U'c')}}};
    move.moves = true;
    CHECK_FALSE(fitLayerEdit(l, move));
    // the same changes not as a move: each kept or dropped on its own
    move.moves = false;
    const auto apart = fitLayerEdit(l, move);
    REQUIRE(apart);
    REQUIRE(apart->changes.size() == 1);
    CHECK(apart->changes[0].after == glyphCell(U'b'));
    // a paste (a move that clears nothing) lands on the opaque cells
    CellEdit paste{{{1, 0, Cell{}, glyphCell(U'x')}, {2, 0, Cell{}, glyphCell(U'y')}}};
    paste.moves = true;
    const auto pasted = fitLayerEdit(l, paste);
    REQUIRE(pasted);
    REQUIRE(pasted->changes.size() == 1);
    CHECK(pasted->changes[0].x == 1);
    // unlocked the move goes through
    l.props.alphaLocked = false;
    move.moves = true;
    const auto free = fitLayerEdit(l, move);
    REQUIRE(free);
    CHECK(free->changes.size() == 3);

    // a pixel layer: a move that clears a set pixel
    Layer px = makeLayer(LayerKind::Pixels, 2, 1);
    px.cells.at(0, 0) = pixelLayerCell({Blue, Blue});
    px.cells.at(1, 0) = pixelLayerCell({Red, std::nullopt});
    px.props.alphaLocked = true;
    CellEdit shift{{{0, 0, Cell{}, pixelLayerCell({std::nullopt, Blue})}, {1, 0, Cell{}, pixelLayerCell({Blue, std::nullopt})}}};
    shift.moves = true;
    CHECK_FALSE(fitLayerEdit(px, shift));
}

TEST_CASE("the alpha lock keeps an erase off opaque cells and set pixels", "[layers][pixels]") {
    // cells: an opaque cell stays opaque under the lock
    Canvas cells(2, 1);
    cells.at(0, 0) = glyphCell(U'b');
    cells.at(1, 0) = transparentCell();
    StrokeOptions locked;
    locked.lockTransparent = true;
    const GlyphBrush erase(transparentCell());
    CHECK(paintStroke(cells, erase, {}, {{0, 0}, {1, 0}}, locked).empty());
    const CellEdit free = paintStroke(cells, erase, {}, {{0, 0}, {1, 0}});
    REQUIRE(free.changes.size() == 1);
    CHECK(free.changes[0].after == transparentCell());

    // pixels: set pixels stay set, whichever brush would clear them
    Canvas px(1, 1);
    px.at(0, 0) = pixelLayerCell({Blue, Blue});
    StrokeOptions pixelsLocked = locked;
    pixelsLocked.pixelLayer = true;
    CHECK(paintStroke(px, erase, {}, {{0, 0}}, pixelsLocked).empty());
    CHECK(paintStroke(px, GlyphBrush(Cell{}), {}, {{0, 0}}, pixelsLocked).changes.size() == 1);  // repainted, still set
    // and a write that would set a clear pixel is refused too
    px.at(0, 0) = pixelLayerCell({Blue, std::nullopt});
    Cell full;
    full.ch = kFullBlock;
    full.fg = Color::indexed(Red);
    CHECK(paintStroke(px, GlyphBrush(full), {}, {{0, 0}}, pixelsLocked).empty());
}
