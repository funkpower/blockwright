#include "blockwright/cp437.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>

using namespace bw;

TEST_CASE("CP437 round-trips all 256 codes", "[cp437]") {
    std::set<char32_t> seen;
    for (int b = 0; b < 256; ++b) {
        const char32_t cp = cp437ToUnicode(static_cast<std::uint8_t>(b));
        INFO("byte 0x" << std::hex << b);
        auto back = unicodeToCp437(cp);
        REQUIRE(back.has_value());
        CHECK(*back == b);
        seen.insert(cp);
    }
    CHECK(seen.size() == 256);  // no two bytes share a glyph
}

TEST_CASE("CP437 maps the glyphs art depends on", "[cp437]") {
    CHECK(cp437ToUnicode(0xDB) == U'█');
    CHECK(cp437ToUnicode(0xDC) == U'▄');
    CHECK(cp437ToUnicode(0xDF) == U'▀');
    CHECK(cp437ToUnicode(0xDD) == U'▌');
    CHECK(cp437ToUnicode(0xDE) == U'▐');
    CHECK(cp437ToUnicode(0xB0) == U'░');
    CHECK(cp437ToUnicode(0xB1) == U'▒');
    CHECK(cp437ToUnicode(0xB2) == U'▓');
    CHECK(cp437ToUnicode(0xFE) == U'■');
    CHECK(cp437ToUnicode(0xFF) == U' ');
    // low bytes and 0x7F are DOS pictographs, not control characters
    CHECK(cp437ToUnicode(0x01) == U'☺');
    CHECK(cp437ToUnicode(0x07) == U'•');
    CHECK(cp437ToUnicode(0x10) == U'►');
    CHECK(cp437ToUnicode(0x1A) == U'→');
    CHECK(cp437ToUnicode(0x7F) == U'⌂');
    CHECK(cp437ToUnicode('A') == U'A');
}

TEST_CASE("Glyphs CP437 lacks are reported, not turned into '?'", "[cp437]") {
    for (char32_t cp : {U'★', U'…', U'“', U'”', U'€', U'\U0001F600'}) {
        INFO("U+" << std::hex << unsigned(cp));
        CHECK_FALSE(unicodeToCp437(cp).has_value());
    }
    CHECK(unicodeToCp437(U'?') == std::uint8_t('?'));
}

TEST_CASE("Look-alike Greek letters map to the CP437 glyph", "[cp437]") {
    CHECK(unicodeToCp437(U'β') == 0xE1);
    CHECK(unicodeToCp437(U'μ') == 0xE6);
    CHECK(unicodeToCp437(U'ß') == 0xE1);
    CHECK(unicodeToCp437(U'µ') == 0xE6);
}

TEST_CASE("CP437 bytes convert to UTF-8", "[cp437]") {
    CHECK(cp437ToUtf8("A\xDB\xB0") == "A█░");
    std::string s;
    appendUtf8(s, U'\U0001F600');
    CHECK(s == "\xF0\x9F\x98\x80");
}

TEST_CASE("Controls and format characters are quoted by their code point, never raw", "[cp437]") {
    // C0, DEL, C1 (U+009B is a CSI to a terminal that honours C1), the
    // zero-width and bidi marks, embeddings, overrides and isolates, the line
    // and paragraph separators, the byte order mark, a soft hyphen, a tag,
    // a surrogate and past U+10FFFF
    for (const char32_t cp : {0x00u, 0x09u, 0x1Bu, 0x7Fu, 0x80u, 0x85u, 0x9Bu, 0x9Fu, 0xADu, 0x200Bu, 0x200Du, 0x200Eu,
                              0x200Fu, 0x202Au, 0x202Bu, 0x202Cu, 0x202Du, 0x202Eu, 0x2028u, 0x2029u, 0x2060u, 0x2066u,
                              0x2067u, 0x2068u, 0x2069u, 0xFEFFu, 0xE0001u, 0xD800u, 0x110000u}) {
        INFO(std::hex << unsigned(cp));
        CHECK(unprintableCodePoint(cp));
        const std::string q = quotedGlyph(cp);
        CHECK(q.rfind("U+", 0) == 0);
        CHECK(std::all_of(q.begin(), q.end(), [](char c) { return c > 0x20 && c < 0x7F; }));
    }
    CHECK(quotedGlyph(0x9B) == "U+009B");
    CHECK(quotedGlyph(0x202E) == "U+202E");
    CHECK(quotedGlyph(0xE0001) == "U+E0001");
    CHECK(quotedGlyph(0x110000) == "U+110000");
    // what shows is shown as itself
    for (const char32_t cp : {char32_t('A'), U'é', U'★', U'░', U'中', U'\u00A0', U'\U0001F600'}) {
        INFO(std::hex << unsigned(cp));
        CHECK_FALSE(unprintableCodePoint(cp));
        std::string utf8;
        appendUtf8(utf8, cp);
        CHECK(quotedGlyph(cp) == utf8);
    }
}

TEST_CASE("Glyphs that show nothing on their own are quoted by their code point", "[cp437]") {
    // review of #332, N1 and N2: variation selectors (the second half
    // of ❤️), the combining grapheme joiner, a lone combining accent and an
    // enclosing mark, the Hangul fillers, noncharacters and an unassigned code
    // point print as nothing or land on the character before them.
    for (const char32_t cp : {0xFE00u, 0xFE0Eu, 0xFE0Fu, 0xE0100u, 0xE01EFu, 0x034Fu, 0x0301u, 0x0300u, 0x20DDu,
                              0x115Fu, 0x1160u, 0x3164u, 0xFFA0u, 0xFDD0u, 0xFDEFu, 0xFFFEu, 0xFFFFu, 0x1FFFEu,
                              0x10FFFFu, 0x0378u}) {
        INFO(std::hex << unsigned(cp));
        CHECK(unprintableCodePoint(cp));
        const std::string q = quotedGlyph(cp);
        CHECK(q.rfind("U+", 0) == 0);
        CHECK(std::all_of(q.begin(), q.end(), [](char c) { return c > 0x20 && c < 0x7F; }));
    }
    CHECK(quotedGlyph(0xFE0F) == "U+FE0F");
    CHECK(quotedGlyph(0x0301) == "U+0301");
    CHECK(quotedGlyph(0x3164) == "U+3164");
    CHECK(quotedGlyph(0xFFFE) == "U+FFFE");
    CHECK(quotedGlyph(0xE0100) == "U+E0100");
    // the visible ones stay themselves: ❤ (the first half of ❤️), a box line,
    // a Hangul syllable, a spacing mark (U+0903), a private-use glyph
    for (const char32_t cp : {U'\u2764', U'─', U'╬', U'한', U'\u0903', U'\uE0A0', U'z'}) {
        INFO(std::hex << unsigned(cp));
        CHECK_FALSE(unprintableCodePoint(cp));
        std::string utf8;
        appendUtf8(utf8, cp);
        CHECK(quotedGlyph(cp) == utf8);
    }
}
