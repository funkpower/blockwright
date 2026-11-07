// Canvas Size in the core (PLAN §5, #397): the shift each anchor gives, what
// happens to each kind of layer, the blank cells, wide pairs cut at the new
// edges, and the limits.

#include "blockwright/ansi.h"
#include "blockwright/canvas_size.h"
#include "blockwright/composite.h"
#include "blockwright/project.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace bw;

namespace {

constexpr Anchor kAnchors[] = {Anchor::TopLeft,    Anchor::Top,    Anchor::TopRight,
                               Anchor::Left,       Anchor::Centre, Anchor::Right,
                               Anchor::BottomLeft, Anchor::Bottom, Anchor::BottomRight};

// A cell nothing else on these canvases holds: its letter is its place.
Cell mark(int x, int y) {
    Cell c;
    c.ch = char32_t(U'A' + (x + 7 * y) % 26);
    c.fg = Color::indexed(std::uint8_t(1 + (x + y) % 15));
    c.bg = Color::indexed(std::uint8_t((x * 3 + y) % 8));
    return c;
}

Layer marked(LayerKind kind, int w, int h, std::string name, int x = 0, int y = 0) {
    Layer l = makeLayer(kind, w, h, std::move(name), x, y);
    for (int cy = 0; cy < h; ++cy)
        for (int cx = 0; cx < w; ++cx)
            l.cells.at(cx, cy) = mark(cx, cy);
    return l;
}

// A canvas-sized marked background on a w by h stack.
LayerStack background(int w, int h) {
    LayerStack s(w, h);
    s.add(marked(LayerKind::Cells, w, h, "Background"));
    return s;
}

// Every cell of `next` at (x, y) is `old`'s at (x - dx, y - dy) where that is
// on `old`, and `blank` where it isn't.
void checkMoved(const Canvas& old, const Canvas& next, CellShift s, const Cell& blank) {
    for (int y = 0; y < next.height(); ++y)
        for (int x = 0; x < next.width(); ++x) {
            const int sx = x - s.dx, sy = y - s.dy;
            INFO("cell " << x << "," << y);
            if (old.contains(sx, sy))
                CHECK(next.at(x, y) == old.at(sx, sy));
            else
                CHECK(next.at(x, y) == blank);
        }
}

} // namespace

TEST_CASE("each anchor puts the old canvas at its side, corner or centre") {
    // growing 10x6 to 15x9: 5 more columns, 3 more rows
    CHECK(anchorShift(10, 6, 15, 9, Anchor::TopLeft) == CellShift{0, 0});
    CHECK(anchorShift(10, 6, 15, 9, Anchor::Top) == CellShift{2, 0});
    CHECK(anchorShift(10, 6, 15, 9, Anchor::TopRight) == CellShift{5, 0});
    CHECK(anchorShift(10, 6, 15, 9, Anchor::Left) == CellShift{0, 1});
    CHECK(anchorShift(10, 6, 15, 9, Anchor::Centre) == CellShift{2, 1});
    CHECK(anchorShift(10, 6, 15, 9, Anchor::Right) == CellShift{5, 1});
    CHECK(anchorShift(10, 6, 15, 9, Anchor::BottomLeft) == CellShift{0, 3});
    CHECK(anchorShift(10, 6, 15, 9, Anchor::Bottom) == CellShift{2, 3});
    CHECK(anchorShift(10, 6, 15, 9, Anchor::BottomRight) == CellShift{5, 3});
    // shrinking 15x9 to 10x6: the odd column still goes from the right, the
    // odd row from the bottom
    CHECK(anchorShift(15, 9, 10, 6, Anchor::TopLeft) == CellShift{0, 0});
    CHECK(anchorShift(15, 9, 10, 6, Anchor::Centre) == CellShift{-2, -1});
    CHECK(anchorShift(15, 9, 10, 6, Anchor::BottomRight) == CellShift{-5, -3});
    CHECK(anchorShift(15, 9, 10, 6, Anchor::Bottom) == CellShift{-2, -3});
    // the same size never moves
    for (Anchor a : kAnchors)
        CHECK(anchorShift(80, 25, 80, 25, a) == CellShift{});
}

TEST_CASE("anchor names go both ways") {
    for (Anchor a : kAnchors)
        CHECK(anchorFromName(anchorName(a)) == a);
    CHECK(anchorName(Anchor::Centre) == "centre");
    CHECK(anchorName(Anchor::BottomRight) == "bottom-right");
    CHECK_FALSE(anchorFromName("center"));
    CHECK_FALSE(anchorFromName(""));
}

TEST_CASE("the background follows the canvas at every anchor, growing and shrinking") {
    const LayerStack old = background(10, 6);
    for (Anchor a : kAnchors)
        for (auto [w, h] : {std::pair{15, 9}, std::pair{7, 4}, std::pair{13, 3}, std::pair{4, 8}}) {
            INFO("anchor " << anchorName(a) << " to " << w << "x" << h);
            const CellShift s = anchorShift(10, 6, w, h, a);
            const LayerStack next = resizeCanvas(old, w, h, s, 8);
            REQUIRE(next.width() == w);
            REQUIRE(next.height() == h);
            REQUIRE(next.size() == 1);
            const Layer& l = next[0];
            CHECK(l.bounds() == CellRect{0, 0, w, h});
            CHECK(l.props.id == old[0].props.id);
            CHECK(l.props.name == "Background");
            // the bottom cells layer gains the blank the document gives
            // there: a space, grey on black, opaque
            checkMoved(old[0].cells, l.cells, s, Cell{});
            // and the canvas shows the same
            CHECK(composite(next) == l.cells);
        }
}

TEST_CASE("the blank cell is Cell{} on the bottom cells layer and transparent above") {
    CHECK(Cell{}.ch == U' ');
    CHECK(Cell{}.fg == Color::indexed(Grey));
    CHECK(Cell{}.bg == Color::indexed(Black));
    CHECK_FALSE(Cell{}.transparent);

    LayerStack old = background(6, 4);
    old.add(marked(LayerKind::Cells, 6, 4, "Ink"));
    old.add(marked(LayerKind::Pixels, 6, 4, "Pixels"));
    const CellShift s = anchorShift(6, 4, 9, 6, Anchor::Centre);
    const LayerStack next = resizeCanvas(old, 9, 6, s, 8);
    checkMoved(old[0].cells, next[0].cells, s, Cell{});
    checkMoved(old[1].cells, next[1].cells, s, transparentCell());
    checkMoved(old[2].cells, next[2].cells, s, transparentCell());

    // a pixels layer at the bottom isn't a cells layer: it gains transparent cells
    LayerStack pixelsBottom(6, 4);
    pixelsBottom.add(marked(LayerKind::Pixels, 6, 4, "Pixels"));
    const LayerStack grown = resizeCanvas(pixelsBottom, 8, 4, {}, 8);
    checkMoved(pixelsBottom[0].cells, grown[0].cells, {}, transparentCell());
}

TEST_CASE("text, generator, smaller, moved and larger layers keep all their cells and move") {
    LayerStack old = background(10, 6);
    const Layer small = marked(LayerKind::Cells, 3, 2, "Small", 4, 2);
    const Layer moved = marked(LayerKind::Cells, 10, 6, "Moved", 1, 0);
    const Layer large = marked(LayerKind::Pixels, 14, 9, "Large", -2, -1);
    const Layer text = marked(LayerKind::Text, 5, 1, "Text", 2, 5);
    const Layer generator = marked(LayerKind::Generator, 10, 6, "Generator");
    for (const Layer& l : {small, moved, large, text, generator})
        old.add(l);

    for (Anchor a : kAnchors)
        for (auto [w, h] : {std::pair{16, 8}, std::pair{4, 3}}) {
            INFO("anchor " << anchorName(a) << " to " << w << "x" << h);
            const CellShift s = anchorShift(10, 6, w, h, a);
            const LayerStack next = resizeCanvas(old, w, h, s, 8);
            REQUIRE(next.size() == old.size());
            CHECK(next.nextId() == old.nextId());
            for (std::size_t i = 1; i < old.size(); ++i) {
                INFO("layer " << old[i].props.name);
                Layer expect = old[i];
                expect.props.x += s.dx;
                expect.props.y += s.dy;
                CHECK(next[i] == expect);
            }
        }
}

TEST_CASE("a reference picture moves in canvas pixels, a cell 8 or 9 by 16, clamped") {
    LayerStack old = background(10, 6);
    Layer ref = makeLayer(LayerKind::Reference, 0, 0, "Sketch");
    ref.props.reference = ReferenceImage{std::make_shared<const std::string>("not a real png"), 5, -7, 300, 200, 40};
    old.add(ref);

    const CellShift s = anchorShift(10, 6, 20, 10, Anchor::BottomRight);  // {10, 4}
    for (int cellWidth : {8, 9}) {
        const LayerStack next = resizeCanvas(old, 20, 10, s, cellWidth);
        const Layer& r = next[1];
        CHECK(r.props.x == 0);
        CHECK(r.props.y == 0);
        CHECK(r.bounds() == CellRect{0, 0, 0, 0});
        REQUIRE(r.props.reference);
        CHECK(r.props.reference->x == 5 + 10 * cellWidth);
        CHECK(r.props.reference->y == -7 + 4 * 16);
        CHECK(r.props.reference->width == 300);
        CHECK(r.props.reference->height == 200);
        CHECK(r.props.reference->opacity == 40);
        CHECK(*r.props.reference->bytes == "not a real png");
    }

    // a picture already near the limit stops at it
    old[1].props.reference->x = ReferenceImage::kMaxPlace - 3;
    old[1].props.reference->y = -ReferenceImage::kMaxPlace + 3;
    const LayerStack far = resizeCanvas(old, 20, 10, {10, -4}, 8);
    CHECK(far[1].props.reference->x == ReferenceImage::kMaxPlace);
    CHECK(far[1].props.reference->y == -ReferenceImage::kMaxPlace);
}

TEST_CASE("an empty reference layer and other properties come through untouched") {
    LayerStack old = background(4, 3);
    old.add(makeLayer(LayerKind::Reference, 0, 0, "Empty"));
    old[0].props.locked = true;
    old[0].props.visible = false;
    old[0].props.mode = LayerMode::CharsOnly;
    const LayerStack next = resizeCanvas(old, 6, 3, {1, 0}, 8);
    CHECK(next[1] == old[1]);
    CHECK(next[0].props.locked);
    CHECK_FALSE(next[0].props.visible);
    CHECK(next[0].props.mode == LayerMode::CharsOnly);
}

TEST_CASE("a wide pair cut by the new left or right edge keeps its half as a space") {
    // row 0: a pair at columns 1-2; row 1: a pair at 3-4
    LayerStack old = background(6, 2);
    Canvas& c = old[0].cells;
    c.at(1, 0).ch = U'世';
    c.at(2, 0).ch = Glyph::continuation();
    c.at(3, 1).ch = U'界';
    c.at(4, 1).ch = Glyph::continuation();

    SECTION("the left edge cuts through the first pair") {
        // shrink by 2 from the left: column 2 becomes column 0
        const LayerStack next = resizeCanvas(old, 4, 2, {-2, 0}, 8);
        const Cell& kept = next[0].cells.at(0, 0);
        CHECK(kept.ch == U' ');
        CHECK(kept.fg == c.at(2, 0).fg);
        CHECK(kept.bg == c.at(2, 0).bg);
        // the second pair is whole on the canvas
        CHECK(next[0].cells.at(1, 1).ch == U'界');
        CHECK(next[0].cells.at(2, 1).ch.isContinuation());
    }
    SECTION("the right edge cuts through the second pair") {
        const LayerStack next = resizeCanvas(old, 4, 2, {}, 8);
        const Cell& kept = next[0].cells.at(3, 1);
        CHECK(kept.ch == U' ');
        CHECK(kept.fg == c.at(3, 1).fg);
        CHECK(kept.bg == c.at(3, 1).bg);
        CHECK(next[0].cells.at(1, 0).ch == U'世');
        CHECK(next[0].cells.at(2, 0).ch.isContinuation());
    }
    SECTION("an edge between pairs leaves them whole") {
        const LayerStack next = resizeCanvas(old, 5, 2, {}, 8);
        CHECK(next[0].cells.at(3, 1).ch == U'界');
        CHECK(next[0].cells.at(4, 1).ch.isContinuation());
    }
    SECTION("growing leaves every pair whole") {
        const LayerStack next = resizeCanvas(old, 9, 2, {2, 0}, 8);
        CHECK(next[0].cells.at(3, 0).ch == U'世');
        CHECK(next[0].cells.at(4, 0).ch.isContinuation());
        CHECK(next[0].cells.at(5, 1).ch == U'界');
        CHECK(next[0].cells.at(6, 1).ch.isContinuation());
    }
    SECTION("a layer that only moves keeps its pairs as they are") {
        LayerStack two = old;
        Layer ink = makeLayer(LayerKind::Cells, 2, 1, "Ink", 4, 0);
        ink.cells.at(0, 0).ch = U'字';
        ink.cells.at(1, 0).ch = Glyph::continuation();
        two.add(ink);
        const LayerStack next = resizeCanvas(two, 5, 2, {}, 8);
        CHECK(next[1] == two[1]);
    }
}

TEST_CASE("growing then shrinking back gives the stack back, at every anchor") {
    LayerStack old = background(9, 5);
    old.add(marked(LayerKind::Cells, 3, 3, "Small", 2, 1));
    Layer ref = makeLayer(LayerKind::Reference, 0, 0, "Sketch");
    ref.props.reference = ReferenceImage{std::make_shared<const std::string>("x"), 0, 0, 10, 10, 50};
    old.add(ref);
    for (Anchor a : kAnchors) {
        INFO("anchor " << anchorName(a));
        const LayerStack grown = resizeCanvas(old, 14, 8, anchorShift(9, 5, 14, 8, a), 9);
        const LayerStack back = resizeCanvas(grown, 9, 5, anchorShift(14, 8, 9, 5, a), 9);
        CHECK(back == old);
    }
}

TEST_CASE("Canvas Size holds the canvas to File > New's limits and the project's cells") {
    const LayerStack old = background(10, 6);
    CHECK_FALSE(canvasSizeProblem(old, 1, 1));
    CHECK_FALSE(canvasSizeProblem(old, kMaxColumns, kMaxRows));
    for (auto [w, h] : {std::pair{0, 6}, std::pair{10, 0}, std::pair{-1, 6}, std::pair{kMaxColumns + 1, 6},
                        std::pair{10, kMaxRows + 1}}) {
        INFO(w << "x" << h);
        const auto problem = canvasSizeProblem(old, w, h);
        REQUIRE(problem);
        CHECK(*problem == "a canvas is 1 to 400 columns wide and 1 to 5000 rows high");
    }

    // followers count at the new size, the rest at their own
    LayerStack many(10, 10);
    const std::uint64_t perFollower = std::uint64_t(kMaxColumns) * kMaxRows;  // 2,000,000
    const std::size_t followers = std::size_t(project::kMaxTotalCells / perFollower);
    for (std::size_t i = 0; i < followers; ++i)
        many.add(makeLayer(LayerKind::Cells, 10, 10));
    CHECK_FALSE(canvasSizeProblem(many, kMaxColumns, kMaxRows));
    many.add(makeLayer(LayerKind::Cells, 1, 1, "speck", 3, 3));
    const auto problem = canvasSizeProblem(many, kMaxColumns, kMaxRows);
    REQUIRE(problem);
    CHECK(*problem == "the layers would hold over " + std::to_string(project::kMaxTotalCells) + " cells");
    CHECK_FALSE(canvasSizeProblem(many, 10, 10));
}

TEST_CASE("a real piece grows and shrinks around its centre and comes back") {
    const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden/darkmage.ans"));
    const LayerStack old = LayerStack::fromCanvas(doc.canvas);
    const int w = old.width(), h = old.height();
    REQUIRE(w > 4);
    REQUIRE(h > 4);

    const CellShift out = anchorShift(w, h, w + 11, h + 7, Anchor::Centre);
    const LayerStack grown = resizeCanvas(old, w + 11, h + 7, out, 8);
    const Canvas shown = composite(grown);
    checkMoved(doc.canvas, shown, out, Cell{});
    CHECK(resizeCanvas(grown, w, h, anchorShift(w + 11, h + 7, w, h, Anchor::Centre), 8) == old);

    // cut 3 columns from the right and 2 rows from the bottom: the top-left
    // keeps its place, so every kept cell is where it was (the last column
    // is left out, as a pair the edge cuts becomes a space there)
    const LayerStack cut = resizeCanvas(old, w - 3, h - 2, anchorShift(w, h, w - 3, h - 2, Anchor::TopLeft), 8);
    const Canvas cutShown = composite(cut);
    for (int y = 0; y < h - 2; ++y)
        for (int x = 0; x < w - 4; ++x)
            CHECK(cutShown.at(x, y) == doc.canvas.at(x, y));
}
