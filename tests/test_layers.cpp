// Layer model: kinds, properties, the stack, pixel layer cells and edits
// (PLAN §5).

#include "blockwright/layer.h"
#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"

#include <catch2/catch_test_macros.hpp>

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
    // the stroke options leave pixel layers to the per-pixel check, so a
    // painter reading them doesn't refuse a half-set cell's set half
    CHECK_FALSE(layerStrokeOptions(px).lockTransparent);
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
