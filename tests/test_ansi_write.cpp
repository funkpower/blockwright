#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/irc.h"

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

// `body` as a file with SAUCE and its iCE flag off, parsed.
AnsiDocument parseIceOff(const std::string& body) {
    Sauce s = Sauce::fresh(80, 1);
    s.setIce(false);
    return parseAnsi(body + encodeSauce(s, std::uint32_t(body.size())));
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

TEST_CASE("Glyphs whose CP437 byte .ans reads as a control code are told apart", "[write]") {
    // #300: CP437 has ○ ◙ ♪ → ← (0x09 0x0A 0x0D 0x1A 0x1B), but the parser
    // reads those bytes as TAB, LF, CR, SUB and ESC
    CHECK(ansControlByte(U'○') == 0x09);
    CHECK(ansControlByte(U'◙') == 0x0a);
    CHECK(ansControlByte(U'♪') == 0x0d);
    CHECK(ansControlByte(U'→') == 0x1a);
    CHECK(ansControlByte(U'←') == 0x1b);
    CHECK(std::string(ansControlName(0x09)) == "TAB");
    CHECK(std::string(ansControlName(0x0a)) == "LF");
    CHECK(std::string(ansControlName(0x0d)) == "CR");
    CHECK(std::string(ansControlName(0x1a)) == "SUB");
    CHECK(std::string(ansControlName(0x1b)) == "ESC");
    CHECK_FALSE(ansControlByte(U'★'));  // no CP437 code at all
    CHECK_FALSE(ansControlByte(U'•'));  // 0x07 draws
    CHECK_FALSE(ansControlByte(U'A'));
    CHECK_FALSE(ansControlByte(U'\0'));  // NUL is written as a space
    CHECK_FALSE(ansControlByte(U'\t'));  // the codepoint TAB has no CP437 code; ○ is 0x09
    for (int b = 0; b < 256; ++b)
        CHECK((ansControlName(std::uint8_t(b)) != nullptr) ==
              (b == 0x09 || b == 0x0a || b == 0x0d || b == 0x1a || b == 0x1b));

    // the row of #300's repro, with a glyph CP437 has no code for after it
    AnsiDocument doc;
    doc.canvas = Canvas(7, 1);
    const std::u32string row = U"○←◙A★○→";
    for (int x = 0; x < 7; ++x)
        doc.canvas.at(x, 0) = cell(row[std::size_t(x)], White, Black);
    const AnsiOutput out = writeAnsi(doc);
    REQUIRE(out.unwritable.size() == 6);
    CHECK(ansControlCount(out.unwritable) == 5);
    CHECK(ansControlNames(out.unwritable) == "TAB, ESC, LF, SUB");  // first met order, each once
    CHECK(ansControlNames({}).empty());
    CHECK(ansControlNames({{0, 0, U'★'}}).empty());
    CHECK(ansControlCount({{0, 0, U'★'}}) == 0);
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

TEST_CASE("A trailing blinking black space written with iCE on is trimmed as black", "[write]") {
    // #36: the bright background turns iCE on, the blink is dropped, and the
    // two blinking spaces are plain black spaces, trimmed as the editor does.
    // Only A's blink showed (#42): blink on a space shows nothing.
    const AnsiDocument d =
        parseAnsi(withSauce("\x1b[0;5;31;44mA\x1b[0;37;104mB\x1b[0;5;30;40m  ", 80, false));
    REQUIRE_FALSE(d.ice);
    REQUIRE(d.canvas.at(2, 0).attrs == attr::Blink);
    const AnsiOutput out = writeAnsi(d);
    CHECK(out.blinkDropped == 1);
    CHECK(out.bytes.find("B\x1b[0m\r\n") != std::string::npos);
    CHECK(out.bytes.find("\x1b[0;30;40m") == std::string::npos);
    const AnsiDocument back = parseAnsi(out.bytes);
    CHECK(back.ice);
    CHECK(back.canvas.at(2, 0) == Cell{});
    CHECK(back.canvas.at(3, 0) == Cell{});
}

TEST_CASE("Blink on a blank glyph is not lost when iCE is turned on", "[write]") {
    SECTION("#42: blinking spaces next to a bright background are written without loss") {
        Sauce sauce = Sauce::fresh(80, 2);
        sauce.setIce(false);
        const std::string b = "\x1b[0;5;40m   \x1b[0;37;104mB\r\n\x1b[0;5;40m    ";
        const AnsiDocument d = parseAnsi(b + encodeSauce(sauce, static_cast<std::uint32_t>(b.size())));
        REQUIRE_FALSE(d.ice);
        REQUIRE(d.canvas.at(0, 0).attrs == attr::Blink);
        REQUIRE(d.canvas.at(3, 1).attrs == attr::Blink);
        const AnsiOutput out = writeAnsi(d);
        CHECK(out.blinkDropped == 0);
        CHECK_FALSE(out.lossy());
        const AnsiDocument back = parseAnsi(out.bytes);
        CHECK(back.ice);
        for (int x = 0; x < 4; ++x) {
            CHECK(back.canvas.at(x, 1) == Cell{});
            if (x < 3)
                CHECK(back.canvas.at(x, 0) == Cell{});
        }
        CHECK(back.canvas.at(3, 0).bg.index == LightBlue);
    }
    SECTION("a space, NUL and 0xFF are blank; any other glyph's blink is lost") {
        AnsiDocument doc;
        doc.canvas = Canvas(6, 1);
        const char32_t glyphs[] = {U' ', U'\0', U'\u00a0', U'.', U'█'};
        for (int x = 0; x < 5; ++x) {
            Cell c = cell(glyphs[x], Grey, Red);
            c.attrs = attr::Blink;
            doc.canvas.at(x, 0) = c;
        }
        doc.canvas.at(5, 0) = cell(U'B', Grey, LightBlue);
        doc.ice = false;
        const AnsiOutput out = writeAnsi(doc);
        CHECK(out.blinkDropped == 2);
        CHECK(blankGlyph(U' '));
        CHECK(blankGlyph(U'\0'));
        CHECK(blankGlyph(U'\u00a0'));
        CHECK_FALSE(blankGlyph(U'.'));
        CHECK_FALSE(blankGlyph(U'█'));
    }
}

TEST_CASE("openAsEditor turns iCE colours on for a bright background and drops blink as writeAnsi would") {
    // a blinking cell, a bright background, a transparent cell with a
    // bright background that doesn't count
    AnsiDocument doc = parseIceOff("\x1b[0;5;31mA\x1b[0;104mB");
    REQUIRE_FALSE(doc.ice);
    const AnsiOutput writer = writeAnsi(doc);
    REQUIRE(writer.blinkDropped == 1);

    AnsiDocument opened = doc;
    const Opened r = openAsEditor(opened);
    CHECK(r.iceInferred);
    CHECK(r.blinkDropped == 1);
    // #79: the lost cell is marked, and only it
    CHECK(opened.canvas.at(0, 0).lostBlink);
    CHECK_FALSE(opened.canvas.at(1, 0).lostBlink);
    CHECK(opened.ice);
    CHECK(opened.sauce->ice());
    CHECK_FALSE(opened.canvas.at(0, 0).attrs & attr::Blink);
    CHECK(opened.canvas.at(0, 0).bg.paletteIndex() == 0);
    const AnsiOutput out = writeAnsi(opened);
    CHECK(out.bytes == writer.bytes);
    CHECK(out.blinkDropped == 0);

    SECTION("a piece with iCE colours on, or no bright background, is left alone") {
        AnsiDocument on = doc;
        on.ice = true;
        CHECK(openAsEditor(on).blinkDropped == 0);
        CHECK(on.canvas == doc.canvas);
        AnsiDocument dark = parseIceOff("\x1b[0;5;31mA\x1b[0;44mB");
        const AnsiDocument before = dark;
        const Opened d = openAsEditor(dark);
        CHECK_FALSE(d.iceInferred);
        CHECK(d.blinkDropped == 0);
        CHECK_FALSE(dark.ice);
        CHECK(dark.canvas == before.canvas);
    }
    SECTION("a transparent cell's background doesn't count") {
        AnsiDocument t = parseIceOff("\x1b[0;5;31mA\x1b[0;44mB");
        t.canvas.at(2, 0).transparent = true;
        t.canvas.at(2, 0).bg = Color::indexed(12);
        CHECK_FALSE(openAsEditor(t).iceInferred);
        CHECK_FALSE(t.ice);
    }
}

TEST_CASE("openAsEditor loses no blink on a blank glyph, as writeAnsi doesn't", "[write]") {
    SECTION("#42: a blinking black space beside a bright background") {
        AnsiDocument doc = parseIceOff("\x1b[0;5;40m \x1b[0;37;104mB");
        REQUIRE_FALSE(doc.ice);
        REQUIRE(doc.canvas.at(0, 0).attrs == attr::Blink);
        REQUIRE(writeAnsi(doc).blinkDropped == 0);
        const Opened r = openAsEditor(doc);
        CHECK(r.iceInferred);
        CHECK(r.blinkDropped == 0);
        CHECK_FALSE(doc.canvas.at(0, 0).attrs & attr::Blink);
        CHECK_FALSE(writeAnsi(doc).lossy());
    }
    SECTION("only the blink on glyphs that aren't blank is counted") {
        AnsiDocument doc;
        doc.canvas = Canvas(6, 1);
        const char32_t glyphs[] = {U' ', U'\0', U' ', U'.', U'█'};
        for (int x = 0; x < 5; ++x) {
            Cell c = cell(glyphs[x], Grey, Red);
            c.attrs = attr::Blink;
            doc.canvas.at(x, 0) = c;
        }
        doc.canvas.at(5, 0) = cell(U'B', Grey, LightBlue);
        doc.ice = false;
        REQUIRE(writeAnsi(doc).blinkDropped == 2);
        const Opened r = openAsEditor(doc);
        CHECK(r.blinkDropped == 2);
        for (int x = 0; x < 6; ++x)
            CHECK(doc.canvas.at(x, 0).lostBlink == (x == 3 || x == 4));
        for (int x = 0; x < 5; ++x)
            CHECK_FALSE(doc.canvas.at(x, 0).attrs & attr::Blink);
    }
}

TEST_CASE("the lost-blink mark is the editor's own: == and the encoders leave it out") {
    // #82: Cell::lostBlink never reaches a file, an IRC export or a cell
    // comparison; identical() is the one comparison that sees it
    Cell a = cell(U'A', Red, Blue);
    Cell b = a;
    b.lostBlink = true;
    CHECK(a == b);
    CHECK_FALSE(identical(a, b));
    CHECK(identical(b, b));

    AnsiDocument doc = parseIceOff("\x1b[0;5;31;44mA\x1b[0;37;104mB\r\n\x1b[0;33;41mCD");
    AnsiDocument marked = doc;
    for (int y = 0; y < marked.canvas.height(); ++y)
        for (int x = 0; x < marked.canvas.width(); ++x)
            marked.canvas.at(x, y).lostBlink = true;
    CHECK(marked.canvas == doc.canvas);
    const AnsiOutput plain = writeAnsi(doc), withMarks = writeAnsi(marked);
    CHECK(withMarks.bytes == plain.bytes);
    CHECK(withMarks.blinkDropped == plain.blinkDropped);
    CHECK_FALSE(parseAnsi(withMarks.bytes).canvas.at(0, 0).lostBlink);
    CHECK(irc::encodeCanvas(marked.canvas) == irc::encodeCanvas(doc.canvas));
}

TEST_CASE("writesAlike compares two cells as writeAnsi writes them") {
    // #82 round 1 (P3): by CP437 byte, not codepoint
    const Cell a = cell(U'ß', Red, Blue);
    Cell beta = a;
    beta.ch = U'β';
    CHECK(writesAlike(a, beta));
    CHECK(writesAlike(cell(U'µ', Red, Blue), cell(U'μ', Red, Blue)));
    CHECK_FALSE(writesAlike(a, cell(U'B', Red, Blue)));
    // glyphs writeAnsi writes as '?'
    CHECK(writesAlike(cell(U'★', Red, Blue), cell(U'?', Red, Blue)));
    CHECK(writesAlike(cell(U'\t', Red, Blue), cell(U'?', Red, Blue)));
    // colours by palette index, a 24-bit one as its nearest
    Cell rgb = a;
    rgb.fg = Color::fromRgb(kPalette[Red]);
    CHECK(writesAlike(a, rgb));
    CHECK_FALSE(writesAlike(a, cell(U'ß', LightRed, Blue)));
    CHECK_FALSE(writesAlike(a, cell(U'ß', Red, LightBlue)));
    // only the blink bit, and not the mark
    Cell other = a;
    other.attrs = 0x80;
    other.lostBlink = true;
    CHECK(writesAlike(a, other));
    Cell blink = a;
    blink.attrs = attr::Blink;
    CHECK_FALSE(writesAlike(a, blink));
    Cell hole = a;
    hole.transparent = true;
    CHECK_FALSE(writesAlike(a, hole));
    CHECK(writesAlike(hole, Cell{U' ', {}, {}, 0, true}));
}
