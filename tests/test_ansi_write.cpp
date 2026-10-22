#include "util.h"

#include "blockwright/ansi.h"

#include <catch2/catch_test_macros.hpp>

using namespace bw;

namespace {

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    return c;
}

std::string body(const std::string& out) { return out.substr(0, out.find('\x1a')); }

std::string withSauce(const std::string& body, int width, bool ice) {
    Sauce s = Sauce::fresh(width, 1);
    s.setIce(ice);
    return body + encodeSauce(s, static_cast<std::uint32_t>(body.size()));
}

} // namespace

TEST_CASE("Writer follows the canonical conventions", "[write]") {
    AnsiDocument doc;
    doc.canvas = Canvas(6, 4);
    doc.canvas.at(0, 0) = cell(U'A', Red, Black);
    doc.canvas.at(1, 0) = cell(U'B', Red, Black);
    // row 1 left blank
    for (int x = 0; x < 6; ++x)
        doc.canvas.at(x, 2) = cell(U'X', Yellow, Blue);
    doc.canvas.at(0, 3) = cell(U' ', Grey, Blue);

    const std::string expected =
        "\x1b[0m"
        "\x1b[0;31;40mAB\x1b[0m\r\n"            // trailing blanks trimmed
        " \x1b[0m\r\n"                          // an empty row is one space
        "\x1b[0;1;33;44mXXXXXX\x1b[0m"          // full width: no CR LF, it wraps
        "\x1b[0;37;44m \x1b[0m\r\n";
    CHECK(body(writeAnsi(doc).bytes) == expected);
}

TEST_CASE("SGR is written only when the colours change, and restarts each row", "[write]") {
    AnsiDocument doc;
    doc.canvas = Canvas(4, 2);
    doc.canvas.at(0, 0) = cell(U'a', Green, Black);
    doc.canvas.at(1, 0) = cell(U'b', Green, Black);
    doc.canvas.at(2, 0) = cell(U'c', LightGreen, LightRed);
    doc.canvas.at(0, 1) = cell(U'd', LightGreen, LightRed);
    CHECK(body(writeAnsi(doc).bytes) ==
          "\x1b[0m"
          "\x1b[0;32;40mab\x1b[0;1;5;32;41mc\x1b[0m\r\n"
          "\x1b[0;1;5;32;41md\x1b[0m\r\n");
}

TEST_CASE("Trailing-black trim on write", "[write]") {
    AnsiDocument doc;
    doc.canvas = Canvas(8, 1);
    doc.canvas.at(0, 0) = cell(U'#', White, Black);
    doc.canvas.at(1, 0) = cell(U' ', Red, Blue);    // coloured space: kept
    doc.canvas.at(2, 0) = cell(U' ', Red, Black);   // black-background spaces: trimmed
    doc.canvas.at(3, 0) = cell(U' ', White, Black); // whatever their foreground
    Cell t = cell(U'Q', White, Red);
    t.transparent = true;                           // transparent counts as blank
    doc.canvas.at(4, 0) = t;
    SECTION("trailing spaces on black go") {
        CHECK(body(writeAnsi(doc).bytes) == "\x1b[0m\x1b[0;1;37;40m#\x1b[0;31;44m \x1b[0m\r\n");
    }
    SECTION("a black-on-black full block is not blank, so it pads the row") {
        doc.canvas.at(7, 0) = cell(U'█', Black, Black);
        CHECK(body(writeAnsi(doc).bytes) ==
              "\x1b[0m\x1b[0;1;37;40m#\x1b[0;31;44m \x1b[0;31;40m \x1b[0;1;37;40m \x1b[0;37;40m   "
              "\x1b[0;30;40m\xDB\x1b[0m");  // full width: no CR LF
    }
    SECTION("an all-blank row is a single space") {
        Canvas c(8, 1);
        doc.canvas = c;
        CHECK(body(writeAnsi(doc).bytes) == "\x1b[0m \x1b[0m\r\n");
    }
}

TEST_CASE("Written output parses back to the same grid", "[write]") {
    AnsiDocument doc;
    doc.canvas = Canvas(5, 3);
    const char32_t glyphs[] = {U'█', U'▀', U'▄', U'░', U'▒', U'▓', U'☺', U'⌂', U'é', U'A'};
    int i = 0;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 5; ++x, ++i)
            doc.canvas.at(x, y) = cell(glyphs[i % 10], static_cast<std::uint8_t>(i % 16),
                                       static_cast<std::uint8_t>((i * 7 + 1) % 16));
    auto back = parseAnsi(writeAnsi(doc).bytes);
    CHECK(back.canvas == doc.canvas);
}

TEST_CASE("Glyphs with no CP437 byte are written as '?' and reported", "[write]") {
    AnsiDocument doc;
    doc.canvas = Canvas(6, 1);
    doc.canvas.at(0, 0) = cell(U'★', White, Black);
    doc.canvas.at(1, 0) = cell(U'…', White, Black);
    doc.canvas.at(2, 0) = cell(U'→', White, Black);  // CP437 0x1A is EOF in a file
    doc.canvas.at(3, 0) = cell(U'◙', White, Black);  // 0x0A is LF
    doc.canvas.at(4, 0) = cell(U'•', White, Black);  // 0x07 draws fine
    const AnsiOutput out = writeAnsi(doc);
    REQUIRE(out.unwritable.size() == 4);
    CHECK(out.unwritable[0].x == 0);
    CHECK(out.unwritable[0].ch == U'★');
    CHECK(out.unwritable[2].ch == U'→');
    CHECK(out.unwritable[3].x == 3);
    CHECK(body(out.bytes) == "\x1b[0m\x1b[0;1;37;40m????\x07\x1b[0m\r\n");
}

TEST_CASE("RGB cells are quantised when written", "[write]") {
    AnsiDocument doc;
    doc.canvas = Canvas(2, 1);
    Cell c;
    c.ch = U'A';
    c.fg = Color::fromRgb({250, 250, 80});
    c.bg = Color::fromRgb({0, 0, 150});
    doc.canvas.at(0, 0) = c;
    CHECK(body(writeAnsi(doc).bytes) == "\x1b[0m\x1b[0;1;33;44mA\x1b[0m\r\n");
}

TEST_CASE("Writer updates the SAUCE size and iCE from the document", "[write]") {
    AnsiDocument doc = bwtest::parse("abc", 3);
    doc.sauce = Sauce::fresh(80, 25);
    doc.sauce->title = "kept";
    doc.ice = false;
    const std::string file = writeAnsi(doc).bytes;
    auto r = splitSauce(file);
    REQUIRE(r.sauce);
    CHECK(r.sauce->title == "kept");
    CHECK(r.sauce->tinfo1 == 3);
    CHECK(r.sauce->tinfo2 == 1);
    CHECK_FALSE(r.sauce->ice());
}

TEST_CASE("Bright backgrounds with iCE off", "[write]") {
    SECTION("bright backgrounds alone: written with iCE on, nothing lost") {
        const AnsiDocument d = parseAnsi(withSauce("\x1b[104mA", 80, false));
        REQUIRE_FALSE(d.ice);
        REQUIRE(d.canvas.at(0, 0).bg.index == LightBlue);
        const AnsiOutput out = writeAnsi(d);
        CHECK(out.blinkDropped == 0);
        const AnsiDocument back = parseAnsi(out.bytes);
        CHECK(back.ice);
        CHECK(back.canvas.at(0, 0).bg.index == LightBlue);
        CHECK(back.canvas.at(0, 0).attrs == 0);
        CHECK(back.canvas == d.canvas);
    }
    SECTION("blink alone: stays iCE off") {
        const AnsiDocument d = parseAnsi(withSauce("\x1b[5;41mA", 80, false));
        const AnsiOutput out = writeAnsi(d);
        CHECK(out.blinkDropped == 0);
        const AnsiDocument back = parseAnsi(out.bytes);
        CHECK_FALSE(back.ice);
        CHECK(back.canvas.at(0, 0).attrs == attr::Blink);
        CHECK(back.canvas.at(0, 0).bg.index == Red);
    }
    SECTION("both: iCE on keeps the colours and loses the blink, reported") {
        const AnsiDocument d = parseAnsi(withSauce("\x1b[5;41mA\x1b[0;104mB\x1b[5mC", 80, false));
        const AnsiOutput out = writeAnsi(d);
        CHECK(out.blinkDropped == 2);
        const AnsiDocument back = parseAnsi(out.bytes);
        CHECK(back.ice);
        CHECK(back.canvas.at(0, 0).bg.index == Red);
        CHECK(back.canvas.at(0, 0).attrs == 0);
        CHECK(back.canvas.at(1, 0).bg.index == LightBlue);
        CHECK(back.canvas.at(2, 0).bg.index == LightBlue);
        CHECK(back.canvas.at(2, 0).attrs == 0);
    }
}
