// IRC text from the canvas (irc.h).

#include "util.h"

#include "blockwright/cp437.h"
#include "blockwright/irc.h"
#include "blockwright/irc_view.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <tuple>

using namespace bw;
namespace fs = std::filesystem;

namespace {

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        out.push_back(text.substr(at, nl - at));
        at = nl == std::string::npos ? text.size() : nl + 1;
    }
    return out;
}

} // namespace

TEST_CASE("palette colours map to mIRC colours of the same name and shade") {
    // CGA order: the dark eight, then the bright eight
    const std::uint8_t want[16] = {1, 2, 3, 10, 5, 6, 7, 15, 14, 12, 9, 11, 4, 13, 8, 0};
    for (std::uint8_t i = 0; i < 16; ++i)
        CHECK(irc::mircColor(Color::indexed(i)) == want[i]);
}

TEST_CASE("RGB colours go to the nearest of mIRC's 99") {
    CHECK(irc::mircColor(Color::fromRgb({0x00, 0x00, 0xff})) == 60);
    CHECK(irc::mircColor(Color::fromRgb({0xff, 0x00, 0x00})) == 4);
    CHECK(irc::mircColor(Color::fromRgb({0x13, 0x13, 0x13})) == 89);
}

TEST_CASE("RGB cells reach the 99-colour mapping") {
    Canvas c(1, 1);
    c.at(0, 0).ch = U'X';
    c.at(0, 0).fg = Color::fromRgb({0, 128, 255});
    c.at(0, 0).bg = Color::fromRgb({0x13, 0x13, 0x13});
    CHECK(irc::exportCanvas(c) == std::vector<std::string>{"\x03" "59,89X"});
}

// The parser quantises 256-colour and 24-bit SGR colours to the 16-colour
// palette (PLAN §8), so they export as palette colours, not as the nearest of
// mIRC's 99 (53 and 59 here).
TEST_CASE("256-colour and 24-bit input exports with its 16-colour quantisation") {
    CHECK(irc::exportCanvas(parseAnsi("\x1b[38;5;208mX\r\n").canvas) == std::vector<std::string>{"\x03" "7,1X"});
    CHECK(irc::exportCanvas(parseAnsi("\x1b[38;2;0;128;255mX\r\n").canvas) ==
          std::vector<std::string>{"\x03" "10,1X"});
}

TEST_CASE("encodeRow draws palette cells through the row encoder") {
    // the row encoder's own cases (test_mirc.cpp), with the mIRC colours given
    // as palette colours
    // red on black, then 7 in light green on black: one digit unless a digit follows
    CHECK(irc::encodeRow(std::vector{cell(U'a', LightRed, Black), cell(U'7', LightGreen, Black)}) ==
          "\x03" "4,1a\x03" "097");
    // a red block between red-background spaces needs no colour change
    CHECK(irc::encodeRow(std::vector{cell(U' ', Grey, LightRed), cell(U'█', LightRed, Black),
                                     cell(U' ', Grey, LightRed)}) == "\x03" "4,4   \x0f");
    // a top half block is a bottom half block with the colours swapped
    CHECK(irc::encodeRow(std::vector{cell(U'▀', LightRed, Blue), cell(U'▄', Blue, LightRed),
                                     cell(U'▀', LightRed, Blue)}) == "\x03" "2,4▄▄▄");
    CHECK(irc::encodeRow(std::vector<Cell>{}).empty());
}

TEST_CASE("encodeRow writes a non-scalar code point as '?'") {
    CHECK(irc::encodeRow(std::vector{cell(char32_t(0xd800), Grey, Black)}) ==
          irc::encodeRow(std::vector{cell(U'?', Grey, Black)}));
    // a value past U+10FFFF never reaches a cell: a glyph made from one is
    // U+FFFD (PLAN §16, Storage), which is written as itself
    CHECK(cell(char32_t(0x110000), Grey, Black).ch == U'\uFFFD');
    CHECK(irc::encodeRow(std::vector{cell(char32_t(0x110000), Grey, Black)}) ==
          irc::encodeRow(std::vector{cell(U'\uFFFD', Grey, Black)}));
}

TEST_CASE("encodeRow writes C1 controls as '?'") {
    const std::string q = irc::encodeRow(std::vector{cell(U'?', Grey, Black)});
    for (const char32_t cp : {char32_t(0x80), char32_t(0x85), char32_t(0x9b), char32_t(0x9f)}) {
        INFO("U+" << std::hex << unsigned(cp));
        CHECK(irc::encodeRow(std::vector{cell(cp, Grey, Black)}) == q);
    }
    // U+00A0 is not a control: it reaches the encoder as itself, which draws
    // it as the blank it is, like a space
    const auto row = [](char32_t ch) {
        const Cell x = cell(U'x', LightRed, Blue);
        return irc::encodeRow(std::vector{x, cell(ch, LightRed, Blue), x});
    };
    CHECK(row(char32_t(0xa0)) == row(U' '));
    CHECK(row(char32_t(0xa0)) != row(U'?'));
}

TEST_CASE("encodeRow writes C0 controls and DEL as their CP437 glyphs") {
    // as the parser reads those bytes: NUL as a space, the rest as glyphs
    CHECK(irc::encodeRow(std::vector{cell(char32_t(0x01), LightRed, Blue)}) == "\x03" "4,2☺");
    CHECK(irc::encodeRow(std::vector{cell(char32_t(0x7f), LightRed, Blue)}) == "\x03" "4,2⌂");
    std::vector<char32_t> controls;
    for (char32_t cp = 0; cp < 0x20; ++cp)
        controls.push_back(cp);
    controls.push_back(0x7f);
    for (const char32_t cp : controls) {
        INFO("U+" << std::hex << unsigned(cp));
        const char32_t glyph = cp == 0 ? U' ' : cp437ToUnicode(static_cast<std::uint8_t>(cp));
        const std::string line = irc::encodeRow(std::vector{cell(cp, LightRed, Blue), cell(U'x', LightRed, Blue)});
        CHECK(line == irc::encodeRow(std::vector{cell(glyph, LightRed, Blue), cell(U'x', LightRed, Blue)}));
        // no control byte but the colour codes
        CHECK(std::none_of(line.begin(), line.end(), [](char b) {
            return b != 0x03 && (static_cast<unsigned char>(b) < 0x20 || b == 0x7f);
        }));
    }
}

TEST_CASE("transparent cells export as blank cells") {
    Canvas c(1, 1);
    c.at(0, 0) = cell(U'X', LightRed, Blue);
    c.at(0, 0).transparent = true;
    CHECK(irc::encodeCanvas(c) == std::vector<std::string>{""});
    CHECK(irc::exportCanvas(c).empty());
    // between drawn cells it is a black space
    Canvas row(3, 1);
    row.at(0, 0) = cell(U'a', Grey, Black);
    row.at(1, 0) = cell(U'X', LightRed, Blue);
    row.at(1, 0).transparent = true;
    row.at(2, 0) = cell(U'b', Grey, Black);
    CHECK(irc::encodeCanvas(row) ==
          std::vector{irc::encodeRow(std::vector{cell(U'a', Grey, Black), Cell{}, cell(U'b', Grey, Black)})});
    CHECK(irc::encodeRow(std::vector{row.at(1, 0)}) == irc::encodeRow(std::vector{Cell{}}));
}

TEST_CASE("a zero-width canvas encodes as empty rows") {
    CHECK(irc::encodeCanvas(Canvas(0, 2)) == std::vector<std::string>{"", ""});
    CHECK(irc::exportCanvas(Canvas(0, 2)).empty());
}

TEST_CASE("swapShades lets the encoder swap shades") {
    // ░ on a colour it can't merge without the swap
    const std::vector row{cell(U'▓', Blue, Red), cell(U'░', Red, Blue)};
    const std::string plain = irc::encodeRow(row);
    irc::EncodeOptions o;
    o.swapShades = true;
    CHECK(irc::encodeRow(row, o).size() < plain.size());
}

TEST_CASE("encodeCanvas drops trailing black spaces, row by row") {
    Canvas c(6, 3);
    c.at(0, 0) = cell(U'x', Grey, Black);
    c.at(1, 1) = cell(U' ', Grey, Red);  // a coloured space stays
    const auto lines = irc::encodeCanvas(c);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == irc::encodeRow(std::vector{cell(U'x', Grey, Black)}));
    CHECK(lines[1] == irc::encodeRow(std::vector{cell(U' ', Grey, Black), cell(U' ', Grey, Red)}));
    CHECK(lines[2].empty());
}

TEST_CASE("exportCanvas drops blank rows above and below the art, not between") {
    Canvas c(4, 6);
    c.at(1, 1) = cell(U'a', Grey, Black);
    c.at(0, 3) = cell(U'b', Grey, Black);
    const auto lines = irc::exportCanvas(c);
    REQUIRE(lines.size() == 3);
    CHECK(lines[1].empty());
    CHECK(irc::exportCanvas(Canvas(3, 2)).empty());
}

TEST_CASE("overBudget lists the lines longer than the budget") {
    const std::vector<std::string> lines{"abc", "abcd", "", "abcde"};
    const auto over = irc::overBudget(lines, 3);
    REQUIRE(over.size() == 2);
    CHECK(over[0].line == 2);
    CHECK(over[0].bytes == 4);
    CHECK(over[1].line == 4);
    CHECK(over[1].bytes == 5);
    CHECK(irc::overBudget(lines, 5).empty());
    CHECK(irc::joinLines(lines) == "abc\nabcd\n\nabcde\n");
}

// #401: a colour code that has to set a colour its glyph doesn't show (a
// space's foreground, a block's background) sets the one the next cell needs.
TEST_CASE("a space's foreground is the next cell's when that saves a code") {
    // ^C1,1 ^C0A drew these in 8 bytes
    CHECK(irc::encodeRow(std::vector{cell(U' ', Black, Black), cell(U'A', White, Black)}) == "\x03" "0,1 A");
    // ^C1,1 ^C4A
    CHECK(irc::encodeRow(std::vector{cell(U' ', Black, Black), cell(U'A', LightRed, Black)}) == "\x03" "4,1 A");
    // a background change: ^C0,1A^C0,2 ^C8B
    CHECK(irc::encodeRow(std::vector{cell(U'A', White, Black), cell(U' ', Blue, Blue), cell(U'B', Yellow, Blue)}) ==
          "\x03" "0,1A\x03" "8,2 B");
}

TEST_CASE("a block's background is the next cell's when that saves a code") {
    // ^C4█^C2█ drew these in 10 bytes: the space now carries the block's
    // colour as its background and the next cell's as its foreground
    CHECK(irc::encodeRow(std::vector{cell(U'█', LightRed, Black), cell(U' ', Grey, Blue)}) == "\x03" "2,4 █");
    // ^C0,1A^C4█^C2█
    CHECK(irc::encodeRow(std::vector{cell(U'A', White, Black), cell(U'█', LightRed, Black), cell(U' ', Grey, Blue)}) ==
          "\x03" "0,1A\x03" "2,4 █");
}

namespace {

// What a cell looks like, whichever of the ways that draw it the encoder
// picked: one colour for a solid cell, two for a half block, the glyph and
// both colours for anything else.
std::tuple<char32_t, int, int> looks(char32_t ch, int fg, int bg) {
    if (ch == U'\0' || ch == U' ' || ch == U'\u00a0') return {U'█', bg, bg};
    if (ch == U'█' || fg == bg) return {U'█', fg, fg};
    switch (ch) {
    case U'▀': return {U'▀', fg, bg};
    case U'▄': return {U'▀', bg, fg};
    case U'▌': return {U'▌', fg, bg};
    case U'▐': return {U'▌', bg, fg};
    default: return {ch, fg, bg};
    }
}

} // namespace

TEST_CASE("every golden piece's rows read back as the cells they were made from") {
    std::vector<fs::path> pieces;
    for (const auto& e : fs::directory_iterator(bwtest::dataDir() / "golden"))
        if (e.path().extension() == ".ans")
            pieces.push_back(e.path());
    std::sort(pieces.begin(), pieces.end());
    REQUIRE(pieces.size() >= 12);
    for (const auto& p : pieces) {
        DYNAMIC_SECTION(p.filename().string()) {
            const Canvas c = parseAnsi(bwtest::readFile(p)).canvas;
            const auto lines = irc::encodeCanvas(c);
            REQUIRE(int(lines.size()) == c.height());
            for (int y = 0; y < c.height(); ++y) {
                INFO("row " << y + 1);
                const auto shown = irc::decodeLine(lines[std::size_t(y)]);
                REQUIRE(int(shown.size()) <= c.width());
                for (int x = 0; x < c.width(); ++x) {
                    INFO("column " << x + 1);
                    const Cell& src = c.at(x, y);
                    const auto want = looks(src.ch.codePointOr(U'?'), irc::mircColor(src.fg), irc::mircColor(src.bg));
                    if (x >= int(shown.size())) {
                        // the trailing black cells IRC drops
                        CHECK(want == std::tuple<char32_t, int, int>{U'█', 1, 1});
                        continue;
                    }
                    const irc::MircCell& got = shown[std::size_t(x)].cell;
                    CHECK(looks(got.ch.codePointOr(U'?'), got.fg, got.bg) == want);
                }
            }
        }
    }
}

// tests/data/golden-irc/<piece>.txt is the fixed IRC output for
// tests/data/golden/<piece>.ans (the irc_oracle test checks `cli export --irc`
// against it too).
TEST_CASE("golden pieces export byte for byte as golden-irc holds them") {
    std::vector<fs::path> pieces;
    for (const auto& e : fs::directory_iterator(bwtest::dataDir() / "golden"))
        if (e.path().extension() == ".ans")
            pieces.push_back(e.path());
    std::sort(pieces.begin(), pieces.end());
    REQUIRE(pieces.size() >= 12);
    for (const auto& p : pieces) {
        DYNAMIC_SECTION(p.filename().string()) {
            const fs::path expected = bwtest::dataDir() / "golden-irc" / (p.stem().string() + ".txt");
            REQUIRE(fs::exists(expected));
            const std::string want = bwtest::readFile(expected);
            const auto lines = irc::exportCanvas(parseAnsi(bwtest::readFile(p)).canvas);
            const auto wantLines = splitLines(want);
            REQUIRE(lines.size() == wantLines.size());
            for (size_t i = 0; i < lines.size(); ++i) {
                INFO("line " << i + 1);
                CHECK(lines[i].size() == wantLines[i].size());
            }
            CHECK(irc::joinLines(lines) == want);
        }
    }
}
