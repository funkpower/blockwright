// XBIN and BIN: header and run bytes checked by hand, the readers' edge
// cases, loss, round trips over every golden piece, and the files in
// tests/data/xbin/ (written by writeXbin and writeBin from golden/tux.ans,
// and decoded by Moebius's XBIN reader to the same cells) as a fixed oracle.

#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"
#include "blockwright/pc_fonts.h"
#include "blockwright/sauce.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <vector>

using namespace bw;
namespace fs = std::filesystem;

namespace {

std::string bytes(std::initializer_list<int> list) {
    std::string out;
    for (int b : list)
        out += static_cast<char>(b);
    return out;
}

std::string compress(std::initializer_list<int> pairs) {
    const std::string s = bytes(pairs);
    return xbinCompressRow(std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

std::string repeated(int n, int ch, int attr) {
    std::string out;
    for (int i = 0; i < n; ++i)
        out += bytes({ch, attr});
    return out;
}

std::string compressBytes(const std::string& s) {
    return xbinCompressRow(std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

Cell cell(char32_t ch, int fg, int bg, std::uint8_t attrs = 0) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(std::uint8_t(fg));
    c.bg = Color::indexed(std::uint8_t(bg));
    c.attrs = attrs;
    return c;
}

// An XBIN header for `width` x `height`.
std::string header(int width, int height, int fontHeight, int flags) {
    return bytes({'X', 'B', 'I', 'N', 0x1A, width & 0xFF, width >> 8, height & 0xFF, height >> 8, fontHeight, flags});
}

Sauce someSauce() {
    Sauce s;
    s.title = "Test";
    s.author = "funkpower";
    s.date = "20261030";
    return s;
}

// The SAUCE record a written file ends with.
Sauce sauceOf(const std::string& file) {
    const SauceSplit split = splitSauce(file);
    REQUIRE(split.sauce);
    return *split.sauce;
}

fs::path golden(const std::string& name) { return bwtest::dataDir() / "golden" / name; }
fs::path fixture(const std::string& name) { return bwtest::dataDir() / "xbin" / name; }

// A palette no 6-bit value is lost from: each entry widened as the reader
// widens it.
std::array<Rgb, 16> customPalette16() {
    const auto widen = [](int v) { return std::uint8_t(v << 2 | v >> 4); };
    std::array<Rgb, 16> p;
    for (int i = 0; i < 16; ++i)
        p[size_t(i)] = {widen(i * 4), widen(63 - i * 4), widen((i * 9) % 64)};
    return p;
}

FileFont bios8x8() {
    const auto f = ibmBios8x8();
    return FileFont{8, std::string(reinterpret_cast<const char*>(f.data()), f.size())};
}

// golden/tux.ans with the palette and font of tux-palette-font.xb.
AnsiDocument tuxWithPaletteAndFont() {
    AnsiDocument d = parseAnsi(bwtest::readFile(golden("tux.ans")));
    d.palette = customPalette16();
    d.font = bios8x8();
    return d;
}

} // namespace

TEST_CASE("artFormatFor goes by the extension, artFormatOf by the XBIN magic first", "[xbin]") {
    CHECK(artFormatFor("piece.xb") == ArtFormat::Xbin);
    CHECK(artFormatFor("dir/PIECE.XBIN") == ArtFormat::Xbin);
    CHECK(artFormatFor("piece.Bin") == ArtFormat::Bin);
    CHECK(artFormatFor("piece.ans") == ArtFormat::Ansi);
    CHECK(artFormatFor("piece") == ArtFormat::Ansi);
    CHECK(artFormatFor(".bin") == ArtFormat::Ansi);  // a dotfile has no extension
    CHECK(artFormatFor("x.bin/piece") == ArtFormat::Ansi);
    CHECK(artFormatFor("piece.xb.ans") == ArtFormat::Ansi);

    CHECK(artFormatOf("piece.ans", "XBIN\x1a") == ArtFormat::Xbin);
    CHECK(artFormatOf("piece.bin", "XBIN\x1a...") == ArtFormat::Xbin);
    CHECK(artFormatOf("piece.xb", "XBIN") == ArtFormat::Xbin);  // by its name
    CHECK(artFormatOf("piece.bin", "\x1b[0m") == ArtFormat::Bin);
    CHECK(artFormatOf("piece.ans", "XBIM\x1a") == ArtFormat::Ansi);
}

TEST_CASE("writeXbin's header: size, font height, flags, palette and font", "[xbin]") {
    AnsiDocument d;
    d.canvas = Canvas(300, 2);
    d.sauce = someSauce();
    d.ice = false;

    std::string out = writeXbin(d).bytes;
    CHECK(out.substr(0, 11) == header(300, 2, 16, 0x04));

    d.ice = true;
    CHECK(writeXbin(d).bytes.substr(0, 11) == header(300, 2, 16, 0x0C));
    CHECK(writeXbin(d, {.compress = false}).bytes.substr(0, 11) == header(300, 2, 16, 0x08));

    d.palette = kPalette;
    d.font = bios8x8();
    out = writeXbin(d, {.compress = false}).bytes;
    CHECK(out.substr(0, 11) == header(300, 2, 8, 0x0B));
    // the standard palette as 6-bit values
    CHECK(out.substr(11, 6) == bytes({0, 0, 0, 0, 0, 0x2A}));
    CHECK(out.substr(11 + 45, 3) == bytes({0x3F, 0x3F, 0x3F}));
    CHECK(out.substr(11 + 48, 2048) == bios8x8().bits);
    CHECK(out.substr(11 + 48 + 2048, 4) == bytes({' ', 0x07, ' ', 0x07}));

    // a font shorter than 256 glyphs is padded with blank ones
    d.palette.reset();
    d.font = FileFont{4, "\xff\xff\xff\xff"};
    out = writeXbin(d, {.compress = false}).bytes;
    CHECK(out.substr(0, 11) == header(300, 2, 4, 0x0A));
    CHECK(out.substr(11, 1024) == "\xff\xff\xff\xff" + std::string(1020, '\0'));
}

TEST_CASE("writeXbin's SAUCE is XBin: DataType 6, the size in TInfo1-2, no flags or font", "[xbin]") {
    AnsiDocument d;
    d.canvas = Canvas(3, 2);
    d.sauce = someSauce();
    d.sauce->tinfo3 = 9;
    d.sauce->setLetterSpacing(2);
    d.sauce->comments = {"kept"};
    const AnsiOutput written = writeXbin(d, {.compress = false});
    // the 9 px letter spacing it leaves out is a loss (#299)
    CHECK(written.letterSpacingDropped);
    const std::string& out = written.bytes;
    const Sauce s = sauceOf(out);
    CHECK(s.dataType == 6);
    CHECK(s.fileType == 0);
    CHECK(s.tinfo1 == 3);
    CHECK(s.tinfo2 == 2);
    CHECK(s.tinfo3 == 0);
    CHECK(s.tinfo4 == 0);
    CHECK(s.flags == 0);
    CHECK(s.font.empty());
    CHECK(s.title == "Test");
    CHECK(s.author == "funkpower");
    CHECK(s.date == "20261030");
    CHECK(s.comments == std::vector<std::string>{"kept"});
    // FileSize counts the header and the cells, not the EOF
    CHECK(s.fileSize == 11 + 12);
    CHECK(out[11 + 12] == '\x1a');
}

TEST_CASE("xbinCompressRow: the run kinds, by hand", "[xbin]") {
    CHECK(compress({}).empty());
    // one cell: every kind costs 3 bytes, and both repeated comes first
    CHECK(compress({'A', 0x07}) == bytes({0xC0, 'A', 0x07}));
    // 64 the same is one run; the 65th starts another
    CHECK(compressBytes(repeated(64, 'A', 0x07)) == bytes({0xFF, 'A', 0x07}));
    CHECK(compressBytes(repeated(65, 'A', 0x07)) == bytes({0xFF, 'A', 0x07, 0xC0, 'A', 0x07}));
    // the attribute repeated
    CHECK(compress({'A', 0x1F, 'B', 0x1F, 'C', 0x1F, 'D', 0x1F}) == bytes({0x83, 0x1F, 'A', 'B', 'C', 'D'}));
    // the character repeated
    CHECK(compress({'X', 1, 'X', 2, 'X', 3, 'X', 4}) == bytes({0x43, 'X', 1, 2, 3, 4}));
    // nothing repeated
    CHECK(compress({'A', 1, 'B', 2, 'C', 3}) == bytes({0x02, 'A', 1, 'B', 2, 'C', 3}));
    // AAAB on one attribute: both repeated then one cell (6 bytes) ties with
    // the attribute repeated (6 bytes), and both repeated comes first
    CHECK(compress({'A', 7, 'A', 7, 'A', 7, 'B', 7}) == bytes({0xC2, 'A', 7, 0xC0, 'B', 7}));
    // AAAAB: both repeated, then one cell (6 bytes) beats the attribute (7)
    CHECK(compress({'A', 7, 'A', 7, 'A', 7, 'A', 7, 'B', 7}) == bytes({0xC3, 'A', 7, 0xC0, 'B', 7}));
    // 65 distinct cells: a plain run is 64 long at most, so one cell goes
    // on its own, and at the same cost the first run is both repeated
    std::string distinct;
    for (int i = 0; i < 65; ++i)
        distinct += bytes({i, i + 1});
    const std::string out = compressBytes(distinct);
    REQUIRE(out.size() == 3 + 1 + 128);
    CHECK(out.substr(0, 4) == bytes({0xC0, 0, 1, 0x3F}));
    CHECK(out.substr(4) == distinct.substr(2));
}

TEST_CASE("xbinCompressRow: random rows decode back, at most a run header per 64 cells past plain", "[xbin]") {
    std::mt19937 rng(21);
    for (int round = 0; round < 200; ++round) {
        const int width = 1 + int(rng() % 200), height = 1 + int(rng() % 3);
        // a small alphabet, so runs of every kind turn up
        const int chars = 1 + int(rng() % 4), attrs = 1 + int(rng() % 4);
        AnsiDocument d;
        d.ice = true;
        d.sauce = someSauce();
        d.canvas = Canvas(width, height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                d.canvas.at(x, y) = cell(cp437ToUnicode(std::uint8_t('A' + rng() % chars)), int(rng() % attrs),
                                         int(rng() % attrs) * 5);
        const AnsiOutput packed = writeXbin(d), plain = writeXbin(d, {.compress = false});
        const AnsiDocument back = parseXbin(packed.bytes);
        REQUIRE(back.canvas == d.canvas);
        REQUIRE(parseXbin(plain.bytes).canvas == d.canvas);
        // at worst a plain run every 64 cells of a row
        CHECK(packed.bytes.size() <= plain.bytes.size() + size_t(height) * ((width + 63) / 64));
    }
}

TEST_CASE("parseXbin decodes runs across row ends and is lenient at the end of the data", "[xbin]") {
    // one run of four cells fills two rows of two
    AnsiDocument d = parseXbin(header(2, 2, 16, 0x0C) + bytes({0xC3, 'A', 0x1E}));
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            CHECK(d.canvas.at(x, y) == cell(U'A', 14, 1));
    CHECK(d.ice);
    CHECK_FALSE(d.sauce);
    CHECK_FALSE(d.palette);
    CHECK_FALSE(d.font);

    // a run longer than the cells left stops at the last cell, and bytes
    // after it are ignored
    d = parseXbin(header(3, 1, 16, 0x04) + bytes({0xC9, 'Z', 0x07, 0x00, 'Q', 0x07}));
    CHECK(d.canvas.width() == 3);
    CHECK(d.canvas.at(2, 0).ch == U'Z');

    // data cut short leaves the rest of the cells blank, mid-run or not
    d = parseXbin(header(4, 1, 16, 0x04) + bytes({0x03, 'A', 0x07, 'B'}));
    CHECK(d.canvas.at(0, 0).ch == U'A');
    CHECK(d.canvas.at(1, 0) == Cell{});
    d = parseXbin(header(4, 1, 16, 0x04) + bytes({0x82, 0x4F, 'A'}));
    CHECK(d.canvas.at(0, 0) == cell(U'A', 15, 4));
    CHECK(d.canvas.at(1, 0) == Cell{});
    d = parseXbin(header(4, 1, 16, 0x04) + bytes({0x42, 'H', 0x01, 0x02}));
    CHECK(d.canvas.at(0, 0) == cell(U'H', 1, 0));
    CHECK(d.canvas.at(1, 0) == cell(U'H', 2, 0));
    CHECK(d.canvas.at(2, 0) == Cell{});
    d = parseXbin(header(2, 1, 16, 0x04) + bytes({0xC1, 'A'}));
    CHECK(d.canvas.at(0, 0) == Cell{});
    d = parseXbin(header(2, 1, 16, 0x00) + bytes({'A', 0x07, 'B'}));
    CHECK(d.canvas.at(0, 0).ch == U'A');
    CHECK(d.canvas.at(1, 0) == Cell{});
    CHECK(parseXbin(header(5, 5, 0, 0x04)).canvas == Canvas(5, 5));
}

TEST_CASE("parseXbin: the attribute's top bit is a bright background with the non-blink flag, else blink", "[xbin]") {
    const std::string cells = bytes({'A', 0x9F});
    AnsiDocument d = parseXbin(header(1, 1, 16, 0x08) + cells);
    CHECK(d.ice);
    CHECK(d.canvas.at(0, 0) == cell(U'A', 15, 9));
    d = parseXbin(header(1, 1, 16, 0x00) + cells);
    CHECK_FALSE(d.ice);
    CHECK(d.canvas.at(0, 0) == cell(U'A', 15, 1, attr::Blink));
    // and writeXbin gives the same byte back either way
    CHECK(writeXbin(d, {.compress = false}).bytes.substr(11, 2) == cells);
}

TEST_CASE("parseXbin reads the palette as 6-bit values and the font", "[xbin]") {
    std::string pal;
    for (const Rgb& c : kPalette)
        pal += bytes({c.r >> 2, c.g >> 2, c.b >> 2});
    AnsiDocument d = parseXbin(header(1, 1, 16, 0x01) + pal + bytes({'A', 7}));
    REQUIRE(d.palette);
    CHECK(*d.palette == kPalette);  // the standard palette widens back exactly
    CHECK_FALSE(customPalette(d));
    CHECK(d.canvas.at(0, 0).ch == U'A');

    pal[0] = 0x3F;  // black's red
    pal[1] = 0x15;
    pal[2] = 0x7F;  // the top two bits are ignored
    d = parseXbin(header(1, 1, 16, 0x01) + pal + bytes({'A', 7}));
    CHECK((*d.palette)[0] == Rgb{0xFF, 0x55, 0xFF});
    CHECK(customPalette(d));

    const FileFont f = bios8x8();
    d = parseXbin(header(1, 1, 8, 0x03) + pal + f.bits + bytes({'A', 7}));
    REQUIRE(d.font);
    CHECK(*d.font == f);
    CHECK(d.canvas.at(0, 0).ch == U'A');
}

TEST_CASE("parseXbin refuses what isn't an XBIN it can read", "[xbin]") {
    CHECK_THROWS_AS(parseXbin(""), AnsiError);
    CHECK_THROWS_AS(parseXbin("XBIN\x1a"), AnsiError);  // no size
    CHECK_THROWS_AS(parseXbin("XBIM" + header(1, 1, 16, 0).substr(4)), AnsiError);
    CHECK_THROWS_AS(parseXbin(header(0, 1, 16, 0)), AnsiError);
    CHECK_THROWS_AS(parseXbin(header(1, 0, 16, 0)), AnsiError);
    CHECK_THROWS_AS(parseXbin(header(kMaxColumns + 1, 1, 16, 0)), AnsiError);
    CHECK_THROWS_AS(parseXbin(header(1, kMaxRows + 1, 16, 0)), AnsiError);
    CHECK_THROWS_AS(parseXbin(header(1, 1, 16, 0x12) + std::string(8192, '\0')), AnsiError);  // 512 glyphs
    CHECK_THROWS_AS(parseXbin(header(1, 1, 16, 0x01) + std::string(47, '\0')), AnsiError);
    CHECK_THROWS_AS(parseXbin(header(1, 1, 0, 0x02) + std::string(4096, '\0')), AnsiError);
    CHECK_THROWS_AS(parseXbin(header(1, 1, 33, 0x02) + std::string(256 * 33, '\0')), AnsiError);
    CHECK_THROWS_AS(parseXbin(header(1, 1, 16, 0x02) + std::string(4095, '\0')), AnsiError);
    // a font height of 0 without a font is no matter
    CHECK_NOTHROW(parseXbin(header(1, 1, 0, 0)));
    // the 512-glyph flag says nothing without a font, but a reader that
    // trusts it reads the wrong data, so it is refused all the same
    CHECK_THROWS_AS(parseXbin(header(1, 1, 16, 0x10)), AnsiError);
}

TEST_CASE("parseXbin keeps SAUCE and finds the data before it, EOF bytes in the cells and all", "[xbin]") {
    AnsiDocument d;
    d.ice = true;
    d.sauce = someSauce();
    d.canvas = Canvas(2, 1);
    // 0x1A as a glyph and as an attribute
    d.canvas.at(0, 0) = cell(cp437ToUnicode(0x1A), 10, 1);
    d.canvas.at(1, 0) = cell(U'A', 0, 0);
    for (const bool packed : {true, false}) {
        const std::string out = writeXbin(d, {.compress = packed}).bytes;
        const AnsiDocument back = parseXbin(out);
        CHECK(back.canvas == d.canvas);
        REQUIRE(back.sauce);
        CHECK(back.sauce->title == "Test");
    }
}

TEST_CASE("parseXbin finds SAUCE only after the cells: cells that spell SAUCE are cells", "[xbin]") {
    // 64 cells, 128 bytes, the last 128 of the file: "SAUCE00" and NULs
    const std::string spelled = "SAUCE00" + std::string(121, '\0');
    AnsiDocument d = parseXbin(header(64, 1, 16, 0x08) + spelled);
    CHECK_FALSE(d.sauce);
    CHECK(d.canvas.at(0, 0).ch == U'S');
    CHECK(d.canvas.at(1, 0).ch == U'U');
    CHECK(d.canvas.at(1, 0).fg == Color::indexed(3));  // 'C'
    // compressed: one plain run of 64
    d = parseXbin(header(64, 1, 16, 0x0C) + bytes({0x3F}) + spelled);
    CHECK_FALSE(d.sauce);
    CHECK(d.canvas.at(0, 0).ch == U'S');
    // with a real record after them, that is the SAUCE
    Sauce s = someSauce();
    d = parseXbin(header(64, 1, 16, 0x08) + spelled + encodeSauce(s, 139));
    REQUIRE(d.sauce);
    CHECK(d.sauce->title == "Test");
    CHECK(d.canvas.at(0, 0).ch == U'S');
    // the last cell's 0x1A attribute, the record straight after it (no EOF)
    d = parseXbin(header(1, 1, 16, 0x08) + bytes({'A', 0x1A}) + encodeSauce(s, 13).substr(1));
    REQUIRE(d.sauce);
    CHECK(d.canvas.at(0, 0) == cell(U'A', 10, 1));
    // a file cut short of its cells: the record at its end is SAUCE, and
    // the cells it stops short of are blank
    d = parseXbin(header(80, 1, 16, 0x08) + bytes({'A', 0x07}) + encodeSauce(s, 13));
    REQUIRE(d.sauce);
    CHECK(d.sauce->title == "Test");
    CHECK(d.canvas.at(0, 0) == cell(U'A', 7, 0));
    CHECK(d.canvas.at(1, 0) == Cell{});
    CHECK(d.canvas.at(2, 0) == Cell{});
}

TEST_CASE("parseXbin: a file cut short reads its cells from before SAUCE's EOF", "[xbin]") {
    Sauce s;
    s.dataType = 6;
    // Codex's repro: a repeat-four run without its attribute is blanks, the EOF
    // does not complete it
    {
        const std::string image = header(400, 1, 16, 0x0C) + bytes({0xC3, 'A'});
        const std::string file = image + encodeSauce(s, std::uint32_t(image.size()));
        const AnsiDocument d = parseXbin(file);
        REQUIRE(d.sauce);
        CHECK(d.canvas == Canvas(400, 1));
        CHECK(splitArtSauce(file, ArtFormat::Xbin).content.size() == image.size());
    }
    // one complete cell, then a run of each kind short its last operand
    const std::string first = header(400, 1, 16, 0x0C) + bytes({0x00, 'A', 0x07});
    Canvas want(400, 1);
    want.at(0, 0) = cell(U'A', 7, 0);
    Sauce commented = s;
    commented.comments = {"a comment"};
    const struct {
        const char* kind;
        std::string run;
    } runs[] = {
        {"plain", bytes({0x00, 'B'})},
        {"same character", bytes({0x40, 'B'})},
        {"same attribute", bytes({0x80, 0x07})},
        {"same both", bytes({0xC3, 'B'})},
    };
    for (const auto& [kind, run] : runs) {
        for (const Sauce& rec : {s, commented}) {
            INFO(kind << (rec.comments.empty() ? ", EOF" : ", EOF + COMNT"));
            const std::string image = first + run;
            const std::string file = image + encodeSauce(rec, std::uint32_t(image.size()));
            const AnsiDocument d = parseXbin(file);
            REQUIRE(d.sauce);
            CHECK(d.sauce->comments == rec.comments);
            CHECK(d.canvas == want);
            const SauceSplit split = splitArtSauce(file, ArtFormat::Xbin);
            REQUIRE(split.sauce);
            CHECK(split.content.size() == image.size());
            CHECK(split.sauce->fileSize == split.content.size());
        }
    }
    // uncompressed: a cell without its attribute
    {
        const std::string image = header(400, 1, 16, 0x08) + bytes({'A', 0x07, 'B'});
        const std::string file = image + encodeSauce(s, std::uint32_t(image.size()));
        const AnsiDocument d = parseXbin(file);
        REQUIRE(d.sauce);
        CHECK(d.canvas == want);
        CHECK(splitArtSauce(file, ArtFormat::Xbin).content.size() == image.size());
    }
    // a last 0x1A attribute before the EOF is the image's
    {
        const std::string image = header(400, 1, 16, 0x0C) + bytes({0x00, 'A', 0x1A});
        const std::string file = image + encodeSauce(s, std::uint32_t(image.size()));
        const AnsiDocument d = parseXbin(file);
        REQUIRE(d.sauce);
        Canvas last(400, 1);
        last.at(0, 0) = cell(U'A', 10, 1);
        CHECK(d.canvas == last);
        CHECK(splitArtSauce(file, ArtFormat::Xbin).content.size() == image.size());
    }
}

TEST_CASE("splitArtSauce: the content is the bytes the cells take, a last 0x1A included", "[xbin]") {
    Sauce s = someSauce();
    s.dataType = 6;
    // a 1x1 XBIN whose attribute is 0x1A, the record straight after it
    const std::string image = header(1, 1, 16, 0x08) + bytes({'A', 0x1A});
    // the split's views are into `file`
    std::string file = image + encodeSauce(s, 13).substr(1);
    SauceSplit split = splitArtSauce(file, ArtFormat::Xbin);
    REQUIRE(split.sauce);
    CHECK(split.content.size() == 13);
    CHECK(split.body.size() == 13);
    // with SAUCE's EOF, the content is the same and the EOF is in the body
    file = image + encodeSauce(s, 13);
    split = splitArtSauce(file, ArtFormat::Xbin);
    REQUIRE(split.sauce);
    CHECK(split.content.size() == 13);
    CHECK(split.body.size() == 14);
    // compressed: where the runs end
    file = header(4, 1, 16, 0x0C) + bytes({0xC3, 'A', 0x1A}) + encodeSauce(s, 14);
    split = splitArtSauce(file, ArtFormat::Xbin);
    REQUIRE(split.sauce);
    CHECK(split.content.size() == 14);
    // cells that spell SAUCE are content
    file = header(64, 1, 16, 0x08) + "SAUCE00" + std::string(121, '\0');
    split = splitArtSauce(file, ArtFormat::Xbin);
    CHECK_FALSE(split.sauce);
    CHECK(split.content.size() == 139);
    // the fixtures' FileSize is their content
    for (const char* name : {"tux.xb", "tux-plain.xb", "tux-palette-font.xb", "tux.bin"}) {
        file = bwtest::readFile(fixture(name));
        split = splitArtSauce(file, artFormatOf(name, file));
        REQUIRE(split.sauce);
        CHECK(split.sauce->fileSize == split.content.size());
    }
    // BIN: the pairs, SAUCE's EOF dropped; .ans: splitSauce
    file = bytes({'A', 0x1A}) + encodeSauce(s, 2);
    split = splitArtSauce(file, ArtFormat::Bin);
    CHECK(split.content.size() == 2);
    file = bytes({'A', 0x1A}) + encodeSauce(s, 2).substr(1);
    split = splitArtSauce(file, ArtFormat::Bin);
    CHECK(split.content.size() == 2);
    file = "hi\x1a" + encodeSauce(s, 2);
    split = splitArtSauce(file, ArtFormat::Ansi);
    CHECK(split.content == "hi");
}

TEST_CASE("parseBin: a record whose FileSize is past the bytes before it is cells", "[xbin]") {
    // the last 64 cells spell "SAUCE00", then 0x07s: FileSize 0x07070707
    const std::string spelled = "SAUCE00" + std::string(121, '\x07');
    const std::string file = repeated(16, 'A', 7) + spelled;
    AnsiDocument d = parseBin(file);
    CHECK_FALSE(d.sauce);
    CHECK(d.canvas.width() == kDefaultBinWidth);
    CHECK(d.canvas.at(15, 0).ch == U'A');
    CHECK(d.canvas.at(16, 0).ch == U'S');
    CHECK(d.canvas.at(79, 0).ch == cp437ToUnicode(0x07));
    // a real record names at most the bytes before it
    Sauce s = someSauce();
    s.dataType = 5;
    s.fileType = 8;
    d = parseBin(repeated(16, 'A', 7) + encodeSauce(s, 32));
    REQUIRE(d.sauce);
    CHECK(d.canvas.width() == 16);
    d = parseBin(repeated(16, 'A', 7) + encodeSauce(s, 0));
    REQUIRE(d.sauce);
    d = parseBin(repeated(16, 'A', 7) + encodeSauce(s, 33));  // the EOF counted
    REQUIRE(d.sauce);
    d = parseBin(repeated(16, 'A', 7) + encodeSauce(s, 34));
    CHECK_FALSE(d.sauce);
}

TEST_CASE("every byte and attribute survives XBIN and BIN, NUL and control codes included", "[xbin]") {
    AnsiDocument d;
    d.ice = true;
    d.sauce = someSauce();
    d.canvas = Canvas(16, 16);
    std::string pairs;
    for (int i = 0; i < 256; ++i) {
        const int a = (i * 7) & 0xFF;
        d.canvas.at(i % 16, i / 16) = cell(cp437ToUnicode(std::uint8_t(i)), a & 15, a >> 4);
        pairs += bytes({i, a});
    }
    CHECK(writeXbin(d, {.compress = false}).bytes.substr(11, 512) == pairs);
    CHECK(writeBin(d).bytes.substr(0, 512) == pairs);
    CHECK(parseXbin(writeXbin(d).bytes).canvas == d.canvas);
    CHECK(parseBin(writeBin(d).bytes).canvas == d.canvas);
    CHECK(parseBin(pairs, {.defaultWidth = 16}).canvas == d.canvas);
}

TEST_CASE("the binary writers write '?' for a glyph CP437 hasn't got, and transparent cells blank", "[xbin]") {
    AnsiDocument d;
    d.sauce = someSauce();
    d.canvas = Canvas(2, 1);
    d.canvas.at(0, 0) = cell(U'一', 7, 0);
    d.canvas.at(1, 0) = cell(U'A', 4, 2);
    d.canvas.at(1, 0).transparent = true;
    for (const AnsiOutput& out : {writeXbin(d, {.compress = false}), writeBin(d)}) {
        REQUIRE(out.unwritable.size() == 1);
        CHECK(out.unwritable[0].x == 0);
        CHECK(out.unwritable[0].ch == U'一');
        CHECK(out.lossy());
        CHECK(out.bytes.find(bytes({'?', 0x07, ' ', 0x07})) != std::string::npos);
    }
}

TEST_CASE("the binary writers force iCE on for a bright background, as writeAnsi does", "[xbin]") {
    AnsiDocument d;
    d.ice = false;
    d.sauce = someSauce();
    d.canvas = Canvas(2, 1);
    d.canvas.at(0, 0) = cell(U'A', 7, 9);
    d.canvas.at(1, 0) = cell(U'B', 7, 1, attr::Blink);
    const AnsiOutput ansi = writeAnsi(d);
    const AnsiOutput x = writeXbin(d, {.compress = false}), b = writeBin(d);
    CHECK(ansi.blinkDropped == 1);
    CHECK(x.blinkDropped == 1);
    CHECK(b.blinkDropped == 1);
    CHECK(std::uint8_t(x.bytes[10]) == 0x08);
    CHECK(x.bytes.substr(11, 4) == bytes({'A', 0x97, 'B', 0x17}));
    CHECK(b.bytes.substr(0, 4) == bytes({'A', 0x97, 'B', 0x17}));
    CHECK(sauceOf(b.bytes).ice());
    // without the bright background blink is kept, as the top bit
    d.canvas.at(0, 0).bg = Color::indexed(1);
    const AnsiOutput kept = writeBin(d);
    CHECK(kept.blinkDropped == 0);
    CHECK(kept.bytes.substr(0, 4) == bytes({'A', 0x17, 'B', 0x97}));
    CHECK_FALSE(sauceOf(kept.bytes).ice());
    CHECK(parseBin(kept.bytes).canvas == d.canvas);
}

TEST_CASE("parseBin: the width from a BinaryText SAUCE, else 160, and iCE as its flags say", "[xbin]") {
    const std::string pairs = bytes({'A', 0x9F, 'B', 0x07, 'C', 0x07});
    // no SAUCE: 160 wide, iCE on
    AnsiDocument d = parseBin(pairs);
    CHECK(d.canvas.width() == kDefaultBinWidth);
    CHECK(d.canvas.height() == 1);
    CHECK(d.ice);
    CHECK(d.canvas.at(0, 0) == cell(U'A', 15, 9));
    CHECK(d.canvas.at(2, 0).ch == U'C');
    CHECK(d.canvas.at(3, 0) == Cell{});
    CHECK(parseBin(pairs, {.defaultWidth = 2}).canvas.height() == 2);

    // BinaryText, FileType 1: 2 wide; iCE off
    Sauce s = someSauce();
    s.dataType = 5;
    s.fileType = 1;
    s.setIce(false);
    std::string file = pairs + encodeSauce(s, std::uint32_t(pairs.size()));
    d = parseBin(file);
    CHECK(d.canvas.width() == 2);
    CHECK(d.canvas.height() == 2);
    CHECK_FALSE(d.ice);
    CHECK(d.canvas.at(0, 0) == cell(U'A', 15, 1, attr::Blink));
    CHECK(d.canvas.at(0, 1).ch == U'C');
    CHECK(d.canvas.at(1, 1) == Cell{});
    REQUIRE(d.sauce);
    CHECK(d.sauce->title == "Test");

    // FileType 0 is the default width
    s.fileType = 0;
    CHECK(parseBin(pairs + encodeSauce(s, 6)).canvas.width() == kDefaultBinWidth);
    // a SAUCE that isn't BinaryText says nothing of the width or iCE
    s.dataType = 1;
    s.fileType = 1;
    d = parseBin(pairs + encodeSauce(s, 6));
    CHECK(d.canvas.width() == kDefaultBinWidth);
    CHECK(d.ice);
}

TEST_CASE("parseBin drops SAUCE's EOF and an odd last byte, and keeps 0x1A in the cells", "[xbin]") {
    const std::string pairs = bytes({0x1A, 0x1A, 'B', 0x1A});
    AnsiDocument d = parseBin(pairs + "\x1a", {.defaultWidth = 2});
    CHECK(d.canvas.height() == 1);
    CHECK(d.canvas.at(0, 0) == cell(cp437ToUnicode(0x1A), 10, 1));
    CHECK(d.canvas.at(1, 0) == cell(U'B', 10, 1));
    // no EOF before the SAUCE
    Sauce s = someSauce();
    s.dataType = 5;
    s.fileType = 1;
    s.setIce(true);
    const std::string record = encodeSauce(s, 4).substr(1);
    REQUIRE(record.substr(0, 5) == "SAUCE");
    CHECK(parseBin(pairs + record).canvas == d.canvas);
    CHECK(parseBin(pairs + encodeSauce(s, 4)).canvas == d.canvas);
}

TEST_CASE("parseBin refuses no pairs, a bad width and too many rows", "[xbin]") {
    CHECK_THROWS_AS(parseBin(""), AnsiError);
    CHECK_THROWS_AS(parseBin("A"), AnsiError);
    CHECK_THROWS_AS(parseBin("\x1a"), AnsiError);
    CHECK_THROWS_AS(parseBin("A\x07", {.defaultWidth = 0}), AnsiError);
    CHECK_THROWS_AS(parseBin("A\x07", {.defaultWidth = kMaxColumns + 1}), AnsiError);
    CHECK_THROWS_AS(parseBin(repeated(kMaxRows + 1, 'A', 7), {.defaultWidth = 1}), AnsiError);
    CHECK(parseBin(repeated(kMaxRows, 'A', 7), {.defaultWidth = 1}).canvas.height() == kMaxRows);
}

TEST_CASE("writeBin: BinaryText SAUCE, and a blank column on an odd width", "[xbin]") {
    AnsiDocument d;
    d.ice = true;
    d.sauce = someSauce();
    d.sauce->tinfo1 = 3;
    d.sauce->tinfo2 = 1;
    d.sauce->setLetterSpacing(2);
    d.sauce->font = "IBM VGA";
    d.canvas = Canvas(3, 1);
    d.canvas.at(2, 0) = cell(U'C', 12, 0);
    const AnsiOutput out = writeBin(d);
    CHECK_FALSE(out.lossy());
    CHECK(out.bytes.substr(0, 9) == bytes({' ', 7, ' ', 7, 'C', 12, ' ', 7, 0x1A}));
    const Sauce s = sauceOf(out.bytes);
    CHECK(s.dataType == 5);
    CHECK(s.fileType == 2);
    CHECK(s.tinfo1 == 0);
    CHECK(s.tinfo2 == 0);
    CHECK(s.ice());
    CHECK(s.letterSpacing() == 2);
    CHECK(s.font == "IBM VGA");
    CHECK(s.fileSize == 8);
    const AnsiDocument back = parseBin(out.bytes);
    CHECK(back.canvas.width() == 4);
    CHECK(back.canvas.at(2, 0) == d.canvas.at(2, 0));
    CHECK(back.canvas.at(3, 0) == Cell{});

    // the widest canvas fits FileType's byte
    d.canvas = Canvas(kMaxColumns, 1);
    CHECK(sauceOf(writeBin(d).bytes).fileType == kMaxColumns / 2);
    CHECK(parseBin(writeBin(d).bytes).canvas.width() == kMaxColumns);
}

TEST_CASE("a palette or font only XBIN keeps is reported lost by the others", "[xbin]") {
    AnsiDocument d;
    d.sauce = someSauce();
    d.canvas = Canvas(2, 1);
    CHECK_FALSE(writeAnsi(d).lossy());

    d.palette = kPalette;  // the standard one loses nothing
    CHECK_FALSE(writeAnsi(d).lossy());
    CHECK_FALSE(writeBin(d).lossy());
    d.palette->at(3) = Rgb{1, 2, 3};
    for (const AnsiOutput& out : {writeAnsi(d), writeBin(d)}) {
        CHECK(out.paletteDropped);
        CHECK_FALSE(out.fontDropped);
        CHECK(out.lossy());
    }
    d.palette.reset();
    d.font = bios8x8();
    for (const AnsiOutput& out : {writeAnsi(d), writeBin(d)}) {
        CHECK_FALSE(out.paletteDropped);
        CHECK(out.fontDropped);
        CHECK(out.lossy());
    }
    d.palette = customPalette16();
    const AnsiOutput x = writeXbin(d);
    CHECK_FALSE(x.lossy());
    const AnsiDocument back = parseXbin(x.bytes);
    CHECK(back.palette == d.palette);
    CHECK(back.font == d.font);
}

// #299: XBin's SAUCE has no TFlags or TInfoS and its header no letter
// spacing or aspect, so an XBIN save leaves out 9 px letter spacing, the
// aspect ratio and a font name, and reports each; BIN keeps all three. 8 px,
// an unset flag and IBM VGA are what an XBIN draws: nothing is lost.
TEST_CASE("writeXbin reports the SAUCE letter spacing, aspect and font name it leaves out", "[xbin]") {
    AnsiDocument d;
    d.canvas = Canvas(4, 1);
    for (int x = 0; x < 4; ++x)
        d.canvas.at(x, 0) = cell(char32_t(U'A' + x), 1, 4);
    d.ice = true;
    d.sauce = someSauce();
    d.sauce->flags = 0x15;  // iCE, 9 px, square
    d.sauce->font = "Amiga Topaz 1";
    // through .ans first, as the issue's file was
    const AnsiDocument src = parseAnsi(writeAnsi(d).bytes);
    REQUIRE(src.sauce);
    REQUIRE(src.sauce->flags == 0x15);
    REQUIRE(src.sauce->font == "Amiga Topaz 1");

    const AnsiOutput x = writeXbin(src);
    CHECK(x.letterSpacingDropped);
    CHECK(x.aspectDropped == 2);
    CHECK(x.fontNameDropped == "Amiga Topaz 1");
    CHECK_FALSE(x.paletteDropped);
    CHECK_FALSE(x.fontDropped);
    CHECK(x.lossy());
    // read back: the cells and iCE (the header's) kept, the three gone
    const AnsiDocument back = parseXbin(x.bytes);
    CHECK(back.canvas == src.canvas);
    CHECK(back.ice);
    REQUIRE(back.sauce);
    CHECK(back.sauce->letterSpacing() == 0);
    CHECK(back.sauce->aspectRatio() == 0);
    CHECK(back.sauce->font.empty());
    // and what an XBIN read back holds, it writes again without loss
    CHECK_FALSE(writeXbin(back).lossy());
    CHECK(writeXbin(back).bytes == x.bytes);

    // BIN keeps all three
    const AnsiOutput b = writeBin(src);
    CHECK_FALSE(b.lossy());
    CHECK(parseBin(b.bytes).sauce->flags == 0x15);
    CHECK(parseBin(b.bytes).sauce->font == "Amiga Topaz 1");

    // each on its own
    AnsiDocument one = src;
    one.sauce->flags = 0x01;
    one.sauce->font = "IBM VGA";
    CHECK_FALSE(writeXbin(one).lossy());
    one.sauce->setLetterSpacing(1);  // 8 px: what an XBIN draws
    CHECK_FALSE(writeXbin(one).lossy());
    one.sauce->setLetterSpacing(2);
    CHECK(writeXbin(one).letterSpacingDropped);
    CHECK(writeXbin(one).aspectDropped == 0);
    CHECK(writeXbin(one).fontNameDropped.empty());
    one.sauce->setLetterSpacing(0);
    one.sauce->setAspectRatio(1);
    CHECK(writeXbin(one).aspectDropped == 1);
    CHECK_FALSE(writeXbin(one).letterSpacingDropped);
    CHECK(writeXbin(one).lossy());
    one.sauce->setAspectRatio(0);
    for (const char* font : {"", "IBM VGA", "IBM VGA 437"}) {
        one.sauce->font = font;
        CHECK_FALSE(writeXbin(one).lossy());
    }
    one.sauce->font = "IBM VGA50";
    CHECK(writeXbin(one).fontNameDropped == "IBM VGA50");
    // no SAUCE on the document: the fresh record writeXbin adds loses nothing
    one.sauce.reset();
    CHECK_FALSE(writeXbin(one).lossy());
}

TEST_CASE("parseArt and writeArt go to the format's codec", "[xbin]") {
    const AnsiDocument d = parseAnsi(bwtest::readFile(golden("tux.ans")));
    CHECK(writeArt(d, ArtFormat::Ansi).bytes == writeAnsi(d).bytes);
    CHECK(writeArt(d, ArtFormat::Xbin).bytes == writeXbin(d).bytes);
    CHECK(writeArt(d, ArtFormat::Bin).bytes == writeBin(d).bytes);
    for (const ArtFormat f : {ArtFormat::Ansi, ArtFormat::Xbin, ArtFormat::Bin})
        CHECK(parseArt(writeArt(d, f).bytes, f).canvas.at(10, 5) == d.canvas.at(10, 5));
}

TEST_CASE("golden pieces round-trip through XBIN, compressed and not, and BIN", "[xbin][golden]") {
    int seen = 0;
    for (const auto& e : fs::directory_iterator(bwtest::dataDir() / "golden")) {
        if (e.path().extension() != ".ans")
            continue;
        ++seen;
        INFO(e.path().filename().string());
        const AnsiDocument d = parseAnsi(bwtest::readFile(e.path()));
        // the cells as an .ans written from the piece holds them
        const AnsiOutput ansi = writeAnsi(d);
        const AnsiDocument want = parseAnsi(ansi.bytes);
        const auto sameText = [&](const AnsiDocument& got) {
            REQUIRE(got.sauce);
            REQUIRE(d.sauce);
            CHECK(got.sauce->title == d.sauce->title);
            CHECK(got.sauce->author == d.sauce->author);
            CHECK(got.sauce->group == d.sauce->group);
            CHECK(got.sauce->date == d.sauce->date);
            CHECK(got.sauce->comments == d.sauce->comments);
        };
        for (const bool packed : {true, false}) {
            const AnsiOutput x = writeXbin(d, {.compress = packed});
            CHECK(x.unwritable.size() == ansi.unwritable.size());
            CHECK(x.blinkDropped == ansi.blinkDropped);
            const AnsiDocument back = parseXbin(x.bytes);
            CHECK(back.canvas == want.canvas);
            CHECK(back.ice == want.ice);
            sameText(back);
            // written again it is the same file, and as .ans the same cells
            CHECK(writeXbin(back, {.compress = packed}).bytes == x.bytes);
            CHECK(parseAnsi(writeAnsi(back).bytes).canvas == want.canvas);
        }
        const AnsiOutput b = writeBin(d);
        CHECK(b.unwritable.size() == ansi.unwritable.size());
        const AnsiDocument back = parseBin(b.bytes);
        const Canvas& c = want.canvas;
        REQUIRE(back.canvas.width() == c.width() + c.width() % 2);
        REQUIRE(back.canvas.height() == c.height());
        bool same = true;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < back.canvas.width(); ++x)
                same = same && back.canvas.at(x, y) == (x < c.width() ? c.at(x, y) : Cell{});
        CHECK(same);
        CHECK(back.ice == want.ice);
        sameText(back);
        CHECK(writeBin(back).bytes == b.bytes);
    }
    CHECK(seen >= 10);
}

TEST_CASE("the XBIN and BIN fixtures: the writers' bytes, and the readers' cells", "[xbin][golden]") {
    const AnsiDocument tux = parseAnsi(bwtest::readFile(golden("tux.ans")));
    const Canvas cells = parseAnsi(writeAnsi(tux).bytes).canvas;

    const std::string packed = bwtest::readFile(fixture("tux.xb"));
    const std::string plain = bwtest::readFile(fixture("tux-plain.xb"));
    const std::string bin = bwtest::readFile(fixture("tux.bin"));
    REQUIRE_FALSE(packed.empty());
    REQUIRE_FALSE(plain.empty());
    REQUIRE_FALSE(bin.empty());
    CHECK(writeXbin(tux).bytes == packed);
    CHECK(writeXbin(tux, {.compress = false}).bytes == plain);
    CHECK(writeBin(tux).bytes == bin);
    CHECK(packed.size() < plain.size());
    CHECK(parseXbin(packed).canvas == cells);
    CHECK(parseXbin(plain).canvas == cells);
    // tux is 30 wide: no padding column
    CHECK(parseBin(bin).canvas == cells);

    const std::string custom = bwtest::readFile(fixture("tux-palette-font.xb"));
    REQUIRE_FALSE(custom.empty());
    const AnsiDocument withBoth = tuxWithPaletteAndFont();
    CHECK(writeXbin(withBoth).bytes == custom);
    const AnsiDocument back = parseXbin(custom);
    CHECK(back.canvas == cells);
    CHECK(back.palette == withBoth.palette);
    CHECK(back.font == withBoth.font);
    // the header, the palette and the font are what was given
    CHECK(custom.substr(0, 11) == header(30, 17, 8, 0x0F));
    CHECK(custom.substr(11 + 48, 2048) == bios8x8().bits);
    // and what follows them is the same cells as tux.xb
    CHECK(custom.substr(11 + 48 + 2048, packed.find("\x1aSAUCE") - 11) == packed.substr(11, packed.find("\x1aSAUCE") - 11));
}
