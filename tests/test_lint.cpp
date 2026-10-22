// The speckle and ladder checks, and the golden sphere: a ball painted with
// the ramp shade brush, dab by dab, that has to pass them.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/lint.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <vector>

using namespace bw;
using Kind = lint::LadderFinding::Kind;

namespace {

Ramp steel() { return *findRamp("steel"); }  // k K w W

Cell cell(char32_t g, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = g;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

Canvas filled(int w, int h, const Cell& c) {
    Canvas canvas(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            canvas.at(x, y) = c;
    return canvas;
}

// Six rows of these rungs, the same every row.
Canvas rows(const Ramp& r, const std::vector<int>& rungs) {
    Canvas c(int(rungs.size()), 6);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < c.width(); ++x)
            c.at(x, y) = rungCell(r, rungs[static_cast<size_t>(x)]);
    return c;
}

int count(const lint::LadderReport& r, Kind k) {
    int n = 0;
    for (const auto& f : r.findings)
        n += f.kind == k;
    return n;
}

// A ball lit from the upper left, painted by hand: twelve lightening passes
// with a round brush, one rung each from black to white. Each dab sits one
// cell inside the last all round (four columns narrower: two each side, one
// row top and bottom), so every ring is a rung and no rung is skipped. The
// light moves the dab up and left only on the passes that end on a solid
// colour (DarkGrey, Grey): those dabs are six columns narrower again, and the
// solid planes they leave grow broad on the shadow side while the shade
// rungs stay one or two cells wide ("broad lit side, ramp
// crowded at the turn").
Canvas paintSphere() {
    Canvas c = filled(72, 34, cell(U' ', Black, Black));
    const RampShadeBrush up(steel(), ShadeDirection::Lighten);
    int x = 36, y = 17, size = 65;
    for (int i = 0; i < 12; ++i) {
        if (i > 0) {
            size -= 4;
            if (i % kRungsPerStep == 0)
                x -= 3, y -= 1, size -= 6;
        }
        BrushSettings s;
        s.tip = Tip::Round;
        s.size = size;
        paintStroke(c, up, s, {{x, y}}).apply(c);
    }
    return c;
}

// Neighbour pairs (across and down) whose rungs on the ramp are more than
// one apart.
int rungSkips(const Canvas& c, const Ramp& r) {
    int n = 0;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x) {
            const auto g = rungOf(r, c.at(x, y));
            if (!g)
                continue;
            if (x + 1 < c.width())
                if (const auto h = rungOf(r, c.at(x + 1, y)); h && std::abs(*h - *g) > 1)
                    ++n;
            if (y + 1 < c.height())
                if (const auto h = rungOf(r, c.at(x, y + 1)); h && std::abs(*h - *g) > 1)
                    ++n;
        }
    return n;
}

} // namespace

TEST_CASE("pixels reads cells two pixels each", "[lint]") {
    Canvas c(4, 1);
    c.at(0, 0) = cell(kFullBlock, White, Black);
    c.at(1, 0) = cell(kUpperHalf, Red, Blue);
    c.at(2, 0) = cell(kLowerHalf, Red, Blue);
    c.at(3, 0) = cell(kShadeLight, Red, Blue);
    const auto g = lint::pixels(c);
    REQUIRE(g.size() == 2);
    CHECK(g[0][0] == White);
    CHECK(g[1][0] == White);
    CHECK(g[0][1] == Red);
    CHECK(g[1][1] == Blue);
    CHECK(g[0][2] == Blue);
    CHECK(g[1][2] == Red);
    CHECK_FALSE(g[0][3]);
    CHECK(lint::pixels(c, lint::Box{1, 0, 2, 0})[0].size() == 2);
    CHECK(lint::pixels(c, lint::Box{9, 9, 12, 12}).empty());
}

TEST_CASE("noise is the share of lonely pixels", "[lint]") {
    Canvas c = filled(9, 5, cell(U' ', Black, Grey));
    CHECK(lint::noise(c) == 0.0);
    // One lone white pixel (a ▀ top) among grey: interior pixels are rows
    // 1-8 by columns 1-7, 56 of them.
    c.at(4, 2) = cell(kUpperHalf, White, Grey);
    CHECK(lint::noise(c) == 1.0 / 56);
    // Black pixels and pixels next to a shade cell don't count.
    c.at(4, 2) = cell(kUpperHalf, Black, Grey);
    CHECK(lint::noise(c) == 0.0);
    c.at(4, 2) = cell(kUpperHalf, White, Grey);
    c.at(5, 2) = cell(kShadeMedium, White, Grey);
    CHECK(lint::noise(c) == 0.0);
    // The edge of the box is skipped, as at the edge of the canvas.
    c.at(5, 2) = cell(U' ', Black, Grey);
    CHECK(lint::noise(c, lint::Box{4, 2, 6, 4}) == 0.0);
}

TEST_CASE("noise trims each row's trailing black spaces, as in a written file", "[lint]") {
    // A grey column at x=1 on black: the file has rows two cells long, so
    // the column's pixels have no right neighbour and aren't counted.
    Canvas c(80, 3);
    for (int y = 0; y < 3; ++y)
        c.at(1, y) = Cell{kUpperHalf, Color::indexed(Grey), Color::indexed(Black)};
    CHECK(lint::noise(c) == 0.0);
    // A space on another colour isn't trimmed, nor is anything before it.
    c.at(2, 1) = cell(U' ', Black, Blue);
    CHECK(lint::noise(c) == 1.0);
    // Trimmed per row of the canvas, then cut to the box.
    CHECK(lint::noise(c, lint::Box{0, 0, 79, 2}) == 1.0);
    CHECK(lint::noise(c, lint::Box{0, 0, 1, 2}) == 0.0);
}

TEST_CASE("the ladder check finds what spoils shading", "[lint][ladder]") {
    const Ramp r = steel();
    Canvas c = filled(12, 6, cell(U' ', Black, Grey));

    SECTION("a flat area is clean") {
        const auto rep = lint::checkLadder(c, r);
        CHECK(rep.clean());
        CHECK(rep.cells == 72);
        CHECK(rep.ladderCells == 72);
        CHECK(rep.shadeCells == 0);
    }
    SECTION("a shade of non-neighbours") {
        c.at(3, 2) = cell(kShadeMedium, White, DarkGrey);
        c.at(4, 2) = cell(kShadeMedium, Grey, Black);
        const auto rep = lint::checkLadder(c, r);
        CHECK(count(rep, Kind::NonNeighbourShade) == 2);
        CHECK(rep.findings.front().x == 3);
        CHECK(rep.findings.front().y == 2);
    }
    SECTION("a wide rung") {
        // "each rung is 1-3 cells wide along the gradient"
        CHECK(lint::LadderOptions{}.maxRung == 3);
        // A crowded ladder across each row, K w░K w▒K w▓K w, with the w▒K
        // step 6 cells wide: one wide rung a row.
        auto ladder = [&](int stepWidth, int rows) {
            c = filled(12, 6, cell(U' ', Black, Grey));
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < 12; ++x)
                    c.at(x, y) = rungCell(r, x < 2 ? 4 : x < 3 ? 5 : x < 3 + stepWidth ? 6 : x < 4 + stepWidth ? 7 : 8);
        };
        ladder(6, 6);
        auto rep = lint::checkLadder(c, r);
        CHECK(count(rep, Kind::WideRung) == 6);
        CHECK(rep.findings.front().x == 3);
        // 3 cells is within the rule
        ladder(3, 6);
        CHECK(count(lint::checkLadder(c, r), Kind::WideRung) == 0);
        // the same step two rows high lies along a contour: 2 cells wide
        ladder(6, 2);
        CHECK(count(lint::checkLadder(c, r), Kind::WideRung) == 0);
        // and within maxRung either way
        lint::LadderOptions tight;
        tight.maxRung = 1;
        CHECK(count(lint::checkLadder(c, r, {}, tight), Kind::WideRung) == 2);
        // Down the columns the same: a 4-row step of w▓K between w▒K and w.
        c = filled(6, 8, cell(U' ', Black, Grey));
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 6; ++x)
                c.at(x, y) = rungCell(r, y < 1 ? 5 : y < 2 ? 6 : y < 6 ? 7 : 8);
        rep = lint::checkLadder(c, r);
        CHECK(count(rep, Kind::WideRung) == 6);
        REQUIRE_FALSE(rep.findings.empty());
        CHECK(rep.findings.front().y == 2);
    }
    SECTION("a flat field or a plane at a gradient's end isn't a wide rung") {
        // a field of one shade is no step of a gradient
        c = filled(12, 6, cell(kShadeMedium, Grey, DarkGrey));
        CHECK(count(lint::checkLadder(c, r), Kind::WideRung) == 0);
        // nor a block of shade in a solid field: lighter on both sides
        c = filled(12, 6, cell(U' ', Black, Grey));
        for (int y = 1; y <= 4; ++y)
            for (int x = 3; x <= 8; ++x)
                c.at(x, y) = cell(kShadeMedium, Grey, DarkGrey);
        CHECK(count(lint::checkLadder(c, r), Kind::WideRung) == 0);
        // nor a broad shade plane at the end of a ladder
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 12; ++x)
                c.at(x, y) = rungCell(r, x < 1 ? 4 : x < 2 ? 5 : 6);
        CHECK(count(lint::checkLadder(c, r), Kind::WideRung) == 0);
        // and black is ink or the ground, not a lower rung: a K░k plane
        // running into black is the plane at the end, as at the canvas edge
        CHECK(count(lint::checkLadder(rows(r, {3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}), r), Kind::WideRung) == 0);
        CHECK(count(lint::checkLadder(rows(r, {4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1}), r), Kind::WideRung) == 0);
        // a wide step between two shades still counts beside black
        CHECK(count(lint::checkLadder(rows(r, {0, 3, 2, 2, 2, 2, 2, 1, 0}), r), Kind::WideRung) == 6);
    }
    SECTION("a speck") {
        // a lone W▒w cell in grey: lighter than every neighbour
        c.at(5, 3) = rungCell(r, 10);
        auto rep = lint::checkLadder(c, r);
        REQUIRE(count(rep, Kind::Speckle) == 1);
        CHECK(rep.findings.front().x == 5);
        CHECK(rep.findings.front().y == 3);
        // one rung off its neighbours is still a speck
        c.at(5, 3) = cell(kShadeLight, White, Grey);
        CHECK(count(lint::checkLadder(c, r), Kind::Speckle) == 1);
        // two cells together are a cluster, not a speck
        c.at(6, 3) = cell(kShadeLight, White, Grey);
        CHECK(count(lint::checkLadder(c, r), Kind::Speckle) == 0);
        // a speck on the canvas edge has three neighbours and is judged too
        c.at(0, 2) = cell(kShadeLight, White, Grey);
        CHECK(count(lint::checkLadder(c, r), Kind::Speckle) == 1);
        // one in the corner has two and isn't, nor is black there (ink)
        c.at(0, 2) = cell(U' ', Black, Grey);
        c.at(11, 0) = cell(kShadeLight, White, Grey);
        c.at(0, 0) = cell(U' ', Black, Black);
        CHECK(count(lint::checkLadder(c, r), Kind::Speckle) == 0);
    }
    SECTION("ink, glints and the ends of a gradient aren't specks") {
        // a speck is "a lone non-black, non-glint pixel"
        auto specks = [&] { return count(lint::checkLadder(c, r), Kind::Speckle); };
        // a clean diagonal gradient: its corners have two neighbours
        c = Canvas(3, 3);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                c.at(x, y) = rungCell(r, 5 + x + y);
        CHECK(specks() == 0);
        c = Canvas(8, 6);
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 8; ++x)
                c.at(x, y) = rungCell(r, x + y);
        CHECK(specks() == 0);
        // a black ink line, 8-connected, across grey
        c = filled(12, 6, cell(U' ', Black, Grey));
        for (int k = 0; k < 6; ++k)
            c.at(3 + k, k) = cell(U' ', Black, Black);
        CHECK(specks() == 0);
        // a black rivet or an eye
        c = filled(12, 6, cell(U' ', Black, Grey));
        c.at(5, 3) = cell(U' ', Black, Black);
        CHECK(specks() == 0);
        // the one-cell specular peak of a ball (White, the ramp's top rung)
        c = Canvas(25, 13);
        for (int y = 0; y < 13; ++y)
            for (int x = 0; x < 25; ++x)
                c.at(x, y) = rungCell(r, std::max(0, 12 - std::abs(x - 12) - 2 * std::abs(y - 6)));
        CHECK(specks() == 0);
        // but a one-cell-wide light diagonal in a flat field is all specks
        // (nature.md: one-pixel diagonals break into specks); the one in the
        // corner has two neighbours
        c = filled(12, 6, cell(U' ', Black, Grey));
        for (int k = 0; k < 6; ++k)
            c.at(k, k) = rungCell(r, 10);
        CHECK(specks() == 5);
    }
    SECTION("a lone shade cell in a flat field is a speck") {
        c = filled(12, 6, cell(U' ', Black, Black));
        c.at(5, 2) = cell(kShadeLight, DarkGrey, Black);
        const auto rep = lint::checkLadder(c, r);
        CHECK(count(rep, Kind::Speckle) == 1);
        CHECK_FALSE(rep.clean());
    }
    SECTION("a checkerboard of two shades is speckle") {
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 12; ++x)
                c.at(x, y) = cell((x + y) % 2 ? kShadeLight : kShadeMedium, DarkGrey, Black);
        // every cell but the four corners, which have two neighbours
        auto rep = lint::checkLadder(c, r);
        CHECK(count(rep, Kind::Speckle) == 68);
        CHECK_FALSE(rep.clean());
        // and the same higher up the ramp, w░K and w▒K (rungs 5 and 6)
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 12; ++x)
                c.at(x, y) = rungCell(r, 5 + (x + y) % 2);
        rep = lint::checkLadder(c, r);
        CHECK(count(rep, Kind::Speckle) == 68);
    }
    SECTION("a diagonal ladder of one-cell rungs is clean") {
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 12; ++x)
                c.at(x, y) = rungCell(r, std::clamp(x + y - 2, 0, 12));
        const auto rep = lint::checkLadder(c, r);
        for (const auto& f : rep.findings)
            UNSCOPED_INFO(f.message);
        CHECK(rep.clean());
    }
    SECTION("a skipped rung") {
        // k, then K▓k (rung 3), w░K (rung 5) and w (rung 8): the hard edges
        // A to B▓A skip rungs
        c = filled(12, 6, cell(U' ', Black, Black));
        for (int y = 0; y < 6; ++y) {
            for (int x = 4; x <= 6; ++x)
                c.at(x, y) = cell(kShadeDark, DarkGrey, Black);
            for (int x = 7; x <= 9; ++x)
                c.at(x, y) = cell(kShadeLight, Grey, DarkGrey);
            for (int x = 10; x <= 11; ++x)
                c.at(x, y) = cell(U' ', Black, Grey);
        }
        const auto rep = lint::checkLadder(c, r);
        CHECK_FALSE(rep.clean());
        // reported once per pair, at its left cell: 3 to 5 and 5 to 8 on
        // each row
        CHECK(count(rep, Kind::RungSkip) == 12);
        CHECK(rep.findings.front().x == 6);
        // a shade right next to black ink or a seam is an edge, not a skip
        for (int y = 0; y < 6; ++y)
            for (int x = 7; x <= 11; ++x)
                c.at(x, y) = cell(U' ', Black, Black);
        CHECK(count(lint::checkLadder(c, r), Kind::RungSkip) == 0);
        // and so are two solid planes side by side (a fold on a hard form)
        c = filled(12, 6, cell(U' ', Black, Grey));
        for (int y = 0; y < 6; ++y)
            for (int x = 6; x < 12; ++x)
                c.at(x, y) = cell(kFullBlock, White, Black);
        CHECK(lint::checkLadder(c, r).clean());
        c = filled(12, 6, cell(U' ', Black, DarkGrey));
        for (int y = 0; y < 6; ++y)
            for (int x = 6; x < 12; ++x)
                c.at(x, y) = cell(kFullBlock, White, Black);
        CHECK(count(lint::checkLadder(c, r), Kind::RungSkip) == 0);
        // The same bands on a K░k base: 1, 3, 5, 8 climb together, three
        // skips a row.
        c = filled(12, 6, rungCell(r, 1));
        for (int y = 0; y < 6; ++y) {
            for (int x = 4; x <= 6; ++x)
                c.at(x, y) = rungCell(r, 3);
            for (int x = 7; x <= 9; ++x)
                c.at(x, y) = rungCell(r, 5);
            for (int x = 10; x <= 11; ++x)
                c.at(x, y) = rungCell(r, 8);
        }
        CHECK(count(lint::checkLadder(c, r), Kind::RungSkip) == 18);
        // Two solids side by side at the top of a climbing gradient: solid
        // DarkGrey then solid White skips Grey, one skip a row.
        auto skips = [&](const std::vector<int>& rungs) {
            return count(lint::checkLadder(rows(r, rungs), r), Kind::RungSkip);
        };
        CHECK(skips({0, 1, 2, 3, 4, 4, 12, 12, 12, 12, 12, 0}) == 6);
        CHECK(skips({1, 2, 3, 4, 4, 12, 12, 12, 12, 12}) == 6);
        // with no third level it is still a fold
        CHECK(skips({4, 4, 4, 4, 4, 4, 12, 12, 12, 12, 12, 12}) == 0);
        // a cylinder that skips every other rung: every jump of every row
        CHECK(skips({0, 2, 4, 6, 8, 10, 12, 10, 8, 6, 4, 2, 0}) == 60);
    }
    SECTION("outlines and planes meeting aren't skipped rungs") {
        auto skips = [&] { return count(lint::checkLadder(c, r), Kind::RungSkip); };
        // a grey outline (rung 8) round bands 9 10 11: the ramp turns back
        // at the outline
        c = filled(5, 5, rungCell(r, 8));
        for (int y = 1; y <= 3; ++y)
            for (int x = 1; x <= 3; ++x)
                c.at(x, y) = rungCell(r, 8 + x);
        CHECK(skips() == 0);
        // a solid White top face on a w▒K side face: two planes
        c = filled(12, 6, rungCell(r, 6));
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 12; ++x)
                c.at(x, y) = rungCell(r, 12);
        CHECK(skips() == 0);
        // Grey against K░k
        c = filled(12, 6, rungCell(r, 1));
        for (int y = 0; y < 6; ++y)
            for (int x = 0; x < 6; ++x)
                c.at(x, y) = rungCell(r, 8);
        CHECK(skips() == 0);
        // but a third level going on the same way makes it a gradient
        for (int y = 0; y < 6; ++y)
            c.at(11, y) = rungCell(r, 0 + 3);
        for (int y = 0; y < 6; ++y)
            for (int x = 6; x < 11; ++x)
                c.at(x, y) = rungCell(r, 5);
        // 8, 5, 3 falling: two skips a row
        CHECK(skips() == 12);
        // An outline in the next-darker colour, DarkGrey round bands 9 to 12:
        // a solid level one cell wide at the end of a gradient is an outline,
        // not its bottom rung, whether the canvas edge or black is beyond it.
        auto rowSkips = [&](const std::vector<int>& rungs) {
            return count(lint::checkLadder(rows(r, rungs), r), Kind::RungSkip);
        };
        CHECK(rowSkips({4, 9, 9, 10, 10, 11, 11, 12, 12, 4}) == 0);
        CHECK(rowSkips({0, 4, 9, 9, 10, 10, 11, 11, 12, 12, 4, 0}) == 0);
        // but a DarkGrey plane two cells wide is the gradient's bottom rung
        CHECK(rowSkips({0, 4, 4, 9, 9, 10, 10, 11, 11, 12, 12, 4, 0}) == 6);
        // and a one-cell shade at the end is a rung, not an outline
        CHECK(rowSkips({0, 3, 9, 9, 10, 10, 11, 11, 12, 12, 0}) == 6);
        // a real DarkGrey outline on a Grey-to-White fall
        CHECK(rowSkips({0, 11, 10, 9, 8, 8, 4, 0}) == 0);
    }
    SECTION("only the outline brush's colour at a sequence's end is an outline") {
        // An end cell is left out only when it is one cell, solid, and the
        // ramp colour next below the darker colour beside it. Anything else
        // at the end is a rung, and the jump next to it is judged.
        auto rowSkips = [&](const std::vector<int>& rungs) {
            return count(lint::checkLadder(rows(r, rungs), r), Kind::RungSkip);
        };
        // a lighter end cell is a highlight, not an outline
        CHECK(rowSkips({0, 8, 10, 12, 0}) == 12);
        CHECK(rowSkips({0, 12, 10, 8, 8, 8, 0}) == 12);
        CHECK(rowSkips({0, 8, 8, 8, 10, 12, 0}) == 12);
        CHECK(rowSkips({5, 6, 8}) == 6);
        // R2 with a one-cell White, at black and at the canvas edge
        CHECK(rowSkips({0, 1, 2, 3, 4, 4, 12, 0}) == 6);
        CHECK(rowSkips({1, 2, 3, 4, 12}) == 6);
        // a climb ending on one Grey cell, skipping 5 to 7
        CHECK(rowSkips({0, 1, 2, 3, 4, 8, 0}) == 6);
        // DarkGrey beside rung 6 is that rung's own darker colour, not the
        // next below it: the climb 4, 6, 8 has two skips
        CHECK(rowSkips({0, 4, 6, 8, 8, 8, 8, 0}) == 12);
        // the cylinder with solid DarkGrey ends: every jump of every row
        CHECK(rowSkips({0, 4, 6, 8, 10, 12, 10, 8, 6, 4, 0}) == 48);
    }
    SECTION("one shade glyph") {
        for (int x = 2; x <= 9; x += 2)
            c.at(x, 2) = cell(kShadeMedium, White, Grey);
        CHECK(count(lint::checkLadder(c, r), Kind::OneGlyph) == 1);
        c.at(2, 2) = cell(kShadeLight, White, Grey);
        CHECK(count(lint::checkLadder(c, r), Kind::OneGlyph) == 0);
    }
    SECTION("noise") {
        for (int y = 0; y < 6; ++y)
            for (int x = (y % 2); x < 12; x += 2)
                c.at(x, y) = cell(kUpperHalf, White, Grey);
        const auto rep = lint::checkLadder(c, r);
        CHECK(rep.noise > 0.02);
        CHECK(count(rep, Kind::Noise) == 1);
    }
    SECTION("the box limits the check and is clipped to the canvas") {
        c.at(10, 4) = cell(kShadeMedium, White, Black);
        CHECK(lint::checkLadder(c, r, lint::Box{0, 0, 5, 5}).clean());
        CHECK_FALSE(lint::checkLadder(c, r, lint::Box{8, 3, 40, 40}).clean());
        CHECK(lint::checkLadder(c, r, lint::Box{8, 3, 40, 40}).cells == 12);
        CHECK(lint::checkLadder(c, r, lint::Box{20, 20, 30, 30}).cells == 0);
    }
    SECTION("text and other colours aren't judged") {
        c.at(1, 1) = cell(U'A', White, Black);
        c.at(2, 1) = cell(U' ', Black, Blue);
        c.at(1, 3) = cell(kShadeLight, Blue, Black);  // a shade on another ramp
        c.at(3, 3) = cell(kShadeMedium, LightRed, Grey);
        Cell rgb = cell(kShadeLight, White, Grey);
        rgb.fg = Color::fromRgb({0xff, 0xff, 0xff});
        c.at(5, 3) = rgb;
        const auto rep = lint::checkLadder(c, r);
        for (const auto& f : rep.findings)
            UNSCOPED_INFO(f.message);
        CHECK(rep.clean());
        CHECK(rep.ladderCells == 67);
    }
    SECTION("blinking shades are judged on their colours") {
        Cell b = rungCell(r, 1);
        b.attrs = attr::Blink;
        c.at(4, 2) = b;
        CHECK(count(lint::checkLadder(c, r), Kind::NonNeighbourShade) == 0);
        b = cell(kShadeMedium, White, DarkGrey);
        b.attrs = attr::Blink;
        c.at(4, 2) = b;
        CHECK(count(lint::checkLadder(c, r), Kind::NonNeighbourShade) == 1);
    }
}

TEST_CASE("the ladder check reads short and odd lines safely", "[lint][ladder]") {
    // Build with -D_GLIBCXX_ASSERTIONS (or a sanitizer) to catch a read past
    // the end of a line's levels.
    const Ramp r = steel();
    auto rowSkips = [&](const std::vector<int>& rungs) {
        return count(lint::checkLadder(rows(r, rungs), r), Kind::RungSkip);
    };
    // 12 falling to 10 goes on falling: one skip a row
    CHECK(rowSkips({0, 12, 10, 9, 8, 8, 8, 0}) == 6);
    for (const std::vector<int>& line : std::vector<std::vector<int>>{
             {4}, {12}, {3}, {0}, {0, 0}, {0, 0, 0, 0}, {4, 8}, {8, 4}, {0, 4}, {4, 0}, {0, 4, 0}, {4, 12}, {12, 4}})
        CHECK(rowSkips(line) == 0);
    // one cell, and one row
    Canvas one(1, 1);
    one.at(0, 0) = rungCell(r, 4);
    CHECK(count(lint::checkLadder(one, r), Kind::RungSkip) == 0);
    Canvas line(5, 1);
    for (int x = 0; x < 5; ++x)
        line.at(x, 0) = rungCell(r, std::vector<int>{0, 8, 10, 12, 0}[static_cast<size_t>(x)]);
    CHECK(count(lint::checkLadder(line, r), Kind::RungSkip) == 2);
}

TEST_CASE("the ladder check is linear in the canvas", "[lint][ladder]") {
    // A ladder over the largest canvas the parser makes, K░k K▒k ... K▓k K
    // with the ▒ step all the way across: one wide rung a row, found without
    // rescanning the columns.
    Canvas c = filled(kMaxColumns, kMaxRows, cell(kShadeMedium, DarkGrey, Black));
    for (int y = 0; y < kMaxRows; ++y) {
        c.at(0, y) = rungCell(steel(), 1);
        c.at(kMaxColumns - 2, y) = rungCell(steel(), 3);
        c.at(kMaxColumns - 1, y) = rungCell(steel(), 4);
    }
    const auto rep = lint::checkLadder(c, steel());
    CHECK(rep.shadeCells == (kMaxColumns - 1) * kMaxRows);
    CHECK(count(rep, Kind::WideRung) == kMaxRows);
    // a flat field of it is no gradient
    const Canvas flat = filled(kMaxColumns, kMaxRows, cell(kShadeMedium, DarkGrey, Black));
    CHECK(count(lint::checkLadder(flat, steel()), Kind::WideRung) == 0);
    CHECK(count(lint::checkLadder(flat, steel()), Kind::OneGlyph) == 1);
}

TEST_CASE("a sphere painted with the shade brush passes the speckle and ladder checks", "[lint][golden]") {
    const Canvas c = paintSphere();
    const Ramp r = steel();
    // a stepped ladder: no two neighbours more than one rung apart, the
    // black ground included
    CHECK(rungSkips(c, r) == 0);
    const auto rep = lint::checkLadder(c, r);
    for (const auto& f : rep.findings)
        UNSCOPED_INFO(f.message);
    CHECK(rep.clean());
    CHECK(rep.noise == 0.0);
    // a modelled form: a fifth or more of the ball in shade cells (hand
    // shading measures 20-39% on studied pieces), every glyph of the ladder used
    int ball = 0;
    std::set<char32_t> glyphs;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (*rungOf(r, c.at(x, y)) > 0) {
                ++ball;
                if (*rungOf(r, c.at(x, y)) % kRungsPerStep)
                    glyphs.insert(c.at(x, y).ch);
            }
    CHECK(rep.shadeCells * 5 >= ball);
    CHECK(glyphs.size() == 3);

    // The golden file: the ball as the editor would save it.
    AnsiDocument doc;
    doc.canvas = c;
    const std::string bytes = writeAnsi(doc).bytes;
    const auto golden = bwtest::dataDir() / "brushes/sphere-steel.ans";
    if (std::getenv("BW_WRITE_GOLDEN"))
        std::ofstream(golden, std::ios::binary) << bytes;
    // The SAUCE date is today's; compare up to it.
    const std::string want = bwtest::readFile(golden);
    REQUIRE(want.size() == bytes.size());
    CHECK(want.substr(0, want.size() - 128) == bytes.substr(0, bytes.size() - 128));
    // and reads back as the same ladder (a trimmed blank comes back with the
    // default fg, which a space doesn't show)
    const Canvas back = parseAnsi(want).canvas;
    REQUIRE(back.width() == c.width());
    REQUIRE(back.height() == c.height());
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            REQUIRE(rungOf(r, back.at(x, y)) == rungOf(r, c.at(x, y)));
}
