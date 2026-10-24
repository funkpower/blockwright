// The IRC view drawn (irc_render.h): the cells as renderCanvas draws them,
// and the cut mark on rows over the budget.

#include "util.h"

#include "blockwright/irc_render.h"

#include <catch2/catch_test_macros.hpp>

using namespace bw;

namespace {

// Every cell a new colour pair, so the row's line runs about 7 bytes a cell.
Canvas colourful(int w) {
    Canvas c(w, 2);
    for (int x = 0; x < w; ++x) {
        c.at(x, 0) = Cell{U'▀', Color::indexed(std::uint8_t(x % 16)), Color::indexed(std::uint8_t((x + 5) % 16)), 0, false};
        c.at(x, 1) = Cell{U'█', Color::indexed(Grey), Color::indexed(Black), 0, false};
    }
    return c;
}

int marks(const QImage& img, int py) {
    int n = 0;
    for (int x = 0; x < img.width(); ++x)
        n += img.pixel(x, py) == kIrcCutMark;
    return n;
}

} // namespace

TEST_CASE("the IRC view draws its cells and marks where a cut row ends", "[irc][render]") {
    const Canvas c = colourful(40);
    const irc::View v = irc::ircView(c, 60);
    REQUIRE(v.rows[0].cut());
    REQUIRE_FALSE(v.rows[1].cut());
    const int shown = v.rows[0].shown;
    REQUIRE(shown > 0);
    REQUIRE(shown < 40);

    for (const int scale : {1, 2}) {
        INFO("scale " << scale);
        const QImage img = renderIrcView(v, 8, scale);
        const QImage plain = renderCanvas(v.canvas, 8, scale);
        const int gw = 8 * scale, gh = vgaFont().height * scale;
        REQUIRE(img.size() == plain.size());
        CHECK(img.width() == 40 * gw);
        CHECK(img.height() == 2 * gh);
        // a quarter glyph at the first lost cell, down the whole row
        for (int py = 0; py < gh; ++py) {
            CHECK(marks(img, py) == gw / 4);
            CHECK(img.pixel(shown * gw, py) == kIrcCutMark);
            CHECK(img.pixel(shown * gw + gw / 4 - 1, py) == kIrcCutMark);
            CHECK(img.pixel(shown * gw + gw / 4, py) == plain.pixel(shown * gw + gw / 4, py));
        }
        // the row that fits has no mark, and is the canvas as drawn
        for (int py = gh; py < 2 * gh; ++py) {
            CHECK(marks(img, py) == 0);
            for (int x = 0; x < img.width(); x += 7)
                CHECK(img.pixel(x, py) == plain.pixel(x, py));
        }
    }
}

TEST_CASE("a view that fits has no cut mark", "[irc][render]") {
    const Canvas c = colourful(40);
    const irc::View v = irc::ircView(c);
    REQUIRE(v.cutRows() == 0);
    const QImage img = renderIrcView(v, 9);
    CHECK(img.width() == 40 * 9);
    for (int py = 0; py < img.height(); ++py)
        CHECK(marks(img, py) == 0);
}
