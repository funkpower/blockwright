// The IRC view drawn (irc_render.h): the cells as renderCanvas draws them,
// the cut mark on rows over the budget, and a formatted cell drawn as its
// encoding reads back.

#include "util.h"

#include "blockwright/irc_render.h"
#include "blockwright/irc_view.h"
#include "blockwright/mirc.h"
#include "blockwright/render.h"

#include <catch2/catch_test_macros.hpp>

using namespace bw;
using irc::encodeMircRow;
using irc::kMircRgb;
using irc::MircCell;

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

namespace {

// mIRC cells as a client draws them: each colour its 24-bit value, the
// client's own (99) its foreground or background.
Canvas shownCanvas(const std::vector<MircCell>& row) {
    const auto colour = [](int n, std::uint32_t client) {
        const std::uint32_t v = n == irc::kClientColour ? client : kMircRgb[std::size_t(n)];
        return Color::fromRgb({std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)});
    };
    Canvas c(int(row.size()), 1);
    for (std::size_t i = 0; i < row.size(); ++i) {
        const MircCell& m = row[i];
        c.at(int(i), 0) =
            Cell{m.ch, colour(m.fg, irc::kClientForeground), colour(m.bg, irc::kClientBackground), m.attrs, false};
    }
    return c;
}

} // namespace

TEST_CASE("a formatted cell's IRC encoding draws the pixels the cell draws", "[irc][attrs][render]") {
    // #360 review: italic shears a full block over its background, and bold
    // widens a half block, so neither may drop a colour or swap the half
    const char32_t glyphs[] = {U'█', U'▀', U'▄', U'▌', U'▐', U'░', U'▒', U'▓', U' ', U'A'};
    const std::uint8_t formats[] = {attr::Bold, attr::Italic, attr::Underline, attr::Bold | attr::Italic,
                                    attr::Bold | attr::Italic | attr::Underline};
    const std::pair<std::uint8_t, std::uint8_t> colours[] = {{4, 2}, {4, 4}, {1, 1}};
    for (const bool swapShades : {false, true})
        for (const char32_t g : glyphs)
            for (const std::uint8_t a : formats)
                for (const auto& [fg, bg] : colours)
                    for (const bool after : {false, true}) {
                        // alone, and after a cell in other colours
                        std::vector<MircCell> row;
                        if (after)
                            row.push_back(MircCell{U'x', 7, 6, 0});
                        row.push_back(MircCell{g, fg, bg, a});
                        const std::string line = encodeMircRow(row, swapShades);
                        std::vector<MircCell> shown;
                        for (const irc::ShownCell& s : irc::decodeLine(line))
                            shown.push_back(s.cell);
                        INFO("U+" << std::hex << std::uint32_t(g) << std::dec << " attrs " << int(a) << " colours "
                                  << int(fg) << "," << int(bg) << " swap " << swapShades << " after " << after);
                        REQUIRE(shown.size() == row.size());
                        for (const int gw : {8, 9})
                            CHECK(renderCanvas(shownCanvas(shown), gw) == renderCanvas(shownCanvas(row), gw));
                    }
}
