// The speckle and ladder checks, and the golden sphere: a ball painted with
// the ramp shade brush, dab by dab, that has to pass them.

#include "blockwright/ansi.h"
#include "portable_random.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/gradient.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/lint.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
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

// #71: a fallback speck is a ▀/▄ cell that was a shade cell's half where
// neither half shows the pixel's colour or one the shading round it shows
// there, read from the canvas alone: the shade's rung from the cells above
// and below it and at the ends of its run along the row. The free half shows
// a colour the cut shade cell had that the shading there doesn't show: on
// K░w, grey (its foreground). A half in a colour no shade cell there has was
// drawn, and so was its cell.
TEST_CASE("the ladder check finds fallback specks beside shading", "[lint][ladder]") {
    const Ramp r = steel();
    // a field of K░w (rung 5, DarkGrey's main colour)
    Canvas c(7, 5);
    auto field = [&](int rung) {
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 7; ++x)
                c.at(x, y) = rungCell(r, rung);
    };
    auto fallback = [&](bool ice = true) {
        lint::LadderOptions o;
        o.fallback = true;
        o.ice = ice;
        return count(lint::checkLadder(c, r, {}, o), Kind::FallbackSpeck);
    };
    lint::LadderOptions on;
    on.fallback = true;
    field(5);
    SECTION("a shape's edge whose free half fell is a speck") {
        // a yellow shape's lower edge cut into the shade, its free half
        // grey where the shade shows dark grey
        c.at(3, 1) = cell(kFullBlock, Yellow, Black);
        c.at(3, 2) = cell(kUpperHalf, Yellow, Grey);
        // judged only when asked for
        CHECK(count(lint::checkLadder(c, r), Kind::FallbackSpeck) == 0);
        auto rep = lint::checkLadder(c, r, {}, on);
        REQUIRE(count(rep, Kind::FallbackSpeck) == 1);
        const auto it = std::find_if(rep.findings.begin(), rep.findings.end(),
                                     [](const auto& f) { return f.kind == Kind::FallbackSpeck; });
        CHECK(it->x == 3);
        CHECK(it->y == 2);
        // the half shows grey, which the K░w beside it falls back to but
        // doesn't show (#305)
        CHECK(it->message == "cell 3,2: a fallback speck, a half block in a colour the shading beside it doesn't "
                             "show but falls back to where a pixel cuts it");
        // the same pixel over the shade's own colour isn't
        c.at(3, 2) = cell(kUpperHalf, Yellow, DarkGrey);
        CHECK(fallback() == 0);
        c.at(3, 2) = cell(kLowerHalf, Yellow, DarkGrey);
        CHECK(fallback() == 0);
        // nor over white or black, which no shade cell there has: a rim
        // drawn under the shape
        c.at(3, 2) = cell(kUpperHalf, Yellow, White);
        CHECK(fallback() == 0);
        c.at(3, 2) = cell(kUpperHalf, Yellow, Black);
        CHECK(fallback() == 0);
        // nor is one of a single colour, or a full pixel
        c.at(3, 2) = cell(kUpperHalf, White, White);
        CHECK(fallback() == 0);
        c.at(3, 2) = cell(kFullBlock, White, Black);
        CHECK(fallback() == 0);
        // in K▓k, which shows dark grey, a free half fallen to black is one
        field(3);
        c.at(3, 1) = cell(kFullBlock, Yellow, Black);
        c.at(3, 2) = cell(kUpperHalf, Yellow, Black);
        CHECK(fallback() == 1);
    }
    SECTION("a run of them along the row is read from its ends") {
        for (int x = 1; x < 6; ++x) {
            c.at(x, 1) = cell(kFullBlock, Yellow, Black);
            c.at(x, 2) = cell(kUpperHalf, Yellow, Grey);
        }
        CHECK(fallback() == 5);
        for (int x = 1; x < 6; ++x)
            c.at(x, 2) = cell(kUpperHalf, Yellow, DarkGrey);
        CHECK(fallback() == 0);
    }
    SECTION("only beside shading") {
        // a solid field has no fallback to get wrong: a half block there is
        // a pixel cell of its own
        field(4);
        c.at(3, 1) = cell(kFullBlock, Yellow, Black);
        c.at(3, 2) = cell(kUpperHalf, Yellow, Grey);
        CHECK(fallback() == 0);
        // nor does black, or text
        field(0);
        c.at(3, 1) = cell(kFullBlock, Yellow, Black);
        c.at(3, 2) = cell(kUpperHalf, Yellow, Grey);
        CHECK(fallback() == 0);
    }
    SECTION("a rim drawn on purpose isn't a speck") {
        // a grey bevel over a black outline, between white and the shading
        // (cocktober's gravestone base): grey is what W▓w above the white
        // would fall to, but no shade cell of the shading below is black
        for (int x = 0; x < 7; ++x) {
            c.at(x, 1) = rungCell(r, 12);
            c.at(x, 2) = cell(kUpperHalf, Grey, Black);
        }
        CHECK(fallback() == 0);
        // a stripe of white across the shading, its other half fallen grey
        field(5);
        for (int x = 0; x < 7; ++x)
            c.at(x, 2) = cell(kLowerHalf, White, Grey);
        CHECK(fallback() == 0);
    }
    SECTION("a lone pixel beside ink or the canvas's edge isn't judged") {
        // a lone yellow pixel just above black ink, its free half the
        // shade's own colour (a star over a silhouette): the pixel faces
        // the shading, the free half faces the ink, and nothing says which
        // half is which
        c.at(3, 2) = cell(kUpperHalf, Yellow, DarkGrey);
        c.at(3, 3) = cell(U' ', White, Black);
        CHECK(fallback() == 0);
        // nor at the canvas's edge
        c.at(3, 3) = rungCell(r, 5);
        c.at(3, 2) = rungCell(r, 5);
        c.at(3, 4) = cell(kUpperHalf, Yellow, DarkGrey);
        CHECK(fallback() == 0);
    }
    SECTION("what the canvas alone can't tell isn't flagged") {
        // a black pixel cut into K▓k whose free half fell to black: a black
        // space, as ink drawn there is
        field(3);
        c.at(3, 1) = cell(U' ', White, Black);
        c.at(3, 2) = cell(U' ', White, Black);
        CHECK(fallback() == 0);
        // with iCE off, a yellow pixel on a yellow shape's edge over the
        // shade that fell to brown, its dark counterpart (PR #68's F1): a
        // brown rim under a yellow shape, as one drawn there is
        field(5);
        c.at(3, 1) = cell(kFullBlock, Yellow, Black);
        c.at(3, 2) = cell(kLowerHalf, DarkGrey, Brown);
        CHECK(fallback(false) == 0);
        // a shape's edge on the bottom row, its free half facing off the
        // canvas
        c.at(3, 3) = cell(kFullBlock, Yellow, Black);
        c.at(3, 4) = cell(kUpperHalf, Yellow, Grey);
        CHECK(fallback() == 0);
        // a lone yellow pixel cut into the shading, its free half fallen to
        // grey: a yellow pixel drawn over a grey one, as a frame's rivet or
        // a stripe's end is
        field(5);
        c.at(3, 2) = cell(kUpperHalf, Yellow, Grey);
        CHECK(fallback() == 0);
    }
    SECTION("iCE off judges the colours a cell shows") {
        // W▓w (rung 11) shows white; with iCE off a yellow shape's edge over
        // it leaves the free half grey, white's dark counterpart
        field(11);
        c.at(3, 1) = cell(kFullBlock, Yellow, Black);
        c.at(3, 2) = cell(kUpperHalf, Yellow, Grey);
        CHECK(fallback(true) == 1);
        CHECK(fallback(false) == 0);
    }
    SECTION("with iCE off a shade cell falls to its colours' dark counterparts") {
        // #110: Y▓R (fire's rung 11) shows yellow on red with iCE off; a
        // blue shape's lower edge cut into it leaves the free half red,
        // light red's dark counterpart, which no fire rung there shows
        const Ramp fire = *findRamp("fire");
        Canvas f(7, 5);
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 7; ++x)
                f.at(x, y) = rungCell(fire, 11);
        f.at(3, 1) = cell(kFullBlock, Blue, Black);
        f.at(3, 2) = cell(kUpperHalf, Blue, Red);
        auto specks = [&](bool ice) {
            lint::LadderOptions o;
            o.fallback = true;
            o.ice = ice;
            return count(lint::checkLadder(f, fire, {}, o), Kind::FallbackSpeck);
        };
        CHECK(specks(false) == 1);
        // with iCE on light red shows, so red is no colour the cell has
        CHECK(specks(true) == 0);
        // brown, yellow's dark counterpart, falls the same way
        f.at(3, 2) = cell(kUpperHalf, Blue, Brown);
        CHECK(specks(false) == 1);
        CHECK(specks(true) == 0);
        // green is the dark counterpart of neither, so it was drawn
        f.at(3, 2) = cell(kUpperHalf, Blue, Green);
        CHECK(specks(false) == 0);
    }
    SECTION("a solid rung has nothing to fall back to") {
        // #110: a grey half over a blue shape's upper edge, under solid dark
        // grey and with solid dark grey at its run's end: the shade it faces
        // is that solid rung, which shows dark grey and could only have
        // fallen to it, so the grey was drawn
        c.at(2, 2) = rungCell(r, 4);
        c.at(3, 1) = rungCell(r, 4);
        c.at(3, 2) = cell(kUpperHalf, Grey, Blue);
        c.at(3, 3) = cell(kFullBlock, Blue, Black);
        CHECK(fallback() == 0);
        // the same half under K░w, which can fall to grey, is a speck
        c.at(2, 2) = rungCell(r, 5);
        c.at(3, 1) = rungCell(r, 5);
        CHECK(fallback() == 1);
    }
    SECTION("the index keeps them") {
        c.at(3, 1) = c.at(4, 1) = cell(kFullBlock, Yellow, Black);
        lint::LadderIndex index(c, r, on);
        Canvas after = c;
        after.at(3, 2) = cell(kUpperHalf, Yellow, Grey);
        auto added = index.apply(after, {{3, 2}});
        REQUIRE(added.size() == 1);
        CHECK(added.front().kind == Kind::FallbackSpeck);
        CHECK(index.has(added.front()));
        // the run grows: the new cell is a speck, the old one stays one
        Canvas more = after;
        more.at(4, 2) = after.at(3, 2);
        added = index.apply(more, {{4, 2}});
        REQUIRE(added.size() == 1);
        CHECK(added.front().x == 4);
        auto sorted = [](std::vector<lint::LadderFinding> v) {
            std::sort(v.begin(), v.end());
            return v;
        };
        CHECK(sorted(index.findings()) == sorted(lint::checkLadder(more, r, {}, on).findings));
    }
}

// Round 1 of the review of #99: drawn half-block rims beside shading (a
// gravestone's bevel over its outline, a monitor frame's top border, a sun's
// stripes) are drawn, not fallbacks, and finished pieces have rows of them.
TEST_CASE("drawn half-block rims beside shading are not fallback specks", "[lint][ladder]") {
    auto specksOnRow = [](const char* file, const char* ramp, int row) {
        const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / file));
        lint::LadderOptions o;
        o.fallback = true;
        o.ice = doc.ice;
        int n = 0;
        for (const auto& f : lint::checkLadder(doc.canvas, *findRamp(ramp), {}, o).findings)
            n += f.kind == Kind::FallbackSpeck && f.y == row;
        return n;
    };
    CHECK(specksOnRow("cocktober.ans", "steel", 67) == 0);
    CHECK(specksOnRow("battlestation.ans", "sunset", 3) == 0);
    CHECK(specksOnRow("outrun-sunset.ans", "sunset", 12) == 0);
    // #110: a yellow stripe through light red, beside solid fire: a solid
    // rung has nothing to fall back to
    CHECK(specksOnRow("burning-of-losgar.ans", "fire", 24) == 0);
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

namespace {

// A finding's whole identity, as lint.h compares findings.
using FindingKey = lint::LadderFinding;

std::set<FindingKey> keysOf(const std::vector<lint::LadderFinding>& findings) {
    return {findings.begin(), findings.end()};
}

// A canvas for the exactness test: rows or columns of a walk along the
// ramp (so runs and steps of every width), plateaus, or noise, with some
// cells off the ramp (red, a blinking shade) and some shades of two colours
// that aren't neighbours.
Canvas randomLadder(const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int top = r.topRung();
    std::vector<int> walk(size_t(std::max(w, h)));
    int g = pick(0, top);
    for (int& v : walk) {
        if (pick(0, 3) == 0)
            g = std::clamp(g + pick(-2, 2), 0, top);
        v = g;
    }
    const int kind = pick(0, 3);
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = kind == 0 ? walk[size_t(x)] : kind == 1 ? walk[size_t(y)] : kind == 2 ? walk[size_t((x / 5 + y / 3) % walk.size())]
                                                                                          : pick(0, top);
            if (pick(0, 9) == 0)
                v = std::clamp(v + pick(-3, 3), 0, top);
            c.at(x, y) = rungCell(r, v);
            const int odd = pick(0, 40);
            if (odd == 0)
                c.at(x, y) = cell(kFullBlock, Red, Black);
            else if (odd == 1)
                c.at(x, y) = cell(kShadeMedium, White, Black);
            else if (odd == 2)
                c.at(x, y).attrs = attr::Blink;
        }
    return c;
}


// Squares of one rung on black for the exactness test, each cell round them
// one rung lower, one higher or black, row by row and column by column, so
// a square is a wide step across its rows, down its columns, both or
// neither, and one cell's change swaps one for the other at a corner.
Canvas randomSquares(const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int top = r.topRung();
    Canvas c(w, h);
    auto put = [&](int x, int y, int g) {
        if (x >= 0 && y >= 0 && x < w && y < h)
            c.at(x, y) = rungCell(r, std::clamp(g, 0, top));
    };
    for (int n = pick(1, 3); n > 0; --n) {
        const int sw = pick(2, 7), sh = pick(2, 7), x0 = pick(-1, w - 2), y0 = pick(-1, h - 2);
        const int g = pick(1, top - 1);
        auto end = [&] {
            const int k = pick(0, 9);
            return k < 4 ? g - pick(1, 2) : k < 7 ? g + pick(1, 2) : 0;
        };
        for (int y = y0; y < y0 + sh; ++y)
            for (int x = x0; x < x0 + sw; ++x)
                put(x, y, g);
        const int left = end(), right = end(), above = end(), below = end();
        for (int y = y0; y < y0 + sh; ++y) {
            put(x0 - 1, y, pick(0, 3) ? left : end());
            put(x0 + sw, y, pick(0, 3) ? right : end());
        }
        for (int x = x0; x < x0 + sw; ++x) {
            put(x, y0 - 1, pick(0, 3) ? above : end());
            put(x, y0 + sh, pick(0, 3) ? below : end());
        }
    }
    return c;
}
} // namespace

namespace {

// The outline colour beside a level, as checkLadder's end-level rule takes
// it: the ramp colour next below the level's darker colour, solid; none
// where that is black (the ground) or below the ramp.
std::optional<int> outlineRung(const Ramp& r, int rung) {
    const int g = rung - rung % kRungsPerStep - kRungsPerStep;
    if (g < 0 || r.colours[size_t(g / kRungsPerStep)] == Black)
        return std::nullopt;
    return g;
}

// A canvas of shapes on black for the whole-canvas test: blocks shaded in
// steps along a row or a column, each in the outline colour of its edge
// level (outlineRung) one cell round, some cells pixel noise (half
// blocks), so outlines, wide steps hiding others and the global rules all
// turn up.
Canvas randomShapes(const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int top = r.topRung();
    Canvas c(w, h);
    for (int n = pick(1, 3); n > 0; --n) {
        const int x0 = pick(0, w - 1), y0 = pick(0, h - 1), x1 = std::min(w - 1, x0 + pick(2, 14)),
                  y1 = std::min(h - 1, y0 + pick(2, 10));
        const bool across = pick(0, 1);
        const int base = pick(5, top), stepWidth = pick(1, 5), dir = pick(0, 1) ? 1 : -1;
        auto level = [&](int x, int y) {
            const int i = across ? x - x0 : y - y0;
            return std::clamp(base + dir * (i / stepWidth), 4, top);
        };
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const bool rim = x == x0 || x == x1 || y == y0 || y == y1;
                int v = level(x, y);
                if (rim && pick(0, 4))
                    if (const auto line = outlineRung(r, level(std::clamp(x, x0 + 1, std::max(x0 + 1, x1 - 1)),
                                                                     std::clamp(y, y0 + 1, std::max(y0 + 1, y1 - 1)))))
                        v = *line;
                c.at(x, y) = rungCell(r, v);
            }
    }
    for (int k = pick(0, 3) * pick(0, 8); k > 0; --k)
        c.at(pick(0, w - 1), pick(0, h - 1)) = cell(pick(0, 1) ? kUpperHalf : kLowerHalf, White, pick(0, 1) ? Black : Red);
    return c;
}

// An outlined ellipse on black for the whole-canvas test: rings of the
// ramp inward, its edge (a cell with an outside cell among its eight
// neighbours) in the outline colour of its ring, so the edge runs as a
// staircase two cells to a row along its diagonals, as the outline brush
// leaves it.
Canvas randomEllipse(const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int top = r.topRung();
    const double cx = pick(0, w - 1), cy = pick(0, h - 1), rx = pick(3, 16), ry = rx / 2 + pick(0, 2);
    const double ringWidth = pick(1, 4) * 0.5;
    const int lo = pick(5, top - 1);
    auto inside = [&](int x, int y) {
        const double dx = (x - cx) / rx, dy = (y - cy) / ry;
        return dx * dx + dy * dy <= 1.0;
    };
    auto level = [&](int x, int y) {
        const double d = std::hypot((x - cx) / 2.0, y - cy);
        return std::clamp(top - int(d / ringWidth), lo, top);
    };
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!inside(x, y))
                continue;
            bool edge = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    edge = edge || (nx >= 0 && ny >= 0 && nx < w && ny < h && !inside(nx, ny));
                }
            int v = level(x, y);
            if (edge)
                if (const auto line = outlineRung(r, v); line && pick(0, 9))
                    v = *line;
            c.at(x, y) = rungCell(r, v);
        }
    return c;
}

// A field of shade glyphs for the one-glyph rule: ░, ▒ or ▓ (a rung's
// place in its step) in every step of the ramp, a few cells of another
// glyph or solid, so a change can leave the shading one glyph or end that.
Canvas randomShadeField(const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int steps = r.topRung() / kRungsPerStep, glyph = pick(1, 3);
    Canvas c(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int step = pick(0, steps - 1), odd = pick(0, 9);
            c.at(x, y) = rungCell(r, step * kRungsPerStep + (odd == 0 ? pick(1, 3) : odd == 1 ? 0 : glyph));
        }
    return c;
}

// Bands stepping a rung at a time along rows or down columns, each 1-8
// cells wide, the lines shifted a cell now and then: the wide steps a shade
// brush narrows and widens.
Canvas randomBands(const Ramp& r, int w, int h, std::mt19937& rng) {
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const int top = r.topRung();
    const bool across = pick(0, 1);
    const int n = across ? w : h;
    std::vector<int> walk;
    int g = pick(1, top - 1), dir = pick(0, 1) ? 1 : -1;
    while (int(walk.size()) < n + 2) {
        for (int k = pick(1, 8); k > 0; --k)
            walk.push_back(g);
        if (pick(0, 5) == 0 || g + dir < 1 || g + dir > top)
            dir = -dir;
        g = std::clamp(g + dir, 1, top);
    }
    Canvas c(w, h);
    int shift = 0;
    for (int line = 0; line < (across ? h : w); ++line) {
        if (pick(0, 3) == 0)
            shift = pick(0, 2);
        for (int i = 0; i < n; ++i)
            (across ? c.at(i, line) : c.at(line, i)) = rungCell(r, walk[size_t(i + shift)]);
    }
    return c;
}

// The findings `after` has and `before` doesn't, every field compared,
// linting both whole.
std::set<FindingKey> fullDiff(const Canvas& before, const Canvas& after, const Ramp& r, const lint::LadderOptions& o = {}) {
    const auto had = keysOf(lint::checkLadder(before, r, {}, o).findings);
    std::set<FindingKey> out;
    for (const auto& key : keysOf(lint::checkLadder(after, r, {}, o).findings))
        if (!had.count(key))
            out.insert(key);
    return out;
}

// Whether a wide rung of `after` continues one of `before`: the same way,
// on the same line, of the same rung, their runs sharing a cell.
bool continues(const lint::LadderFinding& f, const Canvas& after, const lint::LadderFinding& old, const Canvas& before,
               const Ramp& r) {
    const bool across = f.ny == f.y, oldAcross = old.ny == old.y;
    if (across != oldAcross || (across ? f.y != old.y : f.x != old.x))
        return false;
    if (rungOf(r, after.at(f.x, f.y)) != rungOf(r, before.at(old.x, old.y)))
        return false;
    return across ? f.x <= old.nx && old.x <= f.nx : f.y <= old.ny && old.y <= f.ny;
}

// The far end of the run of the rung at position i along a row (across) or
// a column of c, going by step.
int runEnd(const Canvas& c, const Ramp& r, bool across, int line, int i, int step) {
    auto at = [&](int k) { return rungOf(r, across ? c.at(k, line) : c.at(line, k)); };
    const int n = across ? c.width() : c.height();
    const auto g = at(i);
    while (i + step >= 0 && i + step < n && at(i + step) == g)
        i += step;
    return i;
}

// Whether a skip of `after` moved one of `before` along its levels: the
// same way, on the same line, between the same two rungs, the runs of its
// two levels each sharing a cell with the old one's.
bool moves(const lint::LadderFinding& f, const Canvas& after, const lint::LadderFinding& old, const Canvas& before,
           const Ramp& r) {
    const bool across = f.ny == f.y, oldAcross = old.ny == old.y;
    if (across != oldAcross || (across ? f.y != old.y : f.x != old.x))
        return false;
    const int line = across ? f.y : f.x;
    const int i = across ? f.x : f.y, j = across ? f.nx : f.ny, oi = across ? old.x : old.y,
              oj = across ? old.nx : old.ny;
    auto rung = [&](const Canvas& c, int k) { return rungOf(r, across ? c.at(k, line) : c.at(line, k)); };
    if (rung(after, i) != rung(before, oi) || rung(after, j) != rung(before, oj))
        return false;
    return runEnd(after, r, across, line, i, -1) <= oi && runEnd(before, r, across, line, oi, -1) <= i &&
           oj <= runEnd(after, r, across, line, j, 1) && j <= runEnd(before, r, across, line, oj, 1);
}

// What a brush's guard counts as added, linting both whole, written out
// apart from LadderFinding::place and the index: a wide rung that continues
// none of `before`'s, a skip where `before` had none (at its cell and other
// cell) and that moves none of `before`'s, any other finding of a kind at a
// cell (and its other cell) where `before` had none,
// one glyph where `before` didn't have it, and noise over the limit that
// went up.
std::set<FindingKey> wholeDiff(const Canvas& before, const Canvas& after, const Ramp& r, const lint::LadderOptions& o = {}) {
    using Where = std::tuple<Kind, int, int, int, int>;
    auto where = [](const lint::LadderFinding& f) { return Where{f.kind, f.x, f.y, f.nx, f.ny}; };
    const auto was = lint::checkLadder(before, r, {}, o), is = lint::checkLadder(after, r, {}, o);
    std::set<Where> had;
    std::vector<lint::LadderFinding> wide, skip;
    for (const auto& f : was.findings) {
        had.insert(where(f));
        if (f.kind == Kind::WideRung)
            wide.push_back(f);
        if (f.kind == Kind::RungSkip)
            skip.push_back(f);
    }
    std::set<FindingKey> out;
    for (const auto& f : is.findings) {
        bool added = !had.count(where(f));
        if (f.kind == Kind::Noise)
            added = is.noise > was.noise;
        else if (f.kind == Kind::WideRung)
            added = std::none_of(wide.begin(), wide.end(),
                                 [&](const auto& old) { return continues(f, after, old, before, r); });
        else if (f.kind == Kind::RungSkip)
            added = added &&
                    std::none_of(skip.begin(), skip.end(), [&](const auto& old) { return moves(f, after, old, before, r); });
        if (added)
            out.insert(f);
    }
    return out;
}

// Both canvases' rungs and what was missed or extra, for a failure.
std::string showDiff(const Ramp& r, const Canvas& before, const Canvas& after, const std::set<FindingKey>& want,
                     const std::set<FindingKey>& got) {
    std::ostringstream out;
    for (const Canvas* c : {&before, &after}) {
        for (int y = 0; y < c->height(); ++y)
            for (int x = 0; x < c->width(); ++x) {
                const auto g = rungOf(r, c->at(x, y));
                out << (g ? std::to_string(*g) : std::string("-")) << (x + 1 < c->width() ? " " : "\n");
            }
        out << "--\n";
    }
    for (const auto& key : want)
        if (!got.count(key))
            out << "missed: " << key.message << " (" << key.nx << "," << key.ny << ")\n";
    for (const auto& key : got)
        if (!want.count(key))
            out << "extra: " << key.message << " (" << key.nx << "," << key.ny << ")\n";
    return out.str();
}

} // namespace

// What a change adds, from an index kept a change at a time and from the
// one-off addedFindingsOnce, is exactly what linting both whole canvases
// and comparing places finds (wholeDiff), and the index's findings are
// checkLadder's, every field of every finding: over noise, gradients,
// squares whose end rungs differ (a wide step moving from a row to a column
// at one cell), shaded shapes in their outline colour, outlined ellipses and
// shade glyph fields, one glyph, noise and fallback specks (half blocks of
// the ramp's colours, iCE on and off) included. Most indexes take a few
// changes; one in 25 takes 200-400, some of them undoing the one before.
TEST_CASE("a change adds exactly the findings linting both whole canvases does", "[lint][ladder]") {
    std::mt19937 rng(2026102303);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const Ramp r = steel();
    int cases = 0, swaps = 0, oneGlyph = 0, noisy = 0, lines = 0, longRuns = 0, reverts = 0, measured = 0,
        continued = 0, moved = 0, fallback = 0;
    for (int k = 0; k < 2500; ++k) {
        const int source = pick(0, 5);
        // shade glyph fields small, so a change can reach every odd glyph
        const int w = source == 3 ? pick(2, 6) : pick(1, 30), h = source == 3 ? pick(2, 5) : pick(1, 20);
        lint::LadderOptions o;
        o.maxRung = pick(1, 4);
        o.maxNoise = pick(0, 4) * 0.02;
        o.whole = pick(0, 3) != 0;
        o.fallback = pick(0, 3) != 0;
        o.ice = pick(0, 3) != 0;
        Canvas canvas = source == 0   ? randomLadder(r, w, h, rng)
                        : source == 1 ? randomShapes(r, w, h, rng)
                        : source == 2 ? randomEllipse(r, w, h, rng)
                        : source == 3 ? randomShadeField(r, w, h, rng)
                        : source == 4 ? randomSquares(r, w, h, rng)
                                      : randomBands(r, w, h, rng);
        lint::LadderIndex index(canvas, r, o);
        REQUIRE(keysOf(index.findings()) == keysOf(lint::checkLadder(canvas, r, {}, o).findings));
        const bool longRun = k % 25 == 0;
        longRuns += longRun;
        Canvas previous = canvas;
        std::vector<std::pair<int, int>> previousChanged;
        for (int step = 0, steps = longRun ? pick(200, 400) : pick(1, 4); step < steps; ++step) {
            // a patch, a line or scattered cells moved a rung or two, to
            // anywhere on the ramp, or off it; or the last change undone
            Canvas after = canvas;
            std::vector<std::pair<int, int>> changed;
            const bool revert = step > 0 && pick(0, 5) == 0;
            reverts += revert;
            const int shape = revert ? -1 : pick(0, 4);
            if (revert) {
                after = previous;
                changed = previousChanged;
            }
            const int pw = shape == 0 ? pick(1, 4) : shape == 1 ? pick(1, w) : 1, ph = shape == 0 ? pick(1, 3) : 1;
            if (shape == 3) {
                // a run's end, where a rung meets another along a row or a
                // column, taken off the ladder or into the run beside it
                for (int tries = 0; tries < 30; ++tries) {
                    const int x = pick(0, w - 1), y = pick(0, h - 1);
                    const bool across = pick(0, 1);
                    const int bx = across ? x - 1 : x, by = across ? y : y - 1;
                    if (bx < 0 || by < 0 || rungOf(r, canvas.at(x, y)) == rungOf(r, canvas.at(bx, by)))
                        continue;
                    after.at(x, y) = pick(0, 1) ? canvas.at(bx, by) : Cell{};
                    changed.push_back({x, y});
                    break;
                }
            } else if (shape == 4) {
                // the shading moved onto one glyph, most of the way
                const int glyph = pick(1, 3);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        if (const auto g = rungOf(r, canvas.at(x, y)); g && *g % kRungsPerStep && pick(0, 3)) {
                            after.at(x, y) = rungCell(r, *g - *g % kRungsPerStep + glyph);
                            changed.push_back({x, y});
                        }
            }
            for (int i = 0, n = shape == 2 ? pick(1, 6) : shape > 2 || shape < 0 ? 0 : 1; i < n; ++i) {
                const int x0 = pick(0, w - 1), y0 = pick(0, h - 1);
                for (int y = y0; y < std::min(h, y0 + ph); ++y)
                    for (int x = x0; x < std::min(w, x0 + pw); ++x) {
                        const auto was = rungOf(r, canvas.at(x, y));
                        const int how = pick(0, 11);
                        if (how == 0)
                            after.at(x, y) = cell(kFullBlock, Red, Black);
                        else if (how == 1)
                            after.at(x, y) = cell(U' ', White, Black);
                        else if (how == 2 && pick(0, 1))
                            after.at(x, y) = cell(pick(0, 1) ? kUpperHalf : kLowerHalf, White, pick(0, 1) ? Black : Red);
                        else if (how == 2) {  // a pixel cut into a shade cell, or its fallback
                            const bool top = pick(0, 1);
                            const std::uint8_t pixel = pick(0, 2) ? r.colours[size_t(pick(0, 3))] : Yellow;
                            after.at(x, y) = cell(top ? kUpperHalf : kLowerHalf, pixel, r.colours[size_t(pick(0, 3))]);
                            // mostly on the edge of a shape of the pixel's colour
                            if (const int edge = top ? y - 1 : y + 1; edge >= 0 && edge < h && pick(0, 3)) {
                                after.at(x, edge) = cell(kFullBlock, pixel, Black);
                                changed.push_back({x, edge});
                            }
                        }
                        else if (how == 3)
                            after.at(x, y) = canvas.at(pick(0, w - 1), pick(0, h - 1));  // a copy of another cell
                        else if (how < 6 || !was)
                            after.at(x, y) = rungCell(r, pick(0, r.topRung()));
                        else
                            after.at(x, y) = rungCell(r, std::clamp(*was + pick(-2, 2), 0, r.topRung()));
                        changed.push_back({x, y});
                        if (pick(0, 5) == 0)
                            changed.push_back({x, y});  // a cell named twice counts once
                    }
            }
            if (pick(0, 9) == 0)
                changed.push_back({pick(0, w - 1), pick(0, h - 1)});  // named, unchanged
            if (pick(0, 19) == 0)
                changed.push_back({w, -1});  // off the canvas
            const auto want = wholeDiff(canvas, after, r, o);
            if (step == 0) {
                const auto got = keysOf(lint::addedFindingsOnce(canvas, after, r, changed, o));
                INFO("canvas " << k << " (one-off)\n" << showDiff(r, canvas, after, want, got));
                REQUIRE(got == want);
            }
            if (pick(0, 1)) {
                const auto got = keysOf(index.added(after, changed));
                INFO("canvas " << k << " step " << step << " (added)\n" << showDiff(r, canvas, after, want, got));
                REQUIRE(got == want);
                REQUIRE(index.canvas() == canvas);
                REQUIRE(keysOf(index.findings()) == keysOf(lint::checkLadder(canvas, r, {}, o).findings));
            }
            const auto got = keysOf(index.apply(after, changed));
            INFO("canvas " << k << " step " << step << "\n" << showDiff(r, canvas, after, want, got));
            REQUIRE(got == want);
            REQUIRE(index.canvas() == after);
            const auto all = keysOf(lint::checkLadder(after, r, {}, o).findings);
            REQUIRE(keysOf(index.findings()) == all);
            REQUIRE(index.noise() == lint::noise(after));
            for (const auto& f : all) {
                REQUIRE(index.has(f));
                auto moved = f;
                ++moved.ny;
                REQUIRE(index.has(moved) == bool(all.count(moved)));
            }
            const auto had = keysOf(lint::checkLadder(canvas, r, {}, o).findings);
            for (const auto& key : want) {
                oneGlyph += key.kind == Kind::OneGlyph;
                noisy += key.kind == Kind::Noise;
                lines += key.kind != Kind::OneGlyph && key.kind != Kind::Noise;
                fallback += key.kind == Kind::FallbackSpeck;
                // a wide step that went from a row to a column at one cell, or back
                for (const auto& old : had)
                    swaps += key.kind == Kind::WideRung && old.kind == Kind::WideRung && key.x == old.x &&
                             key.y == old.y && (key.ny == key.y) != (old.ny == old.y) && !all.count(old);
            }
            // findings that only measure differently in place, which the
            // guard doesn't count, wide rungs that continue an old one from a
            // new first cell and skips moved from their old two cells
            for (const auto& key : fullDiff(canvas, after, r, o)) {
                measured += !want.count(key);
                if (key.kind == Kind::WideRung && !want.count(key)) {
                    bool samePlace = false;
                    for (const auto& old : had)
                        samePlace = samePlace || (old.kind == Kind::WideRung && old.place() == key.place());
                    continued += !samePlace;
                }
                if (key.kind == Kind::RungSkip && !want.count(key)) {
                    bool samePlace = false;
                    for (const auto& old : had)
                        samePlace = samePlace || (old.kind == Kind::RungSkip && old.place() == key.place());
                    moved += !samePlace;
                }
            }
            ++cases;
            previous = canvas;
            previousChanged = changed;
            canvas = after;
        }
    }
    UNSCOPED_INFO("cases " << cases << " swaps " << swaps << " one glyph " << oneGlyph << " noise " << noisy << " lines "
                           << lines << " long runs " << longRuns << " reverts " << reverts << " measured " << measured
                           << " continued " << continued << " moved " << moved << " fallback " << fallback);
    CHECK(cases > 30000);
    CHECK(longRuns == 100);
    CHECK(reverts > 4000);
    CHECK(measured > 500);
    CHECK(continued > 150);
    CHECK(moved > 100);
    CHECK(swaps >= 5);
    CHECK(oneGlyph > 50);
    CHECK(noisy > 50);
    CHECK(lines > 2000);
    CHECK(fallback > 100);
}

// Codex round 1 on #55, the run-based repro: taking a wide step across a
// row away shows the steps down the columns it hid, far from the change.
TEST_CASE("a change that ends a wide step across a row adds the column steps it hid", "[lint][ladder]") {
    const Ramp fire = *findRamp("fire");
    Canvas before(70, 65);
    auto put = [&](Canvas& c, int x, int y, int g) { c.at(x, y) = rungCell(fire, g); };
    for (int y = 2; y <= 59; ++y)
        for (int x = 2; x <= 60; ++x)
            put(before, x, y, 9);
    for (int y = 2; y <= 59; ++y)
        put(before, 1, y, 8);
    for (int x = 2; x <= 60; ++x)
        put(before, x, 1, 8);
    put(before, 61, 35, 10);
    put(before, 62, 35, 10);
    for (int x = 10; x <= 13; ++x)
        put(before, x, 60, 10);
    Canvas after = before;
    std::vector<std::pair<int, int>> changed;
    for (int y = 34; y <= 36; ++y) {
        put(after, 60, y, 4);
        changed.push_back({60, y});
    }
    const auto want = wholeDiff(before, after, fire);
    std::set<std::pair<int, int>> at;
    for (const auto& key : want)
        if (key.kind == Kind::WideRung)
            at.insert({key.x, key.y});
    CHECK(at == std::set<std::pair<int, int>>{{10, 2}, {11, 2}, {12, 2}, {13, 2}});
    CHECK(keysOf(lint::addedFindingsOnce(before, after, fire, changed)) == want);
}

// Review of #49 and #55 (P1): a wide step across a row that goes away can
// show one down a column at the same cell and of the same width, which only
// its other cell tells apart. Rung 9 over (2..5, 2..5), 8 left of and above
// its corner, 10 right of and below it: the row step (2,2)-(5,2) hides the
// column step (2,2)-(2,5). Ending the row's step at (6,2) shows the column's.
TEST_CASE("a change that swaps a row's wide step for a column's at one cell adds it", "[lint][ladder]") {
    const Ramp r = steel();
    Canvas before(9, 9);
    for (int y = 2; y <= 5; ++y)
        for (int x = 2; x <= 5; ++x)
            before.at(x, y) = rungCell(r, 9);
    before.at(1, 2) = rungCell(r, 8);
    before.at(2, 1) = rungCell(r, 8);
    before.at(6, 2) = rungCell(r, 10);
    before.at(2, 6) = rungCell(r, 10);
    // the steps' wide findings, as (x, y, nx, ny)
    auto wide = [&](const Canvas& c) {
        std::set<std::tuple<int, int, int, int, std::string>> out;
        for (const auto& f : lint::checkLadder(c, r).findings)
            if (f.kind == Kind::WideRung)
                out.insert({f.x, f.y, f.nx, f.ny, f.message});
        return out;
    };
    const std::string message = "cell 2,2: a rung 4 cells wide (at most 3)";
    CHECK(wide(before) == std::set<std::tuple<int, int, int, int, std::string>>{{2, 2, 5, 2, message}});
    for (const Cell& to : {Cell{}, rungCell(r, 9)}) {
        Canvas after = before;
        after.at(6, 2) = to;
        CHECK(wide(after) == std::set<std::tuple<int, int, int, int, std::string>>{{2, 2, 2, 5, message}});
        const auto want = wholeDiff(before, after, r);
        CHECK(want.count({Kind::WideRung, 2, 2, message, 2, 5}));
        CHECK(keysOf(lint::addedFindingsOnce(before, after, r, {{6, 2}})) == want);
    }
}

// Review of #49 and #55: one glyph is the whole canvas's, not the change's.
// A field of ░ (rung 1) with ▒ (rung 2) in its middle 2x2 becomes one glyph
// when the middle goes to ░; a change all of ░ where ▒ is left elsewhere
// doesn't.
TEST_CASE("a change that leaves the shading one glyph adds that, judged on the whole canvas", "[lint][ladder]") {
    const Ramp r = steel();
    const lint::LadderFinding one{Kind::OneGlyph, 0, 0, "the shading uses one glyph only"};
    Canvas before = filled(4, 4, rungCell(r, 1));
    std::vector<std::pair<int, int>> middle;
    for (int y = 1; y <= 2; ++y)
        for (int x = 1; x <= 2; ++x) {
            before.at(x, y) = rungCell(r, 2);
            middle.push_back({x, y});
        }
    Canvas after = before;
    for (const auto& [x, y] : middle)
        after.at(x, y) = rungCell(r, 1);
    const auto want = wholeDiff(before, after, r, {});
    CHECK(want.count(one));
    CHECK(keysOf(lint::addedFindingsOnce(before, after, r, middle)) == want);
    lint::LadderIndex index(before, r);
    CHECK(!index.has(one));
    CHECK(keysOf(index.apply(after, middle)) == want);
    CHECK(index.has(one));

    // ▒ left in a corner: the middle alone is all ░, the canvas isn't
    Canvas corner = before;
    corner.at(0, 0) = rungCell(r, 2);
    Canvas cornerAfter = corner;
    for (const auto& [x, y] : middle)
        cornerAfter.at(x, y) = rungCell(r, 1);
    CHECK(!wholeDiff(corner, cornerAfter, r, {}).count(one));
    CHECK(!keysOf(lint::addedFindingsOnce(corner, cornerAfter, r, middle)).count(one));
}

// Review of #59 (F1), the maintainer's ruling: a guard counts a finding as
// added when it is new by kind and place, not by the value in its message.
// Noise is added only when it is over the limit and went up.
TEST_CASE("a change adds findings by place, not by what they measure", "[lint][ladder]") {
    const Ramp r = steel();
    using Place = lint::LadderFinding::Place;

    SECTION("a wide rung's place is its first cell and the way its run goes") {
        const lint::LadderFinding across{Kind::WideRung, 1, 2, "w", 6, 2}, down{Kind::WideRung, 1, 2, "w", 1, 7};
        CHECK(across.place() == Place{Kind::WideRung, 1, 2, 2, 2});
        CHECK(down.place() == Place{Kind::WideRung, 1, 2, 1, 3});
        const lint::LadderFinding skip{Kind::RungSkip, 3, 4, "s", 4, 4};
        CHECK(skip.place() == Place{Kind::RungSkip, 3, 4, 4, 4});
        CHECK(lint::LadderFinding{Kind::Noise, 0, 0, "noise 0.083 (at most 0.020)"}.place() ==
              lint::LadderFinding{Kind::Noise, 0, 0, "noise 0.042 (at most 0.020)"}.place());
    }

    SECTION("noise that goes down adds nothing; noise that goes up over the limit adds it") {
        // 0.083 to 0.042, both over 0.02
        Canvas before = filled(6, 4, cell(kFullBlock, Grey, Black));
        before.at(2, 1) = cell(kUpperHalf, White, Grey);
        before.at(3, 2) = cell(kLowerHalf, White, Grey);
        Canvas after = before;
        after.at(3, 2) = cell(kFullBlock, Grey, Black);
        REQUIRE(lint::noise(before) > lint::noise(after));
        REQUIRE(lint::noise(after) > lint::LadderOptions{}.maxNoise);
        CHECK(fullDiff(before, after, r).size() == 1);  // the message changed
        CHECK(wholeDiff(before, after, r).empty());
        lint::LadderIndex index(before, r);
        CHECK(index.added(after, {{3, 2}}).empty());
        CHECK(index.apply(after, {{3, 2}}).empty());
        // and back up
        const auto up = index.apply(before, {{3, 2}});
        REQUIRE(up.size() == 1);
        CHECK(up[0].kind == Kind::Noise);
        CHECK(up[0].message == "noise 0.083 (at most 0.020)");
        // from clean to over the limit
        const Canvas clean = filled(6, 4, cell(kFullBlock, Grey, Black));
        Canvas speck = clean;
        speck.at(2, 1) = cell(kUpperHalf, White, Grey);
        lint::LadderIndex fresh(clean, r);
        const auto over = fresh.apply(speck, {{2, 1}});
        CHECK(keysOf(over) == wholeDiff(clean, speck, r));
        CHECK(std::count_if(over.begin(), over.end(), [](const auto& f) { return f.kind == Kind::Noise; }) == 1);
    }

    SECTION("a wide rung narrowed or widened in place adds nothing") {
        // six rows of 1, 2 x6, 3: each row's step is 6 cells wide
        const Canvas six = rows(r, {1, 2, 2, 2, 2, 2, 2, 3}), five = rows(r, {1, 2, 2, 2, 2, 2, 3, 3});
        REQUIRE(count(lint::checkLadder(six, r), Kind::WideRung) == 6);
        REQUIRE(count(lint::checkLadder(five, r), Kind::WideRung) == 6);
        std::vector<std::pair<int, int>> column;
        for (int y = 0; y < 6; ++y)
            column.push_back({6, y});
        CHECK(fullDiff(six, five, r).size() == 6);  // "6 cells wide" to "5 cells wide"
        CHECK(wholeDiff(six, five, r).empty());
        CHECK(wholeDiff(five, six, r).empty());
        lint::LadderIndex index(six, r);
        CHECK(index.apply(five, column).empty());  // narrowed
        CHECK(keysOf(index.findings()) == keysOf(lint::checkLadder(five, r).findings));
        CHECK(index.apply(six, column).empty());  // widened
        CHECK(keysOf(index.findings()) == keysOf(lint::checkLadder(six, r).findings));
        CHECK(lint::addedFindingsOnce(six, five, r, column).empty());
    }

    // Claude round 2 on #59 (N1): at either end, the same step
    SECTION("a wide rung narrowed or widened at its first cell adds nothing") {
        const Canvas six = rows(r, {1, 2, 2, 2, 2, 2, 2, 3}), five = rows(r, {1, 1, 2, 2, 2, 2, 2, 3});
        std::vector<std::pair<int, int>> column;
        for (int y = 0; y < 6; ++y)
            column.push_back({1, y});
        CHECK(fullDiff(six, five, r).size() == 6);
        CHECK(wholeDiff(six, five, r).empty());
        lint::LadderIndex index(six, r);
        CHECK(index.apply(five, column).empty());
        CHECK(index.apply(six, column).empty());
        CHECK(keysOf(index.findings()) == keysOf(lint::checkLadder(six, r).findings));
    }

    SECTION("a wide rung moved at both ends, joined or split adds nothing") {
        auto all = [](int x0, int x1) {
            std::vector<std::pair<int, int>> out;
            for (int y = 0; y < 6; ++y)
                for (int x = x0; x <= x1; ++x)
                    out.push_back({x, y});
            return out;
        };
        auto same = [&](const Canvas& before, const Canvas& after, const std::vector<std::pair<int, int>>& changed) {
            REQUIRE(count(lint::checkLadder(before, r), Kind::WideRung) > 0);
            REQUIRE(count(lint::checkLadder(after, r), Kind::WideRung) > 0);
            REQUIRE(!fullDiff(before, after, r).empty());
            CHECK(wholeDiff(before, after, r).empty());
            lint::LadderIndex index(before, r);
            CHECK(index.apply(after, changed).empty());
            CHECK(keysOf(index.findings()) == keysOf(lint::checkLadder(after, r).findings));
        };
        // shifted one cell: the first end in, the far end out
        same(rows(r, {1, 2, 2, 2, 2, 2, 3, 3}), rows(r, {1, 1, 2, 2, 2, 2, 2, 3}), all(1, 6));
        // a dab wider than the run, both ends out
        same(rows(r, {1, 1, 2, 2, 2, 2, 3, 3}), rows(r, {1, 2, 2, 2, 2, 2, 2, 3}), all(1, 6));
        // a wide run joined to the run past its far end
        same(rows(r, {1, 2, 2, 2, 2, 3, 2, 2, 3}), rows(r, {1, 2, 2, 2, 2, 2, 2, 2, 3}), all(5, 5));
        // and split again: the first part is still a wide step
        same(rows(r, {1, 2, 2, 2, 2, 2, 2, 2, 3}), rows(r, {1, 2, 2, 2, 2, 3, 2, 2, 3}), all(5, 5));
    }

    SECTION("a wide rung repainted to another rung is added") {
        const Canvas before = rows(r, {1, 2, 2, 2, 2, 2, 2, 7}), after = rows(r, {1, 3, 3, 3, 3, 3, 3, 7});
        std::vector<std::pair<int, int>> changed;
        for (int y = 0; y < 6; ++y)
            for (int x = 1; x <= 6; ++x)
                changed.push_back({x, y});
        const auto want = wholeDiff(before, after, r);
        CHECK(std::count_if(want.begin(), want.end(), [](const auto& f) { return f.kind == Kind::WideRung; }) == 6);
        lint::LadderIndex index(before, r);
        CHECK(keysOf(index.apply(after, changed)) == want);
    }

    SECTION("a wide rung down a column, narrowed or widened at either end, adds nothing") {
        // six columns of these rungs down
        auto columns = [&](const std::vector<int>& rungs) {
            Canvas c(6, int(rungs.size()));
            for (int y = 0; y < c.height(); ++y)
                for (int x = 0; x < 6; ++x)
                    c.at(x, y) = rungCell(r, rungs[size_t(y)]);
            return c;
        };
        auto rowOf = [](int y) {
            std::vector<std::pair<int, int>> out;
            for (int x = 0; x < 6; ++x)
                out.push_back({x, y});
            return out;
        };
        const Canvas six = columns({5, 6, 6, 6, 6, 6, 6, 7});
        REQUIRE(count(lint::checkLadder(six, r), Kind::WideRung) == 6);
        for (const auto& [y, g] : {std::pair{1, 5}, {6, 7}}) {
            Canvas five = six;
            for (int x = 0; x < 6; ++x)
                five.at(x, y) = rungCell(r, g);
            REQUIRE(count(lint::checkLadder(five, r), Kind::WideRung) == 6);
            CHECK(wholeDiff(six, five, r).empty());
            lint::LadderIndex index(six, r);
            CHECK(index.apply(five, rowOf(y)).empty());
            CHECK(index.apply(six, rowOf(y)).empty());
        }
    }

    SECTION("a wide rung in a new place is added") {
        // the last row flat, then stepped like the rest
        const Canvas after = rows(r, {1, 2, 2, 2, 2, 2, 2, 3});
        Canvas before = after;
        before.at(0, 5) = rungCell(r, 2);
        before.at(7, 5) = rungCell(r, 2);
        lint::LadderIndex index(before, r);
        const auto got = index.apply(after, {{0, 5}, {7, 5}});
        CHECK(keysOf(got) == wholeDiff(before, after, r));
        REQUIRE(got.size() == 1);
        CHECK(got[0] == lint::LadderFinding{Kind::WideRung, 1, 5, "cell 1,5: a rung 6 cells wide (at most 3)", 6, 5});
    }
}

// #66 (Claude round 3 on #59, R3-2): a skip whose boundary moved by a cell
// was added, since skips matched their two cells exactly. A skip is the one
// that was when it is the same way, on the same line, and between the same
// two cells or between the same two rungs with each of its levels sharing a
// cell with the old one's.
TEST_CASE("a skip moved along its levels adds nothing; one between other rungs is added", "[lint][ladder]") {
    const Ramp r = steel();
    auto all = [](int x0, int x1) {
        std::vector<std::pair<int, int>> out;
        for (int y = 0; y < 6; ++y)
            for (int x = x0; x <= x1; ++x)
                out.push_back({x, y});
        return out;
    };
    auto skips = [](const std::vector<lint::LadderFinding>& found) {
        return std::count_if(found.begin(), found.end(), [](const auto& f) { return f.kind == Kind::RungSkip; });
    };
    // six rows of 4 4 4 6 6 6 8 8 8: skips at 2|3 (4 to 6) and 5|6 (6 to 8)
    const Canvas before = rows(r, {4, 4, 4, 6, 6, 6, 8, 8, 8});
    REQUIRE(count(lint::checkLadder(before, r), Kind::RungSkip) == 12);

    SECTION("the 4 to 6 skip moved one cell on adds nothing") {
        const Canvas after = rows(r, {4, 4, 4, 4, 6, 6, 8, 8, 8});
        REQUIRE(count(lint::checkLadder(after, r), Kind::RungSkip) == 12);
        REQUIRE(fullDiff(before, after, r).size() == 6);  // now at 3|4
        CHECK(wholeDiff(before, after, r).empty());
        lint::LadderIndex index(before, r);
        CHECK(index.added(after, all(3, 3)).empty());
        CHECK(index.apply(after, all(3, 3)).empty());
        CHECK(keysOf(index.findings()) == keysOf(lint::checkLadder(after, r).findings));
        CHECK(index.apply(before, all(3, 3)).empty());  // and back
        CHECK(lint::addedFindingsOnce(before, after, r, all(3, 3)).empty());
    }

    SECTION("moved one cell back, only the widened 6 level is added") {
        const Canvas after = rows(r, {4, 4, 6, 6, 6, 6, 8, 8, 8});
        lint::LadderIndex index(before, r);
        const auto got = index.apply(after, all(2, 2));
        CHECK(keysOf(got) == wholeDiff(before, after, r));
        CHECK(skips(got) == 0);
        CHECK(got.size() == 6);  // the 6 level, 4 cells wide
    }

    SECTION("a skip down a column moved one cell adds nothing") {
        auto columns = [&](const std::vector<int>& rungs) {
            Canvas c(6, int(rungs.size()));
            for (int y = 0; y < c.height(); ++y)
                for (int x = 0; x < 6; ++x)
                    c.at(x, y) = rungCell(r, rungs[size_t(y)]);
            return c;
        };
        const Canvas down = columns({4, 4, 4, 6, 6, 6, 8, 8, 8}), moved = columns({4, 4, 4, 4, 6, 6, 8, 8, 8});
        std::vector<std::pair<int, int>> row;
        for (int x = 0; x < 6; ++x)
            row.push_back({x, 3});
        REQUIRE(!fullDiff(down, moved, r).empty());
        CHECK(wholeDiff(down, moved, r).empty());
        lint::LadderIndex index(down, r);
        CHECK(index.apply(moved, row).empty());
    }

    SECTION("a skip between another pair of rungs a cell over is added") {
        // 4 to 7 at 3|4, the levels sharing cells with 4 to 6 at 2|3
        const Canvas after = rows(r, {4, 4, 4, 4, 7, 7, 8, 8, 8});
        const auto want = wholeDiff(before, after, r);
        CHECK(want.size() == 6);
        lint::LadderIndex index(before, r);
        const auto got = index.apply(after, all(3, 5));
        CHECK(keysOf(got) == want);
        REQUIRE(skips(got) == 6);
        for (const auto& f : got) {
            CHECK(f.x == 3);
            CHECK(f.nx == 4);
            CHECK(index.has(f));
        }
        // and with the lower level changed: 3 to 6 at 3|4
        const Canvas lower = rows(r, {4, 4, 3, 3, 6, 6, 8, 8, 8});
        lint::LadderIndex again(before, r);
        const auto low = again.apply(lower, all(2, 3));
        CHECK(keysOf(low) == wholeDiff(before, lower, r));
        CHECK(skips(low) == 6);
    }

    SECTION("a skip between another pair of rungs at the same two cells is the one that was") {
        // 4 to 7 at 2|3, where 4 to 6 was: it measures differently in place
        const Canvas after = rows(r, {4, 4, 4, 7, 7, 7, 8, 8, 8});
        REQUIRE(fullDiff(before, after, r).size() == 6);
        CHECK(wholeDiff(before, after, r).empty());
        lint::LadderIndex index(before, r);
        CHECK(index.apply(after, all(3, 5)).empty());
    }

    SECTION("a skip moved past its old level is added") {
        // 4 to 6 from 1|2 to 4|5 and 6 to 8 from 3|4 to 6|7: the 6 levels
        // share no cell
        const Canvas from = rows(r, {4, 4, 6, 6, 8, 8, 8, 8, 8}), to = rows(r, {4, 4, 4, 4, 4, 6, 6, 8, 8});
        lint::LadderIndex index(from, r);
        const auto got = index.apply(to, all(2, 6));
        CHECK(keysOf(got) == wholeDiff(from, to, r));
        CHECK(skips(got) == 12);
    }
}

// Codex round 1 on #59 (#1, F2): with a width rule of 0, a one-cell run's
// step across its row and down its column end at the same cell, and an
// unchanged finding moved from one to the other came back as added. A rule
// below 1 is refused.
TEST_CASE("the ladder check refuses a width rule below one cell", "[lint][ladder]") {
    const Ramp r = steel();
    Canvas before(3, 3);
    const int grid[3][3] = {{0, 4, 0}, {4, 5, 6}, {0, 6, 0}};
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            before.at(x, y) = rungCell(r, grid[y][x]);
    Canvas after = before;
    after.at(2, 1) = rungCell(r, 4);
    for (const int maxRung : {0, -1}) {
        lint::LadderOptions o;
        o.maxRung = maxRung;
        CHECK_THROWS_AS(lint::checkLadder(before, r, {}, o), lint::LintError);
        CHECK_THROWS_AS(lint::LadderIndex(before, r, o), lint::LintError);
        CHECK_THROWS_AS(lint::addedFindingsOnce(before, after, r, {{2, 1}}, o), lint::LintError);
    }
    lint::LadderOptions o;
    o.maxRung = 1;
    lint::LadderIndex index(before, r, o);
    CHECK(keysOf(index.apply(after, {{2, 1}})) == wholeDiff(before, after, r, o));
}

// Review of #59 (F4): an index fed a canvas of another size, or a change
// list that leaves out a changed cell, went on quietly wrong.
TEST_CASE("an index refuses a canvas it doesn't hold", "[lint][ladder]") {
    const Ramp r = steel();
    const Canvas before = filled(8, 6, rungCell(r, 5));
    lint::LadderIndex index(before, r);
    const auto held = keysOf(index.findings());

    SECTION("another size") {
        Canvas after = filled(12, 6, rungCell(r, 5));
        after.at(9, 2) = rungCell(r, 7);
        std::vector<std::pair<int, int>> changed;
        for (int y = 0; y < 6; ++y)
            for (int x = 8; x < 12; ++x)
                changed.push_back({x, y});
        CHECK(!index.differsOnlyAt(after, changed));
        CHECK_THROWS_AS(index.apply(after, changed), lint::LintError);
        CHECK_THROWS_AS(index.added(after, changed), lint::LintError);
        CHECK(index.canvas() == before);
        CHECK(keysOf(index.findings()) == held);
    }

    SECTION("a changed cell left out") {
        Canvas after = before;
        after.at(2, 2) = rungCell(r, 6);
        after.at(5, 3) = rungCell(r, 7);
        CHECK(!index.differsOnlyAt(after, {{2, 2}}));
        CHECK(index.differsOnlyAt(after, {{2, 2}, {5, 3}}));
        CHECK(index.differsOnlyAt(after, {{5, 3}, {2, 2}, {2, 2}, {-1, 9}}));
#ifndef NDEBUG
        // a debug build checks the whole canvas before it moves
        CHECK_THROWS_AS(index.apply(after, {{2, 2}}), lint::LintError);
        CHECK(index.canvas() == before);
#endif
        CHECK(keysOf(index.apply(after, {{2, 2}, {5, 3}})) == wholeDiff(before, after, r));
        CHECK(index.canvas() == after);
    }
}

namespace {

// The reads an index makes applying a change, and what the change adds.
long long work(lint::LadderIndex& index, const Canvas& after, const std::vector<std::pair<int, int>>& changed,
               std::set<FindingKey>& got) {
    const long long start = lint::cellsRead();
    got = keysOf(index.apply(after, changed));
    return lint::cellsRead() - start;
}

} // namespace

// Review of #49 and #55 (P1/P2): judging a change read whole runs again,
// Θ(W × H) for one cell of a flat field or of a wide square's edge. An index
// reads the lines through the change: doubling the canvas at most doubles
// the work, and the answers stay exact.
TEST_CASE("a change costs the lines through it, however large the canvas", "[lint][ladder]") {
    const Ramp r = steel();
    std::vector<long long> flat, square, skipping, halves;
    for (const int n : {128, 256, 512}) {
        // Codex round 1 on #59 (#2): three rows of 1, 3, 5 over and over, a
        // skip every level, the last cell of the middle row moved to rung 2.
        // The skips the row keeps are compared in one pass, which counts.
        {
            const int w = 3 * n;
            Canvas before(w, 3);
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < w; ++x)
                    before.at(x, y) = rungCell(r, 1 + 2 * (x % 3));
            Canvas after = before;
            after.at(w - 1, 1) = rungCell(r, 2);
            lint::LadderIndex index(before, r);
            REQUIRE(count(lint::checkLadder(before, r), Kind::RungSkip) > n);
            std::set<FindingKey> got;
            skipping.push_back(work(index, after, {{w - 1, 1}}, got));
            CHECK(got == wholeDiff(before, after, r, {}));
        }
        // a field of rung 4 with one cell moved to rung 5
        {
            const Canvas before = filled(n, n, rungCell(r, 4));
            Canvas after = before;
            after.at(n / 2, n / 2) = rungCell(r, 5);
            lint::LadderIndex index(before, r);
            std::set<FindingKey> got;
            flat.push_back(work(index, after, {{n / 2, n / 2}}, got));
            CHECK(got == wholeDiff(before, after, r, {}));
            CHECK(!got.empty());
        }
        // a rung-9 square stepped 8 to 10 across it, three cells of its top
        // row moved to 8
        {
            Canvas before(n + 4, n + 4);
            for (int y = 2; y <= n + 1; ++y) {
                for (int x = 2; x <= n + 1; ++x)
                    before.at(x, y) = rungCell(r, 9);
                before.at(1, y) = rungCell(r, 8);
                before.at(n + 2, y) = rungCell(r, 10);
            }
            Canvas after = before;
            std::vector<std::pair<int, int>> changed;
            for (int x = n / 2 - 1; x <= n / 2 + 1; ++x) {
                after.at(x, 2) = rungCell(r, 8);
                changed.push_back({x, 2});
            }
            lint::LadderIndex index(before, r);
            std::set<FindingKey> got;
            square.push_back(work(index, after, changed, got));
            CHECK(got == wholeDiff(before, after, r, {}));
            CHECK(keysOf(index.findings()) == keysOf(lint::checkLadder(after, r).findings));
        }
        // #71: a rung-5 field with a yellow shape cut into it all but right
        // across, its lower edge a row of pixels whose free halves fell to
        // grey over a black line, and one cell of the line erased back to
        // the shading. The pixel above it becomes a fallback speck, judged
        // against the shading at the ends of its run along the row
        {
            lint::LadderOptions o;
            o.fallback = true;
            Canvas before = filled(n, n, rungCell(r, 5));
            for (int x = 1; x < n - 1; ++x) {
                before.at(x, n / 2 - 2) = cell(kFullBlock, Yellow, Black);
                before.at(x, n / 2 - 1) = cell(kUpperHalf, Yellow, Grey);
                before.at(x, n / 2) = cell(U' ', White, Black);
            }
            Canvas after = before;
            after.at(n / 2, n / 2) = rungCell(r, 5);
            lint::LadderIndex index(before, r, o);
            std::set<FindingKey> got;
            halves.push_back(work(index, after, {{n / 2, n / 2}}, got));
            CHECK(got == wholeDiff(before, after, r, o));
            CHECK(std::count_if(got.begin(), got.end(), [](const auto& f) { return f.kind == Kind::FallbackSpeck; }) ==
                  1);
            CHECK(keysOf(index.findings()) == keysOf(lint::checkLadder(after, r, {}, o).findings));
        }
    }
    for (size_t k = 0; k + 1 < flat.size(); ++k) {
        INFO("flat field: " << flat[k] << " then " << flat[k + 1] << " reads");
        CHECK(flat[k + 1] <= 2.1 * double(flat[k]));
        INFO("square's edge: " << square[k] << " then " << square[k + 1] << " reads");
        CHECK(square[k + 1] <= 2.1 * double(square[k]));
        INFO("skipping rows: " << skipping[k] << " then " << skipping[k + 1] << " reads");
        CHECK(skipping[k + 1] <= 2.1 * double(skipping[k]));
        INFO("rows of fallback specks: " << halves[k] << " then " << halves[k + 1] << " reads");
        CHECK(halves[k + 1] <= 2.1 * double(halves[k]));
    }
}

// Timings for the PR: taking an index, a 3x3 dab applied, and the one-off
// addedFindingsOnce, at two canvas sizes. Run with "[bench]".
TEST_CASE("ladder index timings", "[.][bench]") {
    using Clock = std::chrono::steady_clock;
    const Ramp r = steel();
    std::mt19937 rng(7);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    for (const auto& [w, h] : {std::pair{160, 100}, {400, 160}}) {
        const Canvas canvas = randomLadder(r, w, h, rng);
        auto t0 = Clock::now();
        lint::LadderIndex index(canvas, r);
        const double take = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        Canvas now = canvas;
        const int dabs = 2000;
        long long reads = 0;
        double applied = 0;
        for (int d = 0; d < dabs; ++d) {
            Canvas next = now;
            std::vector<std::pair<int, int>> changed;
            const int x0 = pick(0, w - 3), y0 = pick(0, h - 3);
            for (int y = y0; y < y0 + 3; ++y)
                for (int x = x0; x < x0 + 3; ++x) {
                    next.at(x, y) = rungCell(r, pick(0, r.topRung()));
                    changed.push_back({x, y});
                }
            const long long before = lint::cellsRead();
            t0 = Clock::now();
            index.apply(next, changed);
            applied += std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
            reads += lint::cellsRead() - before;
            now = std::move(next);
        }
        Canvas next = now;
        next.at(w / 2, h / 2) = rungCell(r, 5);
        t0 = Clock::now();
        lint::addedFindingsOnce(now, next, r, {{w / 2, h / 2}});
        const double oneOff = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        t0 = Clock::now();
        lint::checkLadder(now, r);
        const double whole = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        WARN(w << "x" << h << ": index " << take << " us, 3x3 dab " << applied / dabs << " us and " << reads / dabs
               << " reads, one-off addedFindingsOnce " << oneOff << " us, checkLadder " << whole << " us");
    }
}

TEST_CASE("an end level is an outline only when it is one cell thick along the line", "[lint][ladder]") {
    // PR #50 round 3 (Codex 1, Claude P3-2): the end-level rule is decided
    // per row and per column, never by a cell's eight neighbours
    const Ramp r = steel();
    // rows y0..y1 of these rungs from column 1, on black
    auto band = [&](int w, int h, int y0, int y1, const std::vector<int>& rungs) {
        Canvas c(w, h);
        for (int y = y0; y <= y1; ++y)
            for (size_t i = 0; i < rungs.size(); ++i)
                c.at(1 + int(i), y) = rungCell(r, rungs[i]);
        return c;
    };
    auto skipAt = [&](const Canvas& c, int x, int y) {
        for (const auto& f : lint::checkLadder(c, r).findings)
            if (f.kind == Kind::RungSkip && f.x == x && f.y == y)
                return true;
        return false;
    };
    SECTION("a dark band two cells deep is the gradient's bottom rung") {
        // Codex's hidden skip: two rows, the second 4 has black above or below
        const Canvas hidden = band(8, 4, 1, 2, {4, 4, 9, 10, 11, 12});
        CHECK(skipAt(hidden, 2, 1));
        CHECK(skipAt(hidden, 2, 2));
        // Claude's bars
        CHECK(skipAt(band(10, 4, 1, 2, {4, 4, 9, 10, 11, 12, 12, 12}), 2, 1));
        CHECK(skipAt(band(13, 4, 1, 2, {4, 4, 4, 4, 4, 4, 9, 10, 11, 12, 12}), 6, 1));
        CHECK(skipAt(band(10, 3, 1, 1, {4, 4, 4, 9, 10, 11, 12, 12}), 3, 1));
        CHECK(skipAt(band(9, 4, 1, 2, {12, 12, 11, 10, 9, 4, 4}), 5, 1));
    }
    SECTION("a staircase filler counts only with the outside on the outer cell's side") {
        // Claude's seam band: rows 1-8, a black seam on row 5 from column 3;
        // the 4s at (2,4) and (2,6) touch the seam diagonally on the inner
        // side, so the two-cell runs on rows 4 and 6 are bands, not a line
        Canvas c = band(12, 10, 1, 8, {4, 4, 9, 10, 11, 12, 12, 12, 12, 12});
        for (int x = 3; x <= 10; ++x)
            c.at(x, 5) = rungCell(r, 0);
        CHECK(skipAt(c, 2, 4));
        CHECK(skipAt(c, 2, 6));
    }
    SECTION("a one-cell end level leaves the plane inside it unjudged") {
        // the broad end plane; Codex's hidden width is the same picture
        CHECK(lint::checkLadder(band(15, 8, 1, 6, {4, 9, 9, 9, 9, 9, 9, 9, 10, 11, 12, 12, 8}), r).clean());
        CHECK(lint::checkLadder(band(14, 8, 1, 6, {4, 9, 9, 9, 9, 9, 9, 9, 9, 10, 11, 12}), r).clean());
    }
    SECTION("a two-cell run is a staircase step only where the staircase goes on") {
        // PR #50 round 4 (Codex 1): a solid 2x2 patch of the line colour is
        // a rung of the gradient, not a rim, however the picture is turned
        auto grid = [&](const std::vector<std::vector<int>>& g, bool flip, bool turn) {
            const int gh = int(g.size()), gw = int(g[0].size());
            Canvas c(turn ? gh : gw, turn ? gw : gh);
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    const int fx = flip ? gw - 1 - x : x;
                    c.at(turn ? y : fx, turn ? fx : y) = rungCell(r, g[size_t(y)][size_t(x)]);
                }
            return c;
        };
        const std::vector<std::vector<int>> patch = {
            {0, 0, 0, 0, 0, 0, 0, 0},     {0, 0, 9, 10, 11, 12, 12, 0}, {0, 4, 4, 9, 10, 11, 12, 0},
            {0, 4, 4, 9, 10, 11, 12, 0},  {0, 0, 9, 10, 11, 12, 12, 0}, {0, 0, 0, 0, 0, 0, 0, 0},
        };
        const std::vector<std::vector<int>> wide = {
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},     {0, 0, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 11, 12, 0},
            {0, 4, 4, 9, 9, 9, 9, 9, 9, 9, 9, 10, 11, 12, 0}, {0, 4, 4, 9, 9, 9, 9, 9, 9, 9, 9, 10, 11, 12, 0},
            {0, 0, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 11, 12, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        };
        CHECK(skipAt(grid(patch, false, false), 2, 2));
        CHECK(skipAt(grid(patch, false, false), 2, 3));
        CHECK(count(lint::checkLadder(grid(wide, false, false), r), Kind::WideRung) == 2);
        for (const bool flip : {false, true})
            for (const bool turn : {false, true}) {
                INFO("mirrored " << flip << ", transposed " << turn);
                CHECK(count(lint::checkLadder(grid(patch, flip, turn), r), Kind::RungSkip) == 2);
                const auto w = lint::checkLadder(grid(wide, flip, turn), r);
                CHECK(count(w, Kind::RungSkip) == 2);
                CHECK(count(w, Kind::WideRung) == 2);
            }
        // Claude round 4 P3-1: rows 1-8 of 4 4 9 10 11 12 12 12 from column 1
        const std::vector<int> bar = {4, 4, 9, 10, 11, 12, 12, 12};
        // one black cell in the outer column: the rows beside it are still a
        // band two deep, each in a 2x2 block of 4
        Canvas notch = band(11, 10, 1, 8, bar);
        notch.at(1, 4) = rungCell(r, 0);
        CHECK(skipAt(notch, 2, 3));
        CHECK(skipAt(notch, 2, 5));
        // every other outer cell black: no 2x2 block, but no staircase either
        Canvas dotted = band(11, 10, 1, 8, bar);
        for (int y = 2; y <= 8; y += 2)
            dotted.at(1, y) = rungCell(r, 0);
        for (int y = 3; y <= 7; y += 2)
            CHECK(skipAt(dotted, 2, y));
        // a one-cell step in the outer edge
        Canvas stepped = band(11, 10, 1, 4, bar);
        for (int y = 5; y <= 8; ++y)
            for (size_t i = 0; i < bar.size(); ++i)
                stepped.at(2 + int(i), y) = rungCell(r, bar[i]);
        CHECK(skipAt(stepped, 2, 4));
        // the corner of a band two deep on a dark mass (a real piece's 4 4 8 12
        // with the outside above the outer cell only): a 2x2 block of 4
        Canvas corner(7, 6);
        corner.at(3, 1) = rungCell(r, 8);
        corner.at(4, 1) = rungCell(r, 8);
        const int rowsOf[3][4] = {{4, 4, 8, 12}, {4, 4, 8, 0}, {4, 4, 4, 4}};
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x)
                corner.at(2 + x, 2 + y) = rungCell(r, rowsOf[y][x]);
        CHECK(skipAt(corner, 3, 2));
        CHECK(skipAt(corner, 4, 2));
    }
    SECTION("a 4-connected diagonal line is one cell thick") {
        // a 45° side: on each row the line's outer cell against black and
        // its inner cell, a staircase filler with black diagonally below
        Canvas c(16, 12);
        for (int y = 1; y <= 10; ++y)
            for (int x = 1; x <= y + 2; ++x)
                c.at(x, y) = rungCell(r, x >= y + 1 ? 4 : std::max(8, 13 - x));
        for (int y = 1; y <= 10; ++y)
            c.at(1, y) = rungCell(r, 4);
        for (int x = 1; x <= 12; ++x)
            c.at(x, 10) = rungCell(r, 4);
        CHECK(count(lint::checkLadder(c, r), Kind::RungSkip) == 0);
    }
    SECTION("a staircase's line may change colour where the level inside it does") {
        // brush-outline review (P3-2): a rim is one line however its colour
        // changes along it (buildings.md: a CYAN rim, LCYAN on the lit arc),
        // so the step at (3,3)-(3,4) goes on as the 4s of the level below
        const Ramp f = *findRamp("fire");
        const std::vector<std::vector<int>> g = {
            {0, 0, 8, 13, 12, 12, 0, 0}, {0, 0, 8, 13, 12, 12, 0, 0}, {0, 0, 8, 12, 12, 12, 0, 0},
            {0, 0, 8, 8, 12, 12, 0, 0},  {0, 0, 0, 8, 4, 11, 0, 0},   {0, 0, 0, 0, 4, 4, 0, 0},
            {0, 0, 0, 0, 0, 0, 0, 0},
        };
        Canvas c(8, 7);
        for (int y = 0; y < 7; ++y)
            for (int x = 0; x < 8; ++x)
                c.at(x, y) = rungCell(f, g[size_t(y)][size_t(x)]);
        CHECK(lint::checkLadder(c, f).clean());
        // a shade there is no line: the step is a band of 8 and a skip
        c.at(4, 4) = rungCell(f, 5);
        bool skip = false;
        for (const auto& x : lint::checkLadder(c, f).findings)
            skip = skip || (x.kind == Kind::RungSkip && x.x == 3 && x.y == 2);
        CHECK(skip);
    }
}

TEST_CASE("detail is the looks per 4x2 window and its flat share", "[lint][detail]") {
    // Measured figures, the detail of funkpower's pieces in
    // tests/data/golden.
    struct Case {
        const char* piece;
        std::optional<lint::Box> box;
        double mean;
        int flat;
    };
    for (const Case& k : {Case{"darkmage.ans", {}, 3.3, 9}, Case{"darkmage.ans", lint::Box{10, 5, 40, 20}, 3.29, 9},
                          Case{"cafe-mug.ans", {}, 2.58, 18}, Case{"cafe-mug.ans", lint::Box{20, 10, 60, 30}, 2.42, 17},
                          Case{"outrun-sunset.ans", {}, 2.52, 8},
                          Case{"outrun-sunset.ans", lint::Box{0, 0, 79, 15}, 2.58, 4},
                          Case{"durin-gate.ans", {}, 2.78, 21},
                          Case{"durin-gate.ans", lint::Box{5, 3, 50, 25}, 3.12, 6}}) {
        const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / k.piece));
        const lint::Detail d = lint::detail(doc.canvas, k.box);
        INFO(k.piece << (k.box ? " in a box" : "") << ": " << d.mean << ", " << d.flatPercent << "% flat of "
                     << d.windows);
        CHECK(std::abs(d.mean - k.mean) < 0.005);
        CHECK(d.flatPercent == k.flat);
    }
}

TEST_CASE("detail skips windows that are cut short or mostly black", "[lint][detail]") {
    Canvas c(9, 3);  // all black: no window judged
    CHECK(lint::detail(c).windows == 0);
    CHECK(lint::detail(c).mean == 0.0);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 9; ++x)
            c.at(x, y).bg = Color::indexed(Blue);
    // two whole windows (columns 0-3 and 4-7 of rows 0-1), both flat
    auto d = lint::detail(c);
    CHECK(d.windows == 2);
    CHECK(d.flat == 2);
    CHECK(d.flatPercent == 100);
    c.at(1, 1).ch = U'░';
    c.at(1, 1).fg = Color::indexed(LightBlue);
    c.at(6, 0).ch = kFullBlock;
    c.at(6, 0).fg = Color::indexed(Blue);  // a full block looks like its colour
    d = lint::detail(c);
    CHECK(d.mean == 1.5);
    CHECK(d.flatPercent == 50);
}

// LadderIndex::wideRungs reads one row or column of the held canvas: the
// wide rungs checkLadder finds along it, every field, in order, as the index
// is built and after changes (bands, squares and ladders, which have runs
// both ways).
TEST_CASE("an index's wide rungs along a line are checkLadder's on that line", "[lint][ladder]") {
    std::mt19937 rng(2026102321);
    auto pick = [&](int lo, int hi) { return bwtest::uniformInt(rng, lo, hi); };
    const Ramp r = steel();
    int lines = 0, found = 0;
    for (int k = 0; k < 300; ++k) {
        const int w = pick(1, 30), h = pick(1, 20), source = pick(0, 2);
        Canvas canvas = source == 0   ? randomLadder(r, w, h, rng)
                        : source == 1 ? randomSquares(r, w, h, rng)
                                      : randomBands(r, w, h, rng);
        lint::LadderIndex index(canvas, r);
        for (int step = 0; step < 3; ++step) {
            const auto all = lint::checkLadder(canvas, r).findings;
            for (const bool across : {true, false})
                for (int line = 0; line < (across ? h : w); ++line) {
                    std::vector<lint::LadderFinding> want;
                    for (const auto& f : all)
                        if (f.kind == Kind::WideRung && (across ? f.y == line && f.ny == line && f.nx != f.x
                                                                : f.x == line && f.nx == line && f.ny != f.y))
                            want.push_back(f);
                    const auto got = index.wideRungs(across, line);
                    INFO("case " << k << " step " << step << (across ? " row " : " column ") << line);
                    REQUIRE(keysOf(got) == keysOf(want));
                    REQUIRE(got.size() == want.size());
                    for (size_t i = 1; i < got.size(); ++i)
                        REQUIRE((across ? got[i - 1].x < got[i].x : got[i - 1].y < got[i].y));
                    ++lines;
                    found += int(got.size());
                }
            // a patch moved a rung or two
            Canvas after = canvas;
            std::vector<std::pair<int, int>> changed;
            const int x0 = pick(0, w - 1), y0 = pick(0, h - 1);
            for (int y = y0; y < std::min(h, y0 + pick(1, 3)); ++y)
                for (int x = x0; x < std::min(w, x0 + pick(1, 6)); ++x) {
                    const auto g = rungOf(r, canvas.at(x, y));
                    after.at(x, y) = rungCell(r, std::clamp(g.value_or(0) + pick(-2, 2), 0, r.topRung()));
                    changed.push_back({x, y});
                }
            (void)index.apply(after, changed);
            canvas = after;
        }
    }
    INFO(lines << " lines, " << found << " wide rungs");
    CHECK(found > 200);
}

// ---- the gradient's dither (#160) ------------------------------------------

namespace {

// The steel ramp's whole ladder across 60 columns, Bayer-dithered.
Canvas ditheredFill() {
    const Ramp r = steel();
    const Canvas blank(60, 8);
    Gradient g;
    g.x1 = 59, g.toRung = r.topRung(), g.banding = GradientBanding::Bayer;
    Canvas c = blank;
    rampGradient(blank, r, g, StrokeOptions{}).apply(c);
    return c;
}

bool speckAt(const std::vector<lint::LadderFinding>& found, int x, int y) {
    return std::any_of(found.begin(), found.end(),
                       [&](const lint::LadderFinding& f) { return f.kind == Kind::Speckle && f.x == x && f.y == y; });
}

} // namespace

TEST_CASE("a dithered cell is exempt only while every ladder neighbour is a rung from it", "[lint][ladder][dither]") {
    CHECK(lint::ditherExempt(true, 5, {4, 4, 4, 4}));
    CHECK(lint::ditherExempt(true, 5, {4, 6, -1, 4}));  // off the ladder or the canvas doesn't count
    CHECK(lint::ditherExempt(true, 5, {-1, -1, -1, -1}));
    CHECK_FALSE(lint::ditherExempt(false, 5, {4, 4, 4, 4}));  // no mark: a speck like any other
    CHECK_FALSE(lint::ditherExempt(true, 5, {4, 4, 4, 3}));   // a neighbour two off
    CHECK_FALSE(lint::ditherExempt(true, 5, {4, 4, 5, 4}));   // one of its own rung
    CHECK_FALSE(lint::ditherExempt(true, -1, {4, 4, 4, 4}));
}

TEST_CASE("the speckle rule leaves the gradient's dither alone and still finds a lone speck in it or beside it",
          "[lint][ladder][dither]") {
    const Ramp r = steel();
    const Canvas c = ditheredFill();
    REQUIRE(lint::checkLadder(c, r).clean());

    // the same cells without their marks: the dither's cells are specks,
    // and only they are
    Canvas bare = c;
    std::vector<std::pair<int, int>> marked;
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (c.at(x, y).dithered) {
                marked.emplace_back(x, y);
                bare.at(x, y).dithered = false;
            }
    const lint::LadderReport bareReport = lint::checkLadder(bare, r);
    CHECK(count(bareReport, Kind::Speckle) > 0);
    for (const auto& f : bareReport.findings)
        if (f.kind == Kind::Speckle)
            CHECK(std::find(marked.begin(), marked.end(), std::pair{f.x, f.y}) != marked.end());

    // a lone cell by hand in the middle of a plateau, and one beside two
    // dithered cells: both specks, the dither still none
    REQUIRE(c.at(16, 0).dithered);
    REQUIRE(c.at(17, 1).dithered);
    REQUIRE_FALSE(c.at(17, 0).dithered);
    REQUIRE(rungOf(r, c.at(38, 1)) == 8);
    Canvas after = c;
    after.at(38, 1) = rungCell(r, 9);
    after.at(17, 0) = rungCell(r, 5);
    const lint::LadderReport report = lint::checkLadder(after, r);
    CHECK(speckAt(report.findings, 38, 1));
    CHECK(speckAt(report.findings, 17, 0));
    CHECK(count(report, Kind::Speckle) == 2);

    // the incremental index agrees, and says what the edit adds
    lint::LadderIndex index(c, r);
    CHECK(index.findings().empty());
    const std::vector<lint::LadderFinding> added = index.apply(after, {{38, 1}, {17, 0}});
    CHECK(speckAt(added, 38, 1));
    CHECK(speckAt(added, 17, 0));
    CHECK(index.findings() == report.findings);

    // a dithered cell drawn over in its own look but unmarked (a copy by
    // hand of the dither) is a speck again
    const auto [mx, my] = marked.front();
    Canvas copied = c;
    copied.at(mx, my).dithered = false;
    lint::LadderIndex again(c, r);
    const bool wasSpeck = speckAt(bareReport.findings, mx, my);
    CHECK(speckAt(again.apply(copied, {{mx, my}}), mx, my) == wasSpeck);
    CHECK(again.findings() == lint::checkLadder(copied, r).findings);
}
