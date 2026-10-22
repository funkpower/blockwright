#include "blockwright/cp437.h"

#include <catch2/catch_test_macros.hpp>

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
