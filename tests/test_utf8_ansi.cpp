#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace bw;
using namespace std::string_literals;
namespace fs = std::filesystem;

namespace {

AnsiDocument parseAs(std::string_view ansi, TextEncoding enc, int width = 80) {
    ParseOptions o;
    o.defaultWidth = width;
    o.encoding = enc;
    return parseAnsi(ansi, o);
}

// The first `n` glyphs of row `y`.
std::u32string row(const AnsiDocument& d, int y, int n) {
    std::u32string s;
    for (int x = 0; x < n; ++x)
        s += d.canvas.at(x, y).ch;
    return s;
}

std::string withSauce(const std::string& body, Sauce s) {
    return body + encodeSauce(s, static_cast<std::uint32_t>(body.size()));
}

} // namespace

TEST_CASE("nextUtf8 takes only well-formed sequences", "[utf8]") {
    auto one = [](std::string_view s) {
        std::size_t i = 0;
        auto cp = nextUtf8(s, i);
        return std::pair{cp, i};
    };
    CHECK(one("A") == std::pair{std::optional<char32_t>(U'A'), std::size_t(1)});
    CHECK(one("\xC3\x87") == std::pair{std::optional<char32_t>(U'Ç'), std::size_t(2)});
    CHECK(one("\xE2\x96\x88") == std::pair{std::optional<char32_t>(U'█'), std::size_t(3)});
    CHECK(one("\xF0\x9F\x98\x80") == std::pair{std::optional<char32_t>(U'\U0001F600'), std::size_t(4)});
    // each bad one is refused and only its first byte is passed over
    for (std::string_view bad : {"\x80", "\xBF", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\xAF",
                                 "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80",
                                 "\xE2\x96", "\xE2\x41\x88", "\xFF"}) {
        CAPTURE(std::string(bad));
        CHECK(one(bad) == std::pair{std::optional<char32_t>(), std::size_t(1)});
    }
}

TEST_CASE("detectEncoding: UTF-8 only when every high byte is well formed and one sequence is long",
          "[utf8]") {
    CHECK(detectEncoding("") == TextEncoding::Cp437);
    CHECK(detectEncoding("plain ASCII\r\n\x1b[1;31mred") == TextEncoding::Cp437);
    CHECK(detectEncoding("\xE2\x96\x88") == TextEncoding::Utf8);
    CHECK(detectEncoding("\x1b[31m\xE2\x96\x88\xE2\x96\x80\r\n") == TextEncoding::Utf8);
    CHECK(detectEncoding("\xF0\x9F\x98\x80") == TextEncoding::Utf8);
    // only two-byte sequences: CP437 pairs look like that (╔╗ is C9 BB, ╚╝ C8 BC)
    CHECK(detectEncoding("\xC9\xBB\r\n\xC8\xBC") == TextEncoding::Cp437);
    CHECK(detectEncoding("caf\xC3\xA9") == TextEncoding::Cp437);
    // one bad byte anywhere: CP437
    CHECK(detectEncoding("\xE2\x96\x88\x80") == TextEncoding::Cp437);
    CHECK(detectEncoding("\xDB\xE2\x96\x88") == TextEncoding::Cp437);
    // a BOM says UTF-8 whatever follows
    CHECK(detectEncoding("\xEF\xBB\xBF") == TextEncoding::Utf8);
    CHECK(detectEncoding("\xEF\xBB\xBF" "A") == TextEncoding::Utf8);
    CHECK(detectEncoding("\xEF\xBB\xBF\xDB\xB0") == TextEncoding::Utf8);
}

TEST_CASE("every CP437 glyph's UTF-8 form reads as that one glyph", "[utf8]") {
    for (int b = 1; b < 256; ++b) {
        // the codes .ans acts on rather than draws
        if (b == '\t' || b == '\n' || b == '\r' || b == 0x1A || b == 0x1B)
            continue;
        const char32_t glyph = cp437ToUnicode(std::uint8_t(b));
        const std::string utf8 = cp437ToUtf8(std::string(1, char(b)));
        CAPTURE(b);
        const AnsiDocument d = parseAs("x" + utf8 + "y", TextEncoding::Utf8);
        CHECK(d.encoding == TextEncoding::Utf8);
        CHECK(row(d, 0, 3) == std::u32string{U'x', glyph, U'y'});
        // and the same as the CP437 byte reads
        CHECK(parseAs(std::string("x") + char(b) + "y", TextEncoding::Cp437).canvas == d.canvas);
        // found without being told, next to a three-byte glyph
        if (b >= 0x80) {
            const AnsiDocument found = parseAnsi("\xE2\x96\x88" + utf8);
            CHECK(found.encoding == TextEncoding::Utf8);
            CHECK(row(found, 0, 2) == std::u32string{U'█', glyph});
        }
    }
}

TEST_CASE("UTF-8 text keeps every code point, not only CP437's", "[utf8]") {
    const AnsiDocument d = parseAnsi("\xE2\x96\x88\xCE\xBB\xE2\x82\xAC\xF0\x9F\x98\x80!");
    REQUIRE(d.encoding == TextEncoding::Utf8);
    CHECK(row(d, 0, 5) == U"█λ€\U0001F600!");
}

TEST_CASE("invalid UTF-8", "[utf8]") {
    SECTION("found: one bad byte makes the whole file CP437") {
        const AnsiDocument d = parseAnsi("\xE2\x96\x88\x80");
        CHECK(d.encoding == TextEncoding::Cp437);
        CHECK(row(d, 0, 4) == U"ΓûêÇ");
    }
    SECTION("forced: each bad byte is one U+FFFD and the rest still reads") {
        struct Case {
            std::string bytes;
            std::u32string cells;
        };
        const std::vector<Case> cases = {
            {"\x80" "A", U"�A"},
            {"\xC0\xAF" "A", U"��A"},                    // overlong /
            {"\xED\xA0\x80" "A", U"���A"},          // a surrogate
            {"\xF4\x90\x80\x80" "A", U"����A"}, // past U+10FFFF
            {"\xE2\x96" "A", U"��A"},                    // cut short
            {"\xE2\x96\x88\xFF\xE2\x96\x88", U"█�█"},
        };
        for (const Case& c : cases) {
            CAPTURE(c.bytes);
            const AnsiDocument d = parseAs(c.bytes, TextEncoding::Utf8);
            CHECK(row(d, 0, int(c.cells.size())) == c.cells);
        }
    }
    SECTION("cut short by the end of the content") {
        const AnsiDocument d = parseAs("A\xE2\x96", TextEncoding::Utf8);
        CHECK(row(d, 0, 3) == U"A��");
    }
}

TEST_CASE("a byte order mark", "[utf8]") {
    SECTION("found: it says UTF-8 and is not a cell") {
        const AnsiDocument d = parseAnsi("\xEF\xBB\xBF" "A\xC3\x87");
        CHECK(d.encoding == TextEncoding::Utf8);
        CHECK(row(d, 0, 3) == U"AÇ ");
    }
    SECTION("followed by bytes that aren't UTF-8: U+FFFD for each") {
        const AnsiDocument d = parseAnsi("\xEF\xBB\xBF\xDB" "A");
        CHECK(d.encoding == TextEncoding::Utf8);
        CHECK(row(d, 0, 2) == U"�A");
    }
    SECTION("forced CP437: three glyphs") {
        const AnsiDocument d = parseAs("\xEF\xBB\xBF" "A", TextEncoding::Cp437);
        CHECK(row(d, 0, 4) == U"∩╗┐A");
    }
    SECTION("only at the start") {
        const AnsiDocument d = parseAs("A\xEF\xBB\xBF", TextEncoding::Utf8);
        CHECK(row(d, 0, 2) == U"A﻿");
    }
}

TEST_CASE("CP437 files with high bytes are not taken for UTF-8", "[utf8]") {
    SECTION("box corners that happen to be valid two-byte UTF-8") {
        const AnsiDocument d = parseAnsi("\xC9\xBB\r\n\xC8\xBC");
        CHECK(d.encoding == TextEncoding::Cp437);
        CHECK(row(d, 0, 2) == U"╔╗");
        CHECK(row(d, 1, 2) == U"╚╝");
    }
    SECTION("every piece in the test data reads exactly as before") {
        int pieces = 0;
        for (const char* sub : {"golden", "render/pieces"}) {
            for (const auto& e : fs::directory_iterator(bwtest::dataDir() / sub)) {
                if (e.path().extension() != ".ans")
                    continue;
                const std::string bytes = bwtest::readFile(e.path());
                if (bytes.empty())
                    continue;
                CAPTURE(e.path().filename().string());
                const AnsiDocument found = parseAnsi(bytes);
                CHECK(found.encoding == TextEncoding::Cp437);
                const AnsiDocument cp437 = parseAs(bytes, TextEncoding::Cp437);
                CHECK(found.canvas == cp437.canvas);
                CHECK(found.ice == cp437.ice);
                ++pieces;
            }
        }
        CHECK(pieces >= 15);
    }
}

TEST_CASE("SAUCE on a UTF-8 file", "[utf8][sauce]") {
    SECTION("a UTF-8 copy of a piece reads as the piece, SAUCE and all") {
        const AnsiDocument cp437 = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden/tux.ans"));
        const AnsiDocument utf8 =
            parseAnsi(bwtest::readFile(bwtest::dataDir() / "utf8/tux-utf8.ans"));
        CHECK(cp437.encoding == TextEncoding::Cp437);
        CHECK(utf8.encoding == TextEncoding::Utf8);
        REQUIRE(utf8.sauce);
        REQUIRE(cp437.sauce);
        CHECK(utf8.sauce->title == cp437.sauce->title);
        CHECK(utf8.sauce->author == cp437.sauce->author);
        CHECK(utf8.sauce->group == cp437.sauce->group);
        CHECK(utf8.sauce->tinfo1 == cp437.sauce->tinfo1);
        CHECK(utf8.sauce->tinfo2 == cp437.sauce->tinfo2);
        CHECK(utf8.ice == cp437.ice);
        CHECK(utf8.canvas == cp437.canvas);
        // written back, it is the CP437 piece
        CHECK(writeAnsi(utf8).bytes == writeAnsi(cp437).bytes);
    }
    SECTION("SAUCE's text stays CP437 bytes and has no say in the detection") {
        Sauce s = Sauce::fresh(80, 1);
        s.title = "\xC9\xCD\xBB caf\x82";  // ╔═╗ café, CP437
        s.author = "\xE2\x96\x88";          // UTF-8 for █, kept as three CP437 bytes
        const AnsiDocument d = parseAnsi(withSauce("\xE2\x96\x88", s));
        CHECK(d.encoding == TextEncoding::Utf8);
        CHECK(row(d, 0, 1) == U"█");
        REQUIRE(d.sauce);
        CHECK(d.sauce->title == s.title);
        CHECK(d.sauce->author == s.author);

        // ASCII art under a SAUCE record holding UTF-8: still CP437
        const AnsiDocument ascii = parseAnsi(withSauce("hi", s));
        CHECK(ascii.encoding == TextEncoding::Cp437);
    }
    SECTION("0x1A ends a UTF-8 file's content") {
        const AnsiDocument d = parseAnsi("\xE2\x96\x88\x1A\xDB\xDB");
        CHECK(d.encoding == TextEncoding::Utf8);
        CHECK(row(d, 0, 2) == U"█ ");
    }
}

TEST_CASE("controls and escapes act the same in UTF-8", "[utf8]") {
    const std::string art = "\x1b[1;31m\xE2\x96\x88\x1b[0m\tA\r\n\x01\x7F\x00" "B"s;
    const AnsiDocument d = parseAs(art, TextEncoding::Utf8);
    CHECK(d.canvas.at(0, 0).ch == U'█');
    CHECK(d.canvas.at(0, 0).fg.index == LightRed);
    CHECK(d.canvas.at(8, 0).ch == U'A');
    CHECK(d.canvas.at(8, 0).fg.index == Grey);
    CHECK(row(d, 1, 4) == U"☺⌂ B");
    // and as they do in CP437 once the glyph is a byte
    const AnsiDocument cp =
        parseAs("\x1b[1;31m\xDB\x1b[0m\tA\r\n\x01\x7F\x00" "B"s,
                TextEncoding::Cp437);
    CHECK(cp.canvas == d.canvas);
}

// ---- writing UTF-8 .ans (#302 part 2) ----

namespace {

// The content writeAnsi wrote, before its SAUCE.
std::string contentOf(const AnsiOutput& out) { return std::string(splitSauce(out.bytes).content); }

AnsiDocument oneRow(std::u32string glyphs) {
    AnsiDocument d;
    d.canvas = Canvas(static_cast<int>(glyphs.size()), 1);
    for (std::size_t x = 0; x < glyphs.size(); ++x)
        d.canvas.at(static_cast<int>(x), 0).ch = glyphs[x];
    return d;
}

} // namespace

TEST_CASE("utf8Writable: everything but C0 and C1 controls and surrogates", "[utf8]") {
    CHECK(utf8Writable(U'\0'));  // written as a space
    CHECK(utf8Writable(U' '));
    CHECK(utf8Writable(U'~'));
    CHECK(utf8Writable(U'é'));
    CHECK(utf8Writable(U'█'));
    CHECK(utf8Writable(U'★'));
    CHECK(utf8Writable(U'\U0001F600'));
    CHECK(utf8Writable(U'\U0010FFFF'));
    for (char32_t c : {U'\x01', U'\t', U'\n', U'\r', U'\x1a', U'\x1b', U'\x1f', U'\x7f'})
        CHECK_FALSE(utf8Writable(c));
    CHECK_FALSE(utf8Writable(char32_t(0xD800)));
    CHECK_FALSE(utf8Writable(char32_t(0xDFFF)));
    CHECK_FALSE(utf8Writable(char32_t(0x110000)));
    // C1: a terminal that honours them takes C2 9B for a CSI
    for (char32_t c = 0x80; c <= 0x9f; ++c)
        CHECK_FALSE(utf8Writable(c));
    CHECK(utf8Writable(char32_t(0x7e)));
    CHECK(utf8Writable(char32_t(0xa0)));  // NBSP, CP437 0xFF
}

TEST_CASE("every CP437 glyph survives a UTF-8 write and read", "[utf8]") {
    AnsiDocument d;
    d.canvas = Canvas(16, 16);
    for (int b = 0; b < 256; ++b)
        d.canvas.at(b % 16, b / 16).ch = cp437ToUnicode(std::uint8_t(b));
    d.canvas.at(0, 0).ch = U'A';  // NUL reads back as a space, so not here
    const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
    CHECK(out.encoding == TextEncoding::Utf8);
    CHECK(out.unwritable.empty());  // ○ ◙ ← → ♪ too, which a CP437 .ans can't hold
    const AnsiDocument back = parseAnsi(out.bytes);
    CHECK(back.encoding == TextEncoding::Utf8);
    CHECK(back.canvas == d.canvas);
    // no byte order mark: the box and block glyphs are three bytes
    CHECK_FALSE(out.bytes.starts_with("\xEF\xBB\xBF"));
    // the CP437 writer loses the five control-code glyphs (#300)
    CHECK(writeAnsi(d).unwritable.size() == 5);
}

TEST_CASE("a CP437 piece written as UTF-8 reads back as the same cells", "[utf8]") {
    for (const auto& e : fs::directory_iterator(bwtest::dataDir() / "golden")) {
        if (e.path().extension() != ".ans")
            continue;
        CAPTURE(e.path().filename().string());
        const AnsiDocument cp437 = parseAnsi(bwtest::readFile(e.path()));
        REQUIRE(cp437.encoding == TextEncoding::Cp437);
        const AnsiOutput out = writeAnsi(cp437, TextEncoding::Utf8);
        CHECK(out.unwritable.empty());
        const AnsiDocument back = parseAnsi(out.bytes);
        CHECK(back.encoding == TextEncoding::Utf8);
        // the cells the CP437 writer gives back, and a CP437 write of them is
        // the CP437 write of the piece
        CHECK(back.canvas == parseAnsi(writeAnsi(cp437).bytes).canvas);
        CHECK(writeAnsi(back).bytes == writeAnsi(cp437).bytes);
        // SAUCE is the piece's, its FileSize the UTF-8 content's length
        REQUIRE(back.sauce);
        REQUIRE(cp437.sauce);
        CHECK(back.sauce->title == cp437.sauce->title);
        CHECK(back.sauce->author == cp437.sauce->author);
        CHECK(back.sauce->fileSize == contentOf(out).size());
    }
}

TEST_CASE("tux written as UTF-8 is the UTF-8 copy of tux", "[utf8]") {
    const AnsiDocument cp437 = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden/tux.ans"));
    CHECK(writeAnsi(cp437, TextEncoding::Utf8).bytes == bwtest::readFile(bwtest::dataDir() / "utf8/tux-utf8.ans"));
    // and the default is still the CP437 file, byte for byte
    CHECK(writeAnsi(cp437).bytes == bwtest::readFile(bwtest::dataDir() / "golden/tux.ans"));
    CHECK(writeAnsi(cp437).encoding == TextEncoding::Cp437);
    CHECK(writeAnsi(cp437, TextEncoding::Cp437).bytes == writeAnsi(cp437).bytes);
}

TEST_CASE("a byte order mark only when the bytes wouldn't say UTF-8", "[utf8]") {
    const std::string bom = "\xEF\xBB\xBF";
    SECTION("ASCII only") {
        const AnsiOutput out = writeAnsi(oneRow(U"hi"), TextEncoding::Utf8);
        CHECK(contentOf(out).starts_with(bom));
        const AnsiDocument back = parseAnsi(out.bytes);
        CHECK(back.encoding == TextEncoding::Utf8);
        CHECK(row(back, 0, 2) == U"hi");
        // the FileSize counts the mark
        REQUIRE(back.sauce);
        CHECK(back.sauce->fileSize == contentOf(out).size());
    }
    SECTION("two-byte sequences only") {
        const AnsiOutput out = writeAnsi(oneRow(U"café"), TextEncoding::Utf8);
        CHECK(contentOf(out).starts_with(bom));
        CHECK(contentOf(out).find("caf\xC3\xA9") != std::string::npos);
        const AnsiDocument back = parseAnsi(out.bytes);
        CHECK(back.encoding == TextEncoding::Utf8);
        CHECK(row(back, 0, 4) == U"café");
    }
    SECTION("a three-byte sequence says it") {
        const AnsiOutput out = writeAnsi(oneRow(U"café█"), TextEncoding::Utf8);
        CHECK_FALSE(out.bytes.starts_with(bom));
        CHECK(contentOf(out).find("caf\xC3\xA9\xE2\x96\x88") != std::string::npos);
        CHECK(parseAnsi(out.bytes).encoding == TextEncoding::Utf8);
    }
    SECTION("an empty canvas") {
        AnsiDocument d;
        d.canvas = Canvas(4, 2);
        const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
        CHECK(out.bytes.starts_with(bom));
        CHECK(parseAnsi(out.bytes).encoding == TextEncoding::Utf8);
    }
}

TEST_CASE("UTF-8 keeps glyphs CP437 has no code for", "[utf8]") {
    const std::u32string glyphs = U"★λ€\U0001F600☺";
    const AnsiOutput out = writeAnsi(oneRow(glyphs), TextEncoding::Utf8);
    CHECK(out.unwritable.empty());
    CHECK(row(parseAnsi(out.bytes), 0, 5) == glyphs);
    // CP437 loses the four with no code
    CHECK(writeAnsi(oneRow(glyphs)).unwritable.size() == 4);
}

TEST_CASE("UTF-8 writes controls, surrogates and NUL safely", "[utf8]") {
    AnsiDocument d = oneRow(U"a\x01" "b\x7f" "c");
    d.canvas.at(4, 0).ch = char32_t(0xD800);
    const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
    REQUIRE(out.unwritable.size() == 3);
    CHECK(out.unwritable[0].x == 1);
    CHECK(out.unwritable[0].ch == U'\x01');
    CHECK(out.unwritable[1].x == 3);
    CHECK(out.unwritable[1].ch == U'\x7f');
    CHECK(out.unwritable[2].x == 4);
    CHECK(out.unwritable[2].ch == char32_t(0xD800));
    CHECK(contentOf(out).find("a?b??") != std::string::npos);

    const AnsiOutput nul = writeAnsi(oneRow(std::u32string{U'x', U'\0', U'y'}), TextEncoding::Utf8);
    CHECK(nul.unwritable.empty());
    CHECK(contentOf(nul).find("x y") != std::string::npos);
}

TEST_CASE("UTF-8 writes a C1 control as '?' and reports it, as the IRC export does", "[utf8]") {
    // #328 review: U+009B went out as C2 9B, which xterm reads as a CSI
    const std::string in = "\xe2\x96\x88\xc2\x9b" "31mX\xc2\x85\xc2\x80\xc2\x9f\xc2\xa0Y\r\n";
    const AnsiDocument d = parseAs(in, TextEncoding::Utf8);
    REQUIRE(d.canvas.at(1, 0).ch == char32_t(0x9b));
    const AnsiOutput out = writeAnsi(d, TextEncoding::Utf8);
    REQUIRE(out.unwritable.size() == 4);
    CHECK(out.unwritable[0].x == 1);
    CHECK(out.unwritable[0].ch == char32_t(0x9b));
    CHECK(out.unwritable[1].ch == char32_t(0x85));
    CHECK(out.unwritable[2].ch == char32_t(0x80));
    CHECK(out.unwritable[3].ch == char32_t(0x9f));
    CHECK(out.lossy());
    const std::string content = contentOf(out);
    CHECK(content.find("\xc2\x9b") == std::string::npos);
    CHECK(content.find("\xc2\x85") == std::string::npos);
    CHECK(content.find("\xc2\x80") == std::string::npos);
    CHECK(content.find("\xc2\x9f") == std::string::npos);
    // what is written reads back: the controls as '?', the rest kept
    CHECK(content.find("\xe2\x96\x88?31mX???\xc2\xa0Y") != std::string::npos);
    const AnsiDocument back = parseAnsi(out.bytes);
    CHECK(back.encoding == TextEncoding::Utf8);
    CHECK(row(back, 0, 10) == U"█?31mX???\u00a0");
}

TEST_CASE("the SAUCE of a UTF-8 .ans is the CP437 record", "[utf8][sauce]") {
    AnsiDocument d = oneRow(U"██");
    Sauce s = Sauce::fresh(2, 1);
    s.title = "\xC9\xCD\xBB caf\x82";  // CP437 bytes, written as they are
    s.author = "funkpower";
    d.sauce = s;
    const AnsiOutput u8 = writeAnsi(d, TextEncoding::Utf8), cp = writeAnsi(d);
    // the same record but for FileSize
    Sauce c = *parseAnsi(cp.bytes).sauce;
    c.fileSize = static_cast<std::uint32_t>(contentOf(u8).size());
    CHECK(splitSauce(u8.bytes).sauce->title == s.title);
    CHECK(u8.bytes.substr(contentOf(u8).size()) == encodeSauce(c, c.fileSize));
}

TEST_CASE("writeArt: XBIN and BIN stay CP437 whatever the .ans encoding", "[utf8][xbin]") {
    const AnsiDocument d = oneRow(U"█░");
    for (ArtFormat f : {ArtFormat::Xbin, ArtFormat::Bin}) {
        CAPTURE(int(f));
        const AnsiOutput u8 = writeArt(d, f, TextEncoding::Utf8);
        CHECK(u8.bytes == writeArt(d, f).bytes);
        CHECK(u8.encoding == TextEncoding::Cp437);
    }
    CHECK(writeArt(d, ArtFormat::Ansi, TextEncoding::Utf8).bytes == writeAnsi(d, TextEncoding::Utf8).bytes);
    CHECK(writeArt(d, ArtFormat::Ansi).bytes == writeAnsi(d).bytes);
}
