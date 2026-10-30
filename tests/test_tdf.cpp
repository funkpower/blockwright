// TheDraw fonts (PLAN §15): the reader, its faces, and the three test fonts
// (tests/data/tdf, made by make_test_fonts.py there).

#include "blockwright/tdf.h"

#include "blockwright/layer.h"
#include "util.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace bw;

namespace {

std::u32string widen(std::string_view s) { return {s.begin(), s.end()}; }

std::vector<std::u32string> fixtureWords() {
    std::istringstream in(bwtest::readFile(bwtest::dataDir() / "text" / "words.txt"));
    std::vector<std::u32string> out;
    for (std::string line; std::getline(in, line);)
        out.push_back(widen(line));
    return out;
}

std::string tdfData(const std::string& name) { return bwtest::readFile(bwtest::dataDir() / "tdf" / name); }

TdfFont onlyFont(const std::string& name) {
    auto fonts = readTdf(tdfData(name));
    REQUIRE(fonts.size() == 1);
    return std::move(fonts[0]);
}

// A row of cells as its characters, a hole as '.'.
std::u32string rowText(const CellBlock& b, int y) {
    std::u32string out;
    for (int x = 0; x < b.width; ++x)
        out += b.at(x, y).transparent ? U'.' : b.at(x, y).ch;
    return out;
}

Cell cell(char32_t ch, int fg, int bg, std::uint8_t attrs = 0) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(static_cast<std::uint8_t>(fg));
    c.bg = Color::indexed(static_cast<std::uint8_t>(bg));
    c.attrs = attrs;
    return c;
}

// Builds .tdf files byte by byte. A glyph is its width, height and stream
// (the terminating 00 added); `at` places a character at a given offset
// instead of after the last glyph.
struct FontSpec {
    std::string name = "test";
    int type = 1, spacing = 1;
    std::vector<std::pair<char, std::string>> glyphs;
    std::vector<std::pair<char, unsigned>> at;  // offsets set by hand
    std::string tail;                           // bytes after the glyphs, inside the data
};

std::string glyphBytes(int w, int h, std::string_view stream) {
    std::string out{static_cast<char>(w), static_cast<char>(h)};
    out += stream;
    out += '\0';
    return out;
}

void putU16(std::string& s, size_t at, unsigned v) {
    s[at] = static_cast<char>(v & 0xFF);
    s[at + 1] = static_cast<char>(v >> 8);
}

std::string fontBytes(const FontSpec& f) {
    std::string data;
    std::vector<unsigned> offsets(94, 0xFFFF);
    for (const auto& [ch, bytes] : f.glyphs) {
        offsets[static_cast<size_t>(ch - '!')] = static_cast<unsigned>(data.size());
        data += bytes;
    }
    data += f.tail;
    for (const auto& [ch, off] : f.at)
        offsets[static_cast<size_t>(ch - '!')] = off;
    std::string out("\x55\xAA\x00\xFF", 4);
    out += static_cast<char>(f.name.size());
    std::string name = f.name;
    name.resize(12, '\0');
    out += name;
    out += std::string(4, '\0');
    out += static_cast<char>(f.type);
    out += static_cast<char>(f.spacing);
    out += std::string(2 + 188, '\0');
    putU16(out, 23, static_cast<unsigned>(data.size()));
    for (size_t k = 0; k < 94; ++k)
        putU16(out, 25 + 2 * k, offsets[k]);
    return out + data;
}

const std::string kSignature = "\x13TheDraw FONTS file\x1A";

std::string fileOf(std::initializer_list<FontSpec> fonts) {
    std::string out = kSignature;
    for (const auto& f : fonts)
        out += fontBytes(f);
    return out;
}

FontSpec simpleBlock() {
    FontSpec f;
    f.glyphs = {{'A', glyphBytes(2, 2, "\xDB\xDB\r\xDB\xDB")}, {'B', glyphBytes(1, 1, "\xDB")}};
    return f;
}

void checkError(std::string_view bytes) {
    INFO(bytes.size() << " bytes");
    CHECK_THROWS_AS(readTdf(bytes), TdfError);
}

const std::u32string kCovered = U"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";

} // namespace

TEST_CASE("the test fonts read as one font each, A-Z, a-z, 0-9 and _ at most 8 columns") {
    struct Expect {
        const char* file;
        const char* name;
        TdfType type;
        int rows;
    };
    for (const auto& e : {Expect{"bw-outline.tdf", "BW Outline", TdfType::Outline, 12},
                          Expect{"bw-block.tdf", "BW Block", TdfType::Block, 3},
                          Expect{"bw-colour.tdf", "BW Colour", TdfType::Colour, 6}}) {
        INFO(e.file);
        const TdfFont font = onlyFont(e.file);
        CHECK(font.name == e.name);
        CHECK(font.type == e.type);
        CHECK(font.spacing == 1);
        CHECK(font.rejected.empty());
        int drawn = 0, tallest = 0;
        for (char32_t ch = TdfFont::kFirst; ch <= TdfFont::kLast; ++ch) {
            const auto& g = font.glyph(ch);
            CHECK(g.has_value() == (kCovered.find(ch) != kCovered.npos));
            if (g) {
                ++drawn;
                CHECK(g->width >= 2);
                CHECK(g->width <= 8);
                tallest = std::max(tallest, g->height);
            }
        }
        CHECK(drawn == 63);
        CHECK(tallest == e.rows);
        const auto face = tdfFace(font);
        CHECK(face->name() == e.name);
        CHECK(face->height() == 2 * e.rows);
        CHECK(face->letterSpacing() == 1);
    }
}

TEST_CASE("a file of several fonts reads them all, in order, as the files of one") {
    const auto fonts = readTdf(tdfData("bw-all.tdf"));
    REQUIRE(fonts.size() == 3);
    const char* files[] = {"bw-outline.tdf", "bw-block.tdf", "bw-colour.tdf"};
    for (size_t i = 0; i < 3; ++i) {
        const TdfFont one = onlyFont(files[i]);
        CHECK(fonts[i].name == one.name);
        CHECK(fonts[i].type == one.type);
        CHECK(fonts[i].glyphs == one.glyphs);
    }
    // Built by hand: three fonts, the second of another type and spacing.
    FontSpec a = simpleBlock(), b = simpleBlock(), c = simpleBlock();
    a.name = "one";
    b.name = "two";
    b.type = 0;
    b.spacing = 0;
    b.glyphs = {{'C', glyphBytes(2, 1, "EF")}};
    c.name = "three";
    const auto three = readTdf(fileOf({a, b, c}));
    REQUIRE(three.size() == 3);
    CHECK(three[0].name == "one");
    CHECK(three[1].name == "two");
    CHECK(three[1].type == TdfType::Outline);
    CHECK(three[1].spacing == 0);
    CHECK(rowText(*three[1].glyph(U'C'), 0) == U"╒╗");
    CHECK(!three[1].glyph(U'A'));
    CHECK(three[2].name == "three");
    CHECK(three[2].glyph(U'A')->width == 2);
}

TEST_CASE("every fixture word lays out in every test font within 80 columns, nothing clipped") {
    const auto words = fixtureWords();
    REQUIRE(words.size() == 5);
    for (const char* file : {"bw-outline.tdf", "bw-block.tdf", "bw-colour.tdf"}) {
        const auto face = tdfFace(onlyFont(file));
        // Each glyph and its spacing: 76, 69, 58 and 70 columns.
        const int widths[] = {76, 69, 58, 70};
        for (size_t i = 0; i < words.size(); ++i) {
            const auto& word = words[i];
            INFO(file << ": " << std::string(word.begin(), word.end()));
            const TextLayout l = layoutText(*face, word, {80});
            CHECK(l.missing.empty());
            REQUIRE(l.cells);
            CHECK(l.mask.width <= 80);
            if (i < 4) {
                CHECK(l.lines.size() == 1);
                CHECK(l.mask.width == widths[i]);
            }
            CHECK(l.mask.height == static_cast<int>(l.lines.size()) * face->height());
            CHECK(l.cells->height * 2 == l.mask.height);
            int ink = 0;
            for (const auto& line : l.lines)
                for (size_t k = line.first; k < line.first + line.count; ++k)
                    ink += face->glyph(word[k])->mask.count();
            CHECK(l.mask.count() == ink);
            // The laid-out cells are the mask: no ink cell lost or made.
            CHECK(maskOfCells(*l.cells) == l.mask);
        }
    }
}

TEST_CASE("outline codes draw in TheDraw's 10th style, '@' a hole and 'O' a blank space") {
    const auto face = tdfFace(onlyFont("bw-outline.tdf"));
    const auto a = face->glyph(U'A');
    REQUIRE(a);
    REQUIRE(a->cells);
    CHECK(a->cells->width == 8);
    CHECK(a->cells->height == 12);
    CHECK(rowText(*a->cells, 0) == U"..╒══╗..");
    CHECK(rowText(*a->cells, 4) == U"│╚════╛║");
    CHECK(rowText(*a->cells, 5) == U"│╓────┐║");
    CHECK(rowText(*a->cells, 9) == U"└╜....└╜");
    CHECK(rowText(*a->cells, 10) == U"........");  // below the letter
    CHECK(a->cells->at(2, 0) == cell(U'╒', Grey, Black));
    // 'O' is a hard space inside the letter: a cell, but blank.
    const auto e = face->glyph(U'e');
    REQUIRE(e);
    CHECK(rowText(*e->cells, 3) == U"..│  ║..");
    CHECK(!e->cells->at(3, 3).transparent);
    CHECK(!sourceCellInk(e->cells->at(3, 3)));
    CHECK(!e->mask.at(3, 6));
    CHECK(e->mask.at(2, 6));
    // The descender rows' '&' is not drawn and takes no column.
    const auto u = face->glyph(U'_');
    REQUIRE(u);
    CHECK(u->cells->width == 8);
    CHECK(rowText(*u->cells, 10) == U"╒══════╗");
    CHECK(rowText(*u->cells, 11) == U"└──────╜");
}

TEST_CASE("block glyphs keep their characters, █ included, grey on black") {
    const auto face = tdfFace(onlyFont("bw-block.tdf"));
    const auto h = face->glyph(U'H');
    REQUIRE(h);
    REQUIRE(h->cells);
    CHECK(rowText(*h->cells, 0) == U"██    ██");
    CHECK(rowText(*h->cells, 1) == U"██▀▀▀▀██");
    CHECK(rowText(*h->cells, 2) == U"▀▀    ▀▀");
    CHECK(h->cells->at(0, 0) == cell(U'█', Grey, Black));
    CHECK(h->cells->at(2, 0) == cell(U' ', Grey, Black));
    // A space in the glyph is a cell, blank, so not ink.
    CHECK(!h->mask.at(2, 0));
    CHECK(h->mask.at(2, 2));
    CHECK(h->mask.at(2, 3));
    // A row shorter than the glyph ends in holes.
    const auto g = face->glyph(U'g');
    REQUIRE(g);
    CHECK(rowText(*g->cells, 0) == U"  ▄▄▄▄▄▄");
    CHECK(rowText(*face->glyph(U'_')->cells, 0) == U"........");
}

TEST_CASE("colour glyphs keep their shades and attributes; a space on blue is ink") {
    const auto face = tdfFace(onlyFont("bw-colour.tdf"));
    const auto h = face->glyph(U'H');
    REQUIRE(h);
    REQUIRE(h->cells);
    CHECK(rowText(*h->cells, 0) == U"██    ██");
    CHECK(rowText(*h->cells, 1) == U"▓▓    ▓▓");
    CHECK(rowText(*h->cells, 2) == U"▒▒▒▒▒▒▒▒");
    CHECK(rowText(*h->cells, 3) == U"░░    ░░");
    CHECK(rowText(*h->cells, 4) == U"        ");
    CHECK(rowText(*h->cells, 5) == U"........");
    CHECK(h->cells->at(0, 0) == cell(U'█', Cyan, Black));
    CHECK(h->cells->at(0, 1) == cell(U'▓', Cyan, Blue));
    CHECK(h->cells->at(0, 3) == cell(U'░', Cyan, Blue));
    CHECK(h->cells->at(0, 4) == cell(U' ', Black, Blue));
    CHECK(h->cells->at(2, 4) == cell(U' ', Black, Black));
    CHECK(sourceCellInk(h->cells->at(0, 4)));
    CHECK(!sourceCellInk(h->cells->at(2, 4)));
    CHECK(h->mask.at(0, 8));
    CHECK(!h->mask.at(2, 8));
    // A descender row ends in the '&' marker, not drawn.
    const auto g = face->glyph(U'g');
    REQUIRE(g);
    CHECK(rowText(*g->cells, 5) == U"      ..");
    CHECK(g->cells->at(0, 5) == cell(U' ', Black, Blue));
    // What a "█ only" rule needs: the █ cells are the top row of every
    // capital that has one, and nothing else is █.
    int solid = 0;
    for (char32_t ch : kCovered) {
        const auto glyph = face->glyph(ch);
        const auto& cells = *glyph->cells;
        for (int y = 0; y < cells.height; ++y)
            for (int x = 0; x < cells.width; ++x)
                if (cells.at(x, y).ch == U'█') {
                    CHECK(y == 0);
                    ++solid;
                }
    }
    CHECK(solid > 0);
}

TEST_CASE("colour attributes: foreground low nibble, background bits 4-6, bit 7 blink") {
    FontSpec f;
    f.type = 2;
    f.glyphs = {{'A', glyphBytes(3, 1, std::string("\xDB\x4E\xB0\xF1&\x0A", 6))}};
    const auto fonts = readTdf(fileOf({f}));
    const auto& a = *fonts[0].glyph(U'A');
    CHECK(a.at(0, 0) == cell(U'█', Yellow, Red));
    CHECK(a.at(1, 0) == cell(U'░', Blue, Grey, attr::Blink));
    // '&' with an attribute after it is drawn.
    CHECK(a.at(2, 0) == cell(U'&', LightGreen, Black));
}

TEST_CASE("colour glyphs are read in pairs: an attribute of 00 or 0D ends no glyph and no row") {
    for (const char attribute : {'\x00', '\x0D'}) {
        INFO("attribute " << int(attribute));
        FontSpec f;
        f.type = 2;
        f.glyphs = {{'A', glyphBytes(2, 1, std::string("\x26") + attribute + "\xDB\x0F")}};
        const auto fonts = readTdf(fileOf({f}));
        CHECK(fonts[0].rejected.empty());
        const auto& a = *fonts[0].glyph(U'A');
        CHECK(a.height == 1);
        CHECK(rowText(a, 0) == U"&█");
        CHECK(a.at(0, 0) == cell(U'&', attribute & 15, (attribute >> 4) & 7));
        CHECK(a.at(1, 0) == cell(U'█', White, Black));
    }
    // As AtlantisSmB's Q has it, a black '&' and a space after it, and the
    // next glyph's data straight after.
    FontSpec f;
    f.type = 2;
    f.glyphs = {{'Q', glyphBytes(3, 1, std::string("\xDB\x0F\x26\x00\x20\x07", 6))},
                {'R', glyphBytes(1, 1, std::string("\xB0\x01", 2))}};
    const auto fonts = readTdf(fileOf({f}));
    const auto& q = *fonts[0].glyph(U'Q');
    CHECK(rowText(q, 0) == U"█& ");
    CHECK(q.at(1, 0) == cell(U'&', Black, Black));
    CHECK(q.at(2, 0) == cell(U' ', Grey, Black));
    CHECK(rowText(*fonts[0].glyph(U'R'), 0) == U"░");
}

TEST_CASE("a colour font's '&' stored alone before 0D or 00 is the descender marker") {
    // As Fraktur and Metal have it: read in pairs the marker would take the
    // 0D or 00 after it as its attribute and run into the next glyph's data,
    // or past the end of the font's.
    FontSpec f;
    f.type = 2;
    f.glyphs = {{'A', glyphBytes(1, 1, std::string("\xDB\x0F\r\xB2\x0F&\r\xB1\x0F&", 10))},
                {'B', glyphBytes(1, 1, std::string("\xDB\x0E", 2))},
                {'C', glyphBytes(1, 1, std::string("\xDB\x0C&", 3))}};
    const auto fonts = readTdf(fileOf({f}));
    CHECK(fonts[0].rejected.empty());
    const auto& a = *fonts[0].glyph(U'A');
    REQUIRE(a.height == 3);
    CHECK(rowText(a, 0) == U"█");
    CHECK(rowText(a, 1) == U"▓");
    CHECK(rowText(a, 2) == U"▒");
    CHECK(fonts[0].glyph(U'B')->at(0, 0) == cell(U'█', Yellow, Black));
    // The last glyph, where the pairs would run past the end of the data.
    const auto& c = *fonts[0].glyph(U'C');
    CHECK(c.height == 1);
    CHECK(rowText(c, 0) == U"█");
}

TEST_CASE("a file whose glyphs come to over 1 Mi cells fails before they are made, however small") {
    // Codex's repro: 18,477 fonts of 94 glyphs sharing one 30x12 glyph's
    // data, 4,194,299 bytes, would make 625 million cells.
    FontSpec big;
    big.glyphs = {{'A', glyphBytes(30, 12, std::string(11, '\r'))}};
    for (char ch = '!'; ch <= '~'; ++ch)
        big.at.push_back({ch, 0});
    const std::string one = fontBytes(big);
    std::string bytes = kSignature;
    for (int n = 0; n < 18477; ++n)
        bytes += one;
    REQUIRE(bytes.size() == 4194299);
    try {
        readTdf(bytes);
        FAIL("read");
    } catch (const TdfError& e) {
        CHECK(std::string(e.what()).find("cells") != std::string::npos);
    }
    // At the limit a file reads: 30 fonts of 94 such glyphs; one of 92 of
    // them and a 21x12; and one of a 1x2 and a 1x1, which its face draws 2
    // rows tall: 1,048,576 cells, each glyph as tall as its font's tallest.
    // One cell more fails.
    constexpr std::size_t kCells = std::size_t{1} << 20;
    FontSpec rest;
    rest.glyphs = {{'A', glyphBytes(30, 12, std::string(11, '\r'))}};
    for (char ch = '!'; ch < '!' + 92; ++ch)
        rest.at.push_back({ch, 0});
    rest.glyphs.push_back({'}', glyphBytes(21, 12, std::string(11, '\r'))});
    FontSpec tail;
    tail.glyphs = {{'A', glyphBytes(1, 2, "\xDB\r\xDB")}, {'B', glyphBytes(1, 1, "\xDB")}};
    REQUIRE(30 * 94 * 360 + 92 * 360 + 252 + 2 * 2 == kCells);
    std::string full = kSignature;
    for (int n = 0; n < 30; ++n)
        full += one;
    full += fontBytes(rest) + fontBytes(tail);
    CHECK(readTdf(full).size() == 32);
    FontSpec tiny;
    tiny.glyphs = {{'A', glyphBytes(1, 1, "\xDB")}};
    CHECK_THROWS_AS(readTdf(full + fontBytes(tiny)), TdfError);
}

TEST_CASE("a file whose glyphs, each as tall as its font's tallest, come to over 1 Mi cells fails") {
    // Codex round 1 (#168): 256 fonts of 93 characters sharing a 30x1
    // glyph and one 30x12, 66,580 bytes. Read as stored they are 806,400
    // cells; their faces stand every glyph 12 rows tall, 8,663,040 cells.
    FontSpec font;
    font.glyphs = {{'!', glyphBytes(30, 1, std::string(30, '\xDB'))}, {'~', glyphBytes(30, 12, std::string(11, '\r'))}};
    for (char ch = '!'; ch < '~'; ++ch)
        font.at.push_back({ch, 0});
    std::string bytes = kSignature;
    for (int n = 0; n < 256; ++n)
        bytes += fontBytes(font);
    REQUIRE(bytes.size() == 66580);
    try {
        readTdf(bytes);
        FAIL("read");
    } catch (const TdfError& e) {
        CHECK(std::string(e.what()).find("cells") != std::string::npos);
    }
    // 30 such fonts are 1,015,200 cells as their faces draw them, and read
    std::string fewer = kSignature;
    for (int n = 0; n < 30; ++n)
        fewer += fontBytes(font);
    CHECK(readTdf(fewer).size() == 30);
    CHECK_THROWS_AS(readTdf(fewer + fontBytes(font)), TdfError);
}

TEST_CASE("a file of over 256 fonts fails") {
    FontSpec tiny;
    tiny.glyphs = {{'A', glyphBytes(1, 1, "\xDB")}};
    std::string bytes = kSignature;
    for (int n = 0; n < 256; ++n)
        bytes += fontBytes(tiny);
    CHECK(readTdf(bytes).size() == 256);
    try {
        readTdf(bytes + fontBytes(tiny));
        FAIL("read");
    } catch (const TdfError& e) {
        CHECK(std::string(e.what()).find("256 fonts") != std::string::npos);
    }
}

TEST_CASE("characters a TheDraw font lacks are missing; the space is blank, half a letter wide") {
    const auto face = tdfFace(onlyFont("bw-block.tdf"));
    // The letters average 7 columns: the space is 4.
    const auto space = face->glyph(U' ');
    REQUIRE(space);
    CHECK(space->mask.width == 4);
    CHECK(space->mask.count() == 0);
    REQUIRE(space->cells);
    CHECK(space->cells->cells == std::vector<Cell>(12, transparentCell()));
    CHECK(!face->glyph(U'!'));
    CHECK(!face->glyph(U'~'));
    CHECK(!face->glyph(U'é'));
    CHECK(!face->glyph(U'\t'));
    const std::u32string text = U"A!é b~";
    const TextLayout l = layoutText(*face, text, {80});
    CHECK(l.missing == std::vector<size_t>{1, 2, 5});
    // A 8, ! 4, é 4, space 4, b 8, ~ 4, and 1 between each.
    CHECK(l.mask.width == 8 + 4 + 4 + 4 + 8 + 4 + 5);
    // A rejected glyph is missing too.
    FontSpec bad = simpleBlock();
    bad.glyphs.push_back({'C', glyphBytes(31, 1, "\xDB")});
    const auto fonts = readTdf(fileOf({bad}));
    REQUIRE(fonts[0].rejected.size() == 1);
    CHECK(fonts[0].rejected[0].ch == U'C');
    CHECK(!tdfFace(fonts[0])->glyph(U'C'));
    CHECK(tdfFace(fonts[0])->glyph(U'A'));
}

TEST_CASE("screening the test fonts on load finds only the block font's coarse pairs") {
    CHECK(tdfFace(onlyFont("bw-outline.tdf"))->confusables().empty());
    CHECK(tdfFace(onlyFont("bw-colour.tdf"))->confusables().empty());
    const auto block = tdfFace(onlyFont("bw-block.tdf"));
    CHECK(block->confusables() ==
          std::vector<ConfusablePair>{{U'B', U'5'}, {U'8', U'0'}, {U'Z', U'2'}, {U'W', U'H'}});
    CHECK(block->confusables() == screenConfusables(*block));
}

TEST_CASE("glyph data the published fonts have: shared offsets, wide rows, short rows, spacing 0") {
    FontSpec f;
    f.spacing = 0;
    f.glyphs = {{'A', glyphBytes(2, 2, "\xDB\r\xDB\xDB\xDB")}};
    f.at = {{'B', 0}};
    const auto fonts = readTdf(fileOf({f}));
    REQUIRE(fonts.size() == 1);
    const TdfFont& font = fonts[0];
    CHECK(font.spacing == 0);
    const auto& a = *font.glyph(U'A');
    // The second row is wider than the width byte: the glyph is 3 wide.
    CHECK(a.width == 3);
    CHECK(rowText(a, 0) == U"█..");
    CHECK(rowText(a, 1) == U"███");
    CHECK(font.glyph(U'B') == font.glyph(U'A'));
    CHECK(tdfFace(font)->letterSpacing() == 0);
}

TEST_CASE("the end of the fonts: end of file, 00 or SAUCE's 1A; anything else is an error") {
    const std::string one = fileOf({simpleBlock()});
    CHECK(readTdf(one).size() == 1);
    CHECK(readTdf(one + '\0' + "junk").size() == 1);
    CHECK(readTdf(one + "\x1A" + "SAUCE00").size() == 1);
    checkError(one + "junk");
    checkError(one + "\x55\xAA");
}

TEST_CASE("broken files fail with a TdfError naming where") {
    const std::string good = fileOf({simpleBlock()});
    REQUIRE(readTdf(good).size() == 1);

    SECTION("no signature") {
        checkError("");
        checkError("TheDraw FONTS file");
        std::string s = good;
        s[1] = 't';
        checkError(s);
    }
    SECTION("no font") { checkError(kSignature); }
    SECTION("a truncated font header") {
        for (size_t n : {size_t{21}, size_t{24}, size_t{100}, kSignature.size() + 212})
            checkError(good.substr(0, n));
        try {
            readTdf(good.substr(0, 100));
        } catch (const TdfError& e) {
            CHECK(std::string(e.what()).find("byte 20") != std::string::npos);
        }
    }
    SECTION("no font indicator") {
        std::string s = good;
        s[21] = 0;
        checkError(s);
    }
    SECTION("a name over 12") {
        std::string s = good;
        s[24] = 13;
        checkError(s);
    }
    SECTION("an unknown type") {
        std::string s = good;
        s[20 + 21] = 3;
        checkError(s);
    }
    SECTION("glyph data past the end of the file") {
        checkError(good.substr(0, good.size() - 1));
        std::string s = good;
        putU16(s, 20 + 23, 0xFFFE);
        checkError(s);
    }
    SECTION("a glyph pointing past the font's data") {
        FontSpec f = simpleBlock();
        f.at = {{'Z', static_cast<unsigned>(fontBytes(f).size() - 213)}};
        checkError(fileOf({f}));
        f.at = {{'Z', 0xFFFE}};
        checkError(fileOf({f}));
        try {
            readTdf(fileOf({f}));
        } catch (const TdfError& e) {
            CHECK(std::string(e.what()).find("'Z'") != std::string::npos);
        }
    }
}

TEST_CASE("a glyph that can't be read is rejected, the rest of the font kept") {
    const auto rejects = [](const std::string& glyph, char32_t ch = U'C', int type = 1) {
        // 'A' reads as any type of font.
        FontSpec f;
        f.type = type;
        f.glyphs = {{'A', glyphBytes(2, 1, "AB")}, {static_cast<char>(ch), glyph}};
        const auto fonts = readTdf(fileOf({f}));
        REQUIRE(fonts.size() == 1);
        CHECK(fonts[0].glyph(U'A'));
        CHECK(!fonts[0].glyph(ch));
        REQUIRE(fonts[0].rejected.size() == 1);
        CHECK(fonts[0].rejected[0].ch == ch);
        return fonts[0].rejected[0].reason;
    };
    // Oversized or empty dimensions.
    CHECK(rejects(glyphBytes(31, 1, "\xDB")).find("31x1") != std::string::npos);
    rejects(glyphBytes(8, 13, "\xDB"));
    rejects(glyphBytes(0, 1, "\xDB"));
    // A row over 30 columns, or over 12 rows.
    rejects(glyphBytes(30, 1, std::string(31, '\xDB')));
    rejects(glyphBytes(1, 12, std::string(12, '\r')));
    // A height of 0, which published fonts have, reads: the rows are what count.
    {
        FontSpec f;
        f.glyphs = {{'A', glyphBytes(1, 0, "\xDB\r\xDB")}};
        const auto fonts = readTdf(fileOf({f}));
        const auto& a = *fonts[0].glyph(U'A');
        CHECK(a.height == 2);
    }
    // The largest that reads: 30 columns, 12 rows.
    {
        FontSpec f;
        std::string rows = std::string(30, '\xDB');
        for (int i = 0; i < 11; ++i)
            rows += "\r" + std::string(30, '\xDB');
        f.glyphs = {{'A', glyphBytes(30, 12, rows)}};
        const auto fonts = readTdf(fileOf({f}));
        const auto& a = *fonts[0].glyph(U'A');
        CHECK(a.width == 30);
        CHECK(a.height == 12);
    }
    // No terminator before the end of the font's data.
    rejects(std::string("\x02\x01\xDB\xDB", 4));
    // A glyph header cut off by the end of the data.
    rejects(std::string("\x02", 1));
    // A colour glyph ending between a character and its attribute.
    rejects(std::string("\x02\x01\xDB\x0F\xDB", 5), U'C', 2);
}

TEST_CASE("every prefix and every one-byte change of a real file reads or fails cleanly") {
    const std::string all = tdfData("bw-all.tdf");
    for (size_t n = 0; n < all.size(); ++n) {
        try {
            readTdf(std::string_view(all).substr(0, n));
        } catch (const TdfError&) {
        }
    }
    // The first font's header and glyph table, and a stretch of its glyphs,
    // each byte set to 00, 0D, '&' and FF.
    std::string s = all;
    for (size_t i = 0; i < 20 + 213 + 400; ++i) {
        const char keep = s[i];
        for (char b : {'\x00', '\x0D', '&', '\xFF'}) {
            s[i] = b;
            try {
                for (const auto& font : readTdf(s))
                    tdfFace(font);
            } catch (const TdfError&) {
            }
        }
        s[i] = keep;
    }
    SUCCEED();
}

TEST_CASE("reading a .tdf file: missing or over the size limit is a TdfError") {
    CHECK(readTdfFile(bwtest::dataDir() / "tdf" / "bw-block.tdf").size() == 1);
    CHECK_THROWS_AS(readTdfFile(bwtest::dataDir() / "tdf" / "no-such.tdf"), TdfError);
    const std::filesystem::path dir = BW_TEST_TMP;
    std::filesystem::create_directories(dir);
    const auto big = dir / "big.tdf";
    {
        std::ofstream out(big, std::ios::binary);
        std::string bytes = fileOf({simpleBlock()});
        bytes.resize(static_cast<size_t>(kMaxTdfBytes) + 1, '\0');
        out << bytes;
    }
    CHECK_THROWS_AS(readTdfFile(big), TdfError);
    std::filesystem::resize_file(big, kMaxTdfBytes);
    CHECK(readTdfFile(big).size() == 1);
    std::filesystem::remove(big);
}
