// The IRC view (irc_view.h): lines read back as a client reads mIRC codes,
// what IRC drops, and each line cut at the byte budget.

#include "blockwright/ansi.h"
#include "blockwright/halfblock.h"
#include "blockwright/irc_view.h"
#include "blockwright/ramp.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace bw;
using namespace bw::irc;

namespace {

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

// What a cell shows, whichever of the glyphs that draw it the encoder
// picked: half blocks, full blocks and spaces as their top and bottom
// colours, ▌ ▐ as their left and right ones, anything else as itself.
std::tuple<char32_t, int, int> shown(char32_t ch, int fg, int bg) {
    switch (ch) {
    case U' ': return {U'▀', bg, bg};
    case U'█': return {U'▀', fg, fg};
    case U'▀': return {U'▀', fg, bg};
    case U'▄': return {U'▀', bg, fg};
    case U'▌': return {U'▌', fg, bg};
    case U'▐': return {U'▌', bg, fg};
    default: return {ch, fg, bg};
    }
}

// The view's colours are all RGB.
std::uint32_t rgb(const Color& c) {
    REQUIRE_FALSE(c.isIndexed());
    const Rgb v = c.rgb;
    return std::uint32_t(v.r) << 16 | std::uint32_t(v.g) << 8 | v.b;
}

} // namespace

TEST_CASE("a line is read as a client reads mIRC codes", "[irc][view]") {
    auto line = decodeLine("\x03" "04,02A\x03" "B\x03" "5,X\x03" "123\x16R\x0fZ\x02\x1f!");
    REQUIRE(line.size() == 8);
    CHECK((line[0].cell.ch == U'A' && line[0].cell.fg == 4 && line[0].cell.bg == 2));
    // ^C alone resets both colours
    CHECK((line[1].cell.ch == U'B' && line[1].cell.fg == kClientColour && line[1].cell.bg == kClientColour));
    // a comma with no digit after it is a glyph, and the background stays
    CHECK((line[2].cell.ch == U',' && line[2].cell.fg == 5 && line[2].cell.bg == kClientColour));
    CHECK(line[3].cell.ch == U'X');
    // two digits at most: 12, then a '3'
    CHECK((line[4].cell.ch == U'3' && line[4].cell.fg == 12));
    // ^V swaps the colours
    CHECK((line[5].cell.ch == U'R' && line[5].cell.fg == kClientColour && line[5].cell.bg == 12));
    // ^O resets them; ^B and ^_ change no colour
    CHECK((line[6].cell.ch == U'Z' && line[6].cell.fg == kClientColour && line[6].cell.bg == kClientColour));
    CHECK(line[7].cell.ch == U'!');
    // each glyph's end, in bytes
    CHECK(line[0].end == 7);
    CHECK(line[7].end == 24);

    // UTF-8: a glyph ends after its last byte; a bad byte is one U+FFFD
    line = decodeLine("é▀\xff" "a\xe2\x96");
    REQUIRE(line.size() == 6);
    CHECK((line[0].cell.ch == U'é' && line[0].end == 2));
    CHECK((line[1].cell.ch == U'▀' && line[1].end == 5));
    CHECK((line[2].cell.ch == U'�' && line[2].end == 6));
    CHECK((line[3].cell.ch == U'a' && line[3].end == 7));
    // a cut-off sequence: its lead byte, then the stray continuation byte
    CHECK((line[4].cell.ch == U'�' && line[4].end == 8));
    CHECK((line[5].cell.ch == U'�' && line[5].end == 9));
    // an overlong form isn't a glyph
    CHECK(decodeLine("\xc0\xaf").size() == 2);
    // colour 99 is the client's own
    CHECK(decodeLine("\x03" "99,99a").front().cell.fg == kClientColour);
    CHECK(decodeLine("").empty());
}

TEST_CASE("an encoded row reads back as the cells it was made from", "[irc][view]") {
    const Canvas c = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "cafe-mug.ans")).canvas;
    const auto lines = encodeCanvas(c);
    int rowsChecked = 0;
    for (int y = 0; y < c.height(); ++y) {
        const auto cells = decodeLine(lines[std::size_t(y)]);
        INFO("row " << y);
        REQUIRE(int(cells.size()) <= c.width());
        for (int x = 0; x < int(cells.size()); ++x) {
            const Cell& src = c.at(x, y);
            const MircCell& got = cells[std::size_t(x)].cell;
            const auto want = shown(src.ch == U'\0' ? U' ' : src.ch.codePoint().value(), mircColor(src.fg), mircColor(src.bg));
            const auto have = shown(got.ch.codePoint().value(), got.fg, got.bg);
            // a solid cell shows one colour, whatever its other one is
            if (std::get<1>(want) == std::get<2>(want) && std::get<0>(want) == U'▀')
                CHECK((std::get<1>(have) == std::get<1>(want) && std::get<2>(have) == std::get<2>(want)));
            else
                CHECK(have == want);
        }
        // the rest are the trailing black spaces the encoder drops
        for (int x = int(cells.size()); x < c.width(); ++x)
            CHECK(mircColor(c.at(x, y).bg) == 1);
        rowsChecked += !cells.empty();
    }
    CHECK(rowsChecked > 30);
}

TEST_CASE("the view shows the mIRC palette and the client's background where IRC drops cells", "[irc][view]") {
    Canvas c = filled(6, 4, cell(U' ', Grey, Black));
    c.at(0, 1) = cell(kFullBlock, White, Black);
    for (int x = 0; x < 6; ++x)
        c.at(x, 2) = cell(kUpperHalf, LightRed, Blue);
    const View v = ircView(c);
    REQUIRE(v.canvas.width() == 6);
    REQUIRE(v.canvas.height() == 4);
    REQUIRE(v.rows.size() == 4);

    CHECK(v.rows[0].trimmed);
    CHECK(v.rows[0].kept == 0);
    CHECK_FALSE(v.rows[1].trimmed);
    CHECK(v.rows[1].kept == 1);
    CHECK(v.rows[2].kept == 6);
    CHECK(v.rows[3].trimmed);
    CHECK(v.cutRows() == 0);

    // White is mIRC 0
    const Cell& w = v.canvas.at(0, 1);
    CHECK(rgb(w.ch == kFullBlock ? w.fg : w.bg) == kMircRgb[0]);
    // the dropped black spaces and the trimmed rows show the client's background
    for (const auto& [x, y] : {std::pair{1, 1}, std::pair{5, 1}, std::pair{0, 0}, std::pair{3, 3}}) {
        INFO(x << "," << y);
        CHECK(v.canvas.at(x, y).ch == U' ');
        CHECK(rgb(v.canvas.at(x, y).bg) == kClientBackground);
    }
    // ▀ light red on blue: mIRC 4 over 2
    const auto px = halfBlocks(c.at(2, 2));
    REQUIRE(px);
    const Cell& h = v.canvas.at(2, 2);
    const auto [ch, top, bottom] = shown(h.ch.codePoint().value(), int(rgb(h.fg)), int(rgb(h.bg)));
    CHECK(ch == U'▀');
    CHECK(std::uint32_t(top) == kMircRgb[4]);
    CHECK(std::uint32_t(bottom) == kMircRgb[2]);
    // nothing is in palette colours: the view is what a client shows
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x)
            CHECK_FALSE(v.canvas.at(x, y).bg.isIndexed());
}

TEST_CASE("a line over the budget loses the cells past it", "[irc][view]") {
    // every cell a new colour pair: about 7 bytes a cell
    Canvas c(40, 2);
    for (int x = 0; x < 40; ++x) {
        c.at(x, 0) = cell(kUpperHalf, std::uint8_t(x % 16), std::uint8_t((x + 5) % 16));
        c.at(x, 1) = cell(kFullBlock, Grey, Black);
    }
    const auto lines = encodeCanvas(c);
    const std::size_t budget = 60;
    REQUIRE(lines[0].size() > budget);
    REQUIRE(lines[1].size() <= budget);
    const View v = ircView(c, budget);
    CHECK(v.cutRows() == 1);
    CHECK(v.rows[0].cut());
    CHECK_FALSE(v.rows[1].cut());
    CHECK(v.rows[0].bytes == lines[0].size());
    CHECK(v.rows[0].kept == 40);
    // ascii.fit(line, 60) re-encodes the 310-byte line and keeps 54 bytes
    // that carry 6 cells
    const int within = 6;
    CHECK(v.rows[0].shown == within);
    for (int x = within; x < 40; ++x)
        CHECK(rgb(v.canvas.at(x, 0).bg) == kClientBackground);
    CHECK(rgb(v.canvas.at(within - 1, 0).bg) != kClientBackground);
    // the least budget the script sends the line whole in shows it whole
    const std::size_t whole = txtSentBytes(lines[0], 0);
    CHECK(ircView(c, whole).cutRows() == 0);
    CHECK(ircView(c, whole - 1).cutRows() == 1);
}

TEST_CASE("a cut line keeps a byte for the reset after it, as the sender's fit does", "[irc][view]") {
    // ten Ç (two bytes each in UTF-8), then 382 X, on cyan: 392 cells in a
    // 408-byte line, "\x0315,10" and the glyphs
    Canvas c(392, 1);
    for (int x = 0; x < 392; ++x)
        c.at(x, 0) = cell(x < 10 ? U'Ç' : U'X', Grey, Cyan);
    const auto lines = encodeCanvas(c);
    REQUIRE(lines[0].size() == 408);
    // ascii.fit(line, 407) keeps 406 bytes, the code and 390 cells, and
    // leaves the last byte for the reset it adds after a space
    const View v = ircView(c, 407);
    CHECK(v.rows[0].cut());
    CHECK(v.rows[0].shown == 390);
    CHECK(rgb(v.canvas.at(390, 0).bg) == kClientBackground);
    // a line that fits is sent whole, to its last byte
    CHECK(ircView(c, 408).rows[0].shown == 392);
    CHECK(ircView(c, 406).rows[0].shown == 389);
}

TEST_CASE("a line over the budget is cut as the sender cuts its own encoding of it", "[irc][view]") {
    // twenty times four half blocks, each pair of colours new: the export
    // writes one-digit codes ("\x03" "4,2"), the sender's encoding two
    // ("\x03" "04,02"), so it carries fewer cells in the same bytes
    const std::uint8_t pairs[4][2] = {{LightRed, Blue}, {Yellow, Green}, {White, Magenta}, {LightCyan, Red}};
    Canvas c(80, 1);
    for (int x = 0; x < 80; ++x)
        c.at(x, 0) = cell(kUpperHalf, pairs[x % 4][0], pairs[x % 4][1]);
    const auto lines = encodeCanvas(c);
    REQUIRE(lines[0].size() == 580);
    // the export's own glyphs: 56 end within 406 bytes
    const auto cells = decodeLine(lines[0]);
    REQUIRE(cells[55].end <= 406);
    REQUIRE(cells[56].end > 406);
    // ascii.fit(line, 407) sends 405 bytes that carry 45 cells
    const View v = ircView(c, 407);
    CHECK(v.rows[0].cut());
    CHECK(v.rows[0].kept == 80);
    CHECK(v.rows[0].bytes == 580);
    CHECK(v.rows[0].shown == 45);
    CHECK(rgb(v.canvas.at(44, 0).bg) != kClientBackground);
    for (int x = 45; x < 80; ++x)
        CHECK(rgb(v.canvas.at(x, 0).bg) == kClientBackground);
    // the cells shown are the export's: ▀ in its own colours
    CHECK(v.canvas.at(44, 0).ch == kUpperHalf);
    CHECK(rgb(v.canvas.at(0, 0).fg) == kMircRgb[4]);
    CHECK(rgb(v.canvas.at(0, 0).bg) == kMircRgb[2]);
    // a budget the export fits is not one the script sends it whole in: it
    // writes the line again with two-digit codes first
    CHECK(ircView(c, 580).rows[0].cut());
    const std::size_t whole = txtSentBytes(lines[0], 0);
    CHECK(whole > 580);
    CHECK(ircView(c, whole).rows[0].shown == 80);
    CHECK(ircView(c, whole - 1).rows[0].cut());
}

TEST_CASE("a padded line over the budget whose sender encoding fits is sent whole", "[irc][view]") {
    // '1' grey on black, then a black space: padded, the export ends the
    // row on a black-on-black block, "\x03" "15,01" "1" "\x03" "1" "█", 12
    // bytes; the sender draws that cell as a space, "\x03" "15,01" "1 " and
    // the reset after it, 9 bytes
    Canvas c(2, 1);
    c.at(0, 0) = cell(U'1', Grey, Black);
    c.at(1, 0) = cell(U' ', Grey, Black);
    EncodeOptions pad;
    pad.pad = true;
    const auto lines = encodeCanvas(c, pad);
    REQUIRE(lines[0] == "\x03" "15,01" "1" "\x03" "1" "█");
    REQUIRE(lines[0].size() == 12);
    // ascii.fit(line, 9) sends "\x03" "15,01" "1 " "\x0f", both cells, uncut
    const View v = ircView(c, 9, pad);
    CHECK(v.rows[0].bytes == 12);
    CHECK(v.rows[0].kept == 2);
    CHECK(v.rows[0].shown == 2);
    CHECK_FALSE(v.rows[0].cut());
    CHECK(v.cutRows() == 0);
    // the cells as the export draws them: the block, not the sender's space
    CHECK(v.canvas.at(1, 0).ch == kFullBlock);
    CHECK(rgb(v.canvas.at(1, 0).bg) != kClientBackground);
    // ascii.fit(line, 8) has to cut the sender's 8 bytes: the '1' alone
    const View t = ircView(c, 8, pad);
    CHECK(t.rows[0].cut());
    CHECK(t.rows[0].shown == 1);
    CHECK(rgb(t.canvas.at(1, 0).bg) == kClientBackground);
}

TEST_CASE("a 120-row piece fits the EFnet budget, and the view agrees with the line sizes", "[irc][view][golden]") {
    const Canvas c = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "cocktober.ans")).canvas;
    REQUIRE(c.height() == 120);
    const auto exported = exportCanvas(c);
    for (const auto& l : exported)
        CHECK(l.size() <= kEfnetWeechatBudget);
    CHECK(txtOverBudget(exported, kEfnetWeechatBudget).empty());
    const View v = ircView(c);
    CHECK(v.cutRows() == 0);

    // under a tighter budget the rows the view cuts are the rows the
    // script sends over it
    const auto lines = encodeCanvas(c);
    for (const std::size_t budget : {std::size_t(150), std::size_t(300)}) {
        const View t = ircView(c, budget);
        std::set<int> over;
        for (const auto& o : txtOverBudget(lines, budget))
            over.insert(o.line - 1);
        CHECK(t.cutRows() > 0);
        for (int y = 0; y < c.height(); ++y) {
            INFO("budget " << budget << ", row " << y);
            CHECK(t.rows[std::size_t(y)].bytes == lines[std::size_t(y)].size());
            CHECK(t.rows[std::size_t(y)].cut() == (over.count(y) == 1));
        }
    }
}

TEST_CASE("a cell is sent as it shows with the document's iCE setting", "[irc][view]") {
    // iCE off: a bright background is blink on its dark colour, and IRC
    // shows the dark colour (#301)
    const Cell bright = cell(U' ', White, LightBlue);
    const Cell off = sentCell(bright, false);
    CHECK(off.bg == Color::indexed(Blue));
    CHECK(off.attrs & attr::Blink);
    CHECK(sentCell(bright, true) == bright);
    // iCE on: blink is a bright background
    Cell blinking = cell(U' ', White, Blue);
    blinking.attrs |= attr::Blink;
    const Cell on = sentCell(blinking, true);
    CHECK(on.bg == Color::indexed(LightBlue));
    CHECK_FALSE(on.attrs & attr::Blink);
    CHECK(sentCell(blinking, false) == blinking);
    // a transparent cell is left alone
    Cell clear = bright;
    clear.transparent = true;
    CHECK(sentCell(clear, false) == clear);
    CHECK(sentCell(clear, true) == clear);

    Canvas canvas(4, 1);
    for (int x = 0; x < 4; ++x)
        canvas.at(x, 0) = bright;
    canvas.at(3, 0) = clear;
    const Canvas sent = sentCanvas(canvas, false);
    for (int x = 0; x < 4; ++x)
        CHECK(sent.at(x, 0) == sentCell(canvas.at(x, 0), false));
    CHECK(sentCanvas(canvas, true).at(0, 0) == bright);

    // the view of the sent canvas shows mIRC 2 where the canvas has 12
    CHECK(rgb(ircView(sent).canvas.at(0, 0).bg) == kMircRgb[2]);
    CHECK(rgb(ircView(canvas).canvas.at(0, 0).bg) == kMircRgb[12]);
    CHECK(encodeCanvas(sent) == encodeCanvas(sentCanvas(sent, false)));
}

TEST_CASE("stars written within the budget are cut as the script sends them (#364)", "[irc][view]") {
    // ★ in bright red and red by turns, on black: the encoder writes each
    // star's colour in one digit ("\x03" "4,1★"), the ascii script in two,
    // and keeps the 67 stars that end within 406 bytes of its own line
    const auto stars = [](int n) {
        Canvas c(80, 1);
        for (int x = 0; x < n; ++x)
            c.at(x, 0) = cell(U'★', x % 2 ? Red : LightRed, Black);
        return c;
    };
    const Canvas c80 = stars(80);
    const auto lines = encodeCanvas(c80);
    REQUIRE(lines[0].size() == 402);
    REQUIRE(lines[0].substr(0, 9) == "\x03" "4,1★\x03" "5");
    REQUIRE(txtSentBytes(lines[0], 407) == 483);
    const View v80 = ircView(c80, 407);
    CHECK(v80.rows[0].bytes == 402);
    CHECK(v80.rows[0].kept == 80);
    CHECK(v80.rows[0].shown == 67);
    CHECK(v80.rows[0].cut());
    CHECK(v80.canvas.at(66, 0).ch == U'★');
    CHECK(rgb(v80.canvas.at(67, 0).bg) == kClientBackground);
    // 68 stars: 342 bytes written, 411 sent, cut to the same 67
    const Canvas c68 = stars(68);
    REQUIRE(encodeCanvas(c68)[0].size() == 342);
    const View v68 = ircView(c68, 407);
    CHECK(v68.rows[0].shown == 67);
    CHECK(v68.rows[0].cut());
    // 67 stars: 337 written, 405 sent, whole
    const Canvas c67 = stars(67);
    REQUIRE(encodeCanvas(c67)[0].size() == 337);
    CHECK(txtSentBytes(encodeCanvas(c67)[0], 407) == 405);
    const View v67 = ircView(c67, 407);
    CHECK(v67.rows[0].shown == 67);
    CHECK_FALSE(v67.rows[0].cut());
}

TEST_CASE("a row cut past the view's width is cut (#364)", "[irc][view]") {
    // 80 lone wide 中, no continuations, bright red and red by turns on
    // black: 402 bytes written, 483 sent, and the script keeps 67 of them.
    // Those lay out in 134 columns, so all 80 the view shows are carried,
    // but the line is cut all the same
    Canvas c(80, 1);
    for (int x = 0; x < 80; ++x)
        c.at(x, 0) = cell(U'中', x % 2 ? Red : LightRed, Black);
    const auto lines = encodeCanvas(c);
    REQUIRE(lines[0].size() == 402);
    REQUIRE(txtSentBytes(lines[0], 407) == 483);
    const View v = ircView(c, 407);
    CHECK(v.rows[0].shown == 80);
    CHECK(v.rows[0].lost);
    CHECK(v.rows[0].cut());
    CHECK(v.cutRows() == 1);
    // with room for the whole line it is not
    const View whole = ircView(c, 483);
    CHECK_FALSE(whole.rows[0].lost);
    CHECK(whole.cutRows() == 0);
}
