#include "util.h"

#include "blockwright/ansi.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace bw;
using bwtest::parse;

namespace {

std::string withSauce(const std::string& body, int width, bool ice) {
    Sauce s = Sauce::fresh(width, 1);
    s.setIce(ice);
    return body + encodeSauce(s, static_cast<std::uint32_t>(body.size()));
}

} // namespace

TEST_CASE("Bold makes the foreground bright", "[parse]") {
    auto d = parse("\x1b[0;31mA\x1b[1mB\x1b[22mC\x1b[0;1;34mD");
    CHECK(d.canvas.at(0, 0).fg.index == Red);
    CHECK(d.canvas.at(1, 0).fg.index == LightRed);
    CHECK(d.canvas.at(2, 0).fg.index == Red);
    CHECK(d.canvas.at(3, 0).fg.index == LightBlue);
    // bold set before the colour still brightens it
    auto e = parse("\x1b[1;32mx");
    CHECK(e.canvas.at(0, 0).fg.index == LightGreen);
}

TEST_CASE("iCE vs blink", "[parse]") {
    SECTION("iCE on: blink brightens the background, no blink attribute") {
        auto d = parseAnsi(withSauce("\x1b[0;5;41mA\x1b[25mB", 80, true));
        REQUIRE(d.ice);
        CHECK(d.canvas.at(0, 0).bg.index == LightRed);
        CHECK(d.canvas.at(0, 0).attrs == 0);
        CHECK(d.canvas.at(1, 0).bg.index == Red);
    }
    SECTION("iCE off: blink is an attribute, the background stays dark") {
        auto d = parseAnsi(withSauce("\x1b[0;5;41mA\x1b[25mB", 80, false));
        REQUIRE_FALSE(d.ice);
        CHECK(d.canvas.at(0, 0).bg.index == Red);
        CHECK(d.canvas.at(0, 0).attrs == attr::Blink);
        CHECK(d.canvas.at(1, 0).attrs == 0);
    }
    SECTION("no SAUCE: iCE is assumed") {
        auto d = parse("\x1b[5;46mA");
        CHECK(d.ice);
        CHECK(d.canvas.at(0, 0).bg.index == LightCyan);
    }
    SECTION("iCE off round-trips the blink attribute") {
        auto d = parseAnsi(withSauce("\x1b[0;5;41mA\x1b[0m\r\n", 80, false));
        auto again = parseAnsi(writeAnsi(d).bytes);
        CHECK_FALSE(again.ice);
        CHECK(again.canvas == d.canvas);
    }
}

TEST_CASE("SGR 7 reverses the colours until 27 or 0", "[parse]") {
    auto d = parse("\x1b[31;44;7mA\x1b[27mB\x1b[7mC\x1b[0mD");
    CHECK(d.canvas.at(0, 0).fg.index == Blue);
    CHECK(d.canvas.at(0, 0).bg.index == Red);
    CHECK(d.canvas.at(1, 0).fg.index == Red);
    CHECK(d.canvas.at(1, 0).bg.index == Blue);
    CHECK(d.canvas.at(2, 0).fg.index == Blue);
    CHECK(d.canvas.at(3, 0).fg.index == Grey);
    CHECK(d.canvas.at(3, 0).bg.index == Black);
    SECTION("bold stays on the foreground, as libansilove") {
        auto e = parse("\x1b[1;31;44;7mA");
        CHECK(e.canvas.at(0, 0).fg.index == LightBlue);
        CHECK(e.canvas.at(0, 0).bg.index == Red);
    }
    SECTION("an iCE bright background becomes a bright foreground") {
        auto e = parse("\x1b[5;31;44;7mA");
        CHECK(e.canvas.at(0, 0).fg.index == LightBlue);
        CHECK(e.canvas.at(0, 0).bg.index == Red);
    }
    SECTION("bold with an iCE bright background stays a bright foreground, not past the palette") {
        // libansilove adds the bright bits and reaches index 20; the
        // foreground is bright if either colour was
        auto e = parse("\x1b[1;5;31;44;7mA");
        CHECK(e.canvas.at(0, 0).fg.index == LightBlue);
        CHECK(e.canvas.at(0, 0).bg.index == Red);
    }
    SECTION("PabloDraw 24-bit colours are ignored while reversed, as libansilove") {
        auto e = parse("\x1b[31;44m\x1b[1;170;170;170t\x1b[7mA\x1b[27mB");
        CHECK(e.canvas.at(0, 0).fg.index == Blue);
        CHECK(e.canvas.at(0, 0).bg.index == Red);
        CHECK(e.canvas.at(1, 0).fg.index == Grey);  // and come back after 27
        CHECK(e.canvas.at(1, 0).bg.index == Blue);
        auto f = parse("\x1b[31;44m\x1b[0;0;170;0t\x1b[7mA");
        CHECK(f.canvas.at(0, 0).fg.index == Blue);
        CHECK(f.canvas.at(0, 0).bg.index == Red);
        // an SGR colour set after CSI t replaces it and is reversed as usual
        auto g = parse("\x1b[31;44m\x1b[1;170;170;170t\x1b[38;5;15;7mA");
        CHECK(g.canvas.at(0, 0).fg.index == LightBlue);
        CHECK(g.canvas.at(0, 0).bg.index == Grey);
    }
}

TEST_CASE("Where libansilove leaves the palette or the screen, we stay in it", "[parse]") {
    // PLAN §8: libansilove adds 8 on every SGR 1 (index 23 after 1;1) and
    // computes row and column -1 for CUP 0
    auto d = parse("\x1b[1;1mA\x1b[0;5;5;44mB");
    CHECK(d.canvas.at(0, 0).fg.index == White);
    CHECK(d.canvas.at(1, 0).bg.index == LightBlue);
    auto e = parse("xy\x1b[0;0HA");
    CHECK(e.canvas.at(0, 0).ch == U'A');
}

TEST_CASE("aixterm bright colours 90-97 and 100-107", "[parse]") {
    auto d = parse("\x1b[93;104mA");
    CHECK(d.canvas.at(0, 0).fg.index == Yellow);
    CHECK(d.canvas.at(0, 0).bg.index == LightBlue);
}

TEST_CASE("Cursor movement used by real art", "[parse]") {
    SECTION("CUF skips cells, leaving them blank") {
        auto d = parse("A\x1b[3CB");
        CHECK(d.canvas.at(0, 0).ch == U'A');
        CHECK(d.canvas.at(1, 0).ch == U' ');
        CHECK(d.canvas.at(4, 0).ch == U'B');
    }
    SECTION("CUF past the right edge stops at the width, so the next glyph wraps, as libansilove") {
        // libansilove (loaders/ansi.c:314-317) clamps the column to the width,
        // not the last column
        auto d = parse("\x1b[200CX", 80);
        REQUIRE(d.canvas.height() == 2);
        CHECK(d.canvas.at(79, 0).ch == U' ');
        CHECK(d.canvas.at(0, 1).ch == U'X');
        auto e = parse("\x1b[79CX\x1b[CY", 80);
        CHECK(e.canvas.at(79, 0).ch == U'X');
        CHECK(e.canvas.at(1, 1).ch == U'Y');
    }
    SECTION("a cursor move after a full row wraps first, as libansilove") {
        // libansilove wraps on the ESC (loaders/ansi.c:152-155), then moves:
        // rd-theylive.ans puts its signature 7 columns into the next row so
        auto d = parse(std::string(80, 'x') + "\x1b[7Crd!", 80);
        REQUIRE(d.canvas.height() == 2);
        CHECK(d.canvas.at(79, 0).ch == U'x');
        CHECK(d.canvas.at(7, 1).ch == U'r');
        auto e = parse(std::string(80, 'x') + "\x1b[AY", 80);  // up from the wrapped row
        REQUIRE(e.canvas.height() == 1);
        CHECK(e.canvas.at(0, 0).ch == U'Y');
        auto f = parse(std::string(80, 'x') + "\x1b[sZ\x1b[uY", 80);  // saves the wrapped position
        CHECK(f.canvas.at(0, 1).ch == U'Y');
    }
    SECTION("CUB, CUU, CUD") {
        auto d = parse("ABC\x1b[2DZ\x1b[2BQ\x1b[1AR");
        CHECK(d.canvas.at(1, 0).ch == U'Z');
        CHECK(d.canvas.at(2, 2).ch == U'Q');
        CHECK(d.canvas.at(3, 1).ch == U'R');
        auto e = parse("\x1b[5DA\x1b[9AB");  // clamped at the top-left
        CHECK(e.canvas.at(0, 0).ch == U'A');
        CHECK(e.canvas.at(1, 0).ch == U'B');
    }
    SECTION("CUP is 1-based and defaults to home; a column past the width wraps, as libansilove") {
        auto d = parse("\x1b[3;5HX\x1b[HY\x1b[2;999fZ", 20);
        CHECK(d.canvas.at(4, 2).ch == U'X');
        CHECK(d.canvas.at(0, 0).ch == U'Y');
        CHECK(d.canvas.at(19, 1).ch == U' ');
        CHECK(d.canvas.at(0, 2).ch == U'Z');
    }
    SECTION("save and restore") {
        auto d = parse("AB\x1b[s\r\nCD\x1b[uE");
        CHECK(d.canvas.at(2, 0).ch == U'E');
        CHECK(d.canvas.at(1, 1).ch == U'D');
    }
}

TEST_CASE("A glyph in the last column wraps lazily", "[parse]") {
    SECTION("the next glyph starts the next row") {
        auto d = parse("ABCDE", 4);
        REQUIRE(d.canvas.height() == 2);
        CHECK(d.canvas.at(3, 0).ch == U'D');
        CHECK(d.canvas.at(0, 1).ch == U'E');
    }
    SECTION("divergence from libansilove: CR LF after a full row ends that row, not one more") {
        // libansilove wraps on the CR and adds a row for the LF, so E would be
        // on row 2. 21 real pieces (the avatars, comic-lulzy) end full-width
        // rows this way, with or without an SGR before the CR LF (PLAN §8).
        auto d = parse("ABCD\r\nE", 4);
        REQUIRE(d.canvas.height() == 2);
        CHECK(d.canvas.at(0, 1).ch == U'E');
        auto e = parse("ABCD\x1b[0m\r\nE", 4);
        REQUIRE(e.canvas.height() == 2);
        CHECK(e.canvas.at(0, 1).ch == U'E');
        auto f = parse("ABCD\nE", 4);
        CHECK(f.canvas.at(0, 1).ch == U'E');
    }
    SECTION("only a glyph's wrap is ended by CR LF: after CUF or TAB to the edge it adds a row, as libansilove") {
        auto d = parse("A\x1b[9C\r\nE", 4);
        REQUIRE(d.canvas.height() == 3);
        CHECK(d.canvas.at(0, 2).ch == U'E');
        auto e = parse("ABCDEFGH\t\r\nI", 8);
        CHECK(e.canvas.at(0, 3).ch == U'I');
    }
    SECTION("SGR doesn't cancel the pending wrap") {
        auto d = parse("ABCD\x1b[0m\x1b[31mE", 4);
        CHECK(d.canvas.at(0, 1).ch == U'E');
        CHECK(d.canvas.at(0, 1).fg.index == Red);
    }
    SECTION("only CR, SGR and CSI t keep a full row open for CR LF; any other escape wraps, as libansilove") {
        // libansilove wraps on every ESC (loaders/ansi.c:152-155)
        auto d = parse(std::string(80, 'X') + "\x1b[K\r\nY", 80);
        REQUIRE(d.canvas.height() == 3);
        CHECK(d.canvas.at(0, 2).ch == U'Y');
        auto e = parse(std::string(80, 'X') + "\x1b\r\nY", 80);  // a lone ESC
        CHECK(e.canvas.height() == 3);
        auto f = parse(std::string(80, 'X') + "\x1b[?7h\r\nY", 80);  // an unknown sequence
        CHECK(f.canvas.height() == 3);
        auto g = parse(std::string(80, 'X') + "\x1b[1;255;0;0t\r\x1b[0m\r\nY", 80);
        REQUIRE(g.canvas.height() == 2);
        CHECK(g.canvas.at(0, 1).ch == U'Y');
    }
    SECTION("the SAUCE width sets the wrap column") {
        auto d = parseAnsi(withSauce(std::string(45, 'x'), 40, true));
        REQUIRE(d.canvas.width() == 40);
        CHECK(d.canvas.height() == 2);
        CHECK(d.canvas.at(4, 1).ch == U'x');
        CHECK(d.canvas.at(5, 1).ch == U' ');
    }
}

TEST_CASE("CR, LF and TAB", "[parse]") {
    auto d = parse("AB\nD\tE");
    CHECK(d.canvas.at(0, 1).ch == U'D');
    CHECK(d.canvas.at(8, 1).ch == U'E');
}

TEST_CASE("A bare CR doesn't move the cursor, as libansilove", "[parse]") {
    // libansilove (loaders/ansi.c:164-165) ignores CR; only LF goes to column 0
    auto d = parse("AB\rC");
    CHECK(d.canvas.at(0, 0).ch == U'A');
    CHECK(d.canvas.at(1, 0).ch == U'B');
    CHECK(d.canvas.at(2, 0).ch == U'C');
}

TEST_CASE("TAB at the right edge wraps the next glyph, as libansilove", "[parse]") {
    // libansilove (loaders/ansi.c:166-167) doesn't clamp the tab stop
    auto d = parse(std::string(76, 'x') + "\tY", 80);
    REQUIRE(d.canvas.height() == 2);
    CHECK(d.canvas.at(79, 0).ch == U' ');
    CHECK(d.canvas.at(0, 1).ch == U'Y');
    auto e = parse(std::string(28, 'x') + "\tY", 30);  // a stop past the width
    CHECK(e.canvas.at(0, 1).ch == U'Y');
}

TEST_CASE("An SGR with no fields (CSI m) does nothing and empty SGR fields are skipped, as libansilove", "[parse]") {
    // libansilove splits SGR with strtok (loaders/ansi.c:399), which yields
    // nothing for ESC[m and skips empty fields
    auto d = parse("\x1b[1;44m\x1b[mA");
    CHECK(d.canvas.at(0, 0).fg.index == White);
    CHECK(d.canvas.at(0, 0).bg.index == Blue);
    auto e = parse("\x1b[1;44m\x1b[;31mA\x1b[31;;mB");
    CHECK(e.canvas.at(0, 0).fg.index == LightRed);
    CHECK(e.canvas.at(0, 0).bg.index == Blue);
    CHECK(e.canvas.at(1, 0).fg.index == LightRed);
    CHECK(e.canvas.at(1, 0).bg.index == Blue);
    auto f = parse("\x1b[1;44m\x1b[0mA");  // an explicit 0 still resets
    CHECK(f.canvas.at(0, 0).fg.index == Grey);
    CHECK(f.canvas.at(0, 0).bg.index == Black);
}

TEST_CASE("A private or out-of-range SGR field is skipped, not a reset (libansilove resets)", "[parse]") {
    // PLAN §8: libansilove's strtonum fails on these and gives 0
    auto d = parse("\x1b[31m\x1b[999;?999mA");
    CHECK(d.canvas.at(0, 0).fg.index == Red);
    auto e = parse("\x1b[31m\x1b[?999mA");
    CHECK(e.canvas.at(0, 0).fg.index == Red);
    auto f = parse("\x1b[31m\x1b[4294967296mA");
    CHECK(f.canvas.at(0, 0).fg.index == Red);
    auto g = parse("\x1b[31m\x1b[?0;44;>1mA");  // the other fields still apply
    CHECK(g.canvas.at(0, 0).fg.index == Red);
    CHECK(g.canvas.at(0, 0).bg.index == Blue);
    // colon-form colours aren't plain digits either: skipped, the rest apply
    auto h = parse("\x1b[31m\x1b[38:5:2;44mA\x1b[48:2:0:255:0;1mB");
    CHECK(h.canvas.at(0, 0).fg.index == Red);
    CHECK(h.canvas.at(0, 0).bg.index == Blue);
    CHECK(h.canvas.at(1, 0).fg.index == LightRed);
    CHECK(h.canvas.at(1, 0).bg.index == Blue);
}

TEST_CASE("A cursor move with more than one parameter uses the first (libansilove moves 1)", "[parse]") {
    // PLAN §8: libansilove's strtonum fails on "2;5" and falls back to 1
    auto d = parse("A\x1b[2;5CB");
    CHECK(d.canvas.at(2, 0).ch == U' ');
    CHECK(d.canvas.at(3, 0).ch == U'B');
    auto e = parse("\x1b[3;9BA");
    CHECK(e.canvas.at(0, 3).ch == U'A');
}

TEST_CASE("A CSI longer than 14 bytes still acts (libansilove draws it as text)", "[parse]") {
    // PLAN §8: libansilove looks for the final byte in the first 14 bytes only
    // (ANSI_SEQUENCE_MAX_LENGTH, loaders/ansi.c:40, 220)
    auto d = parse("\x1b[0;0;0;0;0;0;31mA");
    REQUIRE(d.canvas.height() == 1);
    CHECK(d.canvas.at(0, 0).ch == U'A');
    CHECK(d.canvas.at(0, 0).fg.index == Red);
    CHECK(d.canvas.at(1, 0).ch == U' ');
}

TEST_CASE("An unknown final byte ends the CSI (libansilove scans on for a known one)", "[parse]") {
    // PLAN §8: libansilove would take the A as cursor up and draw only B
    auto d = parse("\x1b[1zAB");
    CHECK(d.canvas.at(0, 0).ch == U'A');
    CHECK(d.canvas.at(1, 0).ch == U'B');
}

TEST_CASE("SGR 21, 22, 25, 39 and 49 keep their usual meaning (libansilove ignores them)", "[parse]") {
    // PLAN §8: libansilove has no case for them; we follow ECMA-48 / ANSI.SYS
    // practice rather than drop them
    auto d = parse("\x1b[1;31mA\x1b[22mB\x1b[1m\x1b[21mC");
    CHECK(d.canvas.at(0, 0).fg.index == LightRed);
    CHECK(d.canvas.at(1, 0).fg.index == Red);
    CHECK(d.canvas.at(2, 0).fg.index == Red);
    auto e = parse("\x1b[5;44mA\x1b[25mB");
    CHECK(e.canvas.at(0, 0).bg.index == LightBlue);
    CHECK(e.canvas.at(1, 0).bg.index == Blue);
    auto f = parse("\x1b[32;44mA\x1b[39mB\x1b[49mC");
    CHECK(f.canvas.at(1, 0).fg.index == Grey);
    CHECK(f.canvas.at(1, 0).bg.index == Blue);
    CHECK(f.canvas.at(2, 0).bg.index == Black);
}

TEST_CASE("CP437 bytes become Unicode glyphs", "[parse]") {
    auto d = parse("\xDB\xDC\xDF\xB0\x01\x7F");
    CHECK(d.canvas.at(0, 0).ch == U'█');
    CHECK(d.canvas.at(1, 0).ch == U'▄');
    CHECK(d.canvas.at(2, 0).ch == U'▀');
    CHECK(d.canvas.at(3, 0).ch == U'░');
    CHECK(d.canvas.at(4, 0).ch == U'☺');
    CHECK(d.canvas.at(5, 0).ch == U'⌂');
}

TEST_CASE("PabloDraw 24-bit CSI t is quantised to the palette", "[parse]") {
    SECTION("foreground and background") {
        auto d = parse("\x1b[1;250;80;90t\x1b[0;10;10;160tA");
        CHECK(d.canvas.at(0, 0).fg.isIndexed());
        CHECK(d.canvas.at(0, 0).fg.index == LightRed);
        CHECK(d.canvas.at(0, 0).bg.index == Blue);
    }
    SECTION("exact palette colours land on their own index") {
        for (std::uint8_t i = 1; i < 16; ++i) {
            const Rgb c = kPalette[i];
            std::string s = "\x1b[1;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";" +
                            std::to_string(c.b) + "tA";
            CHECK(parse(s).canvas.at(0, 0).fg.index == i);
        }
    }
    SECTION("SGR 0 drops the 24-bit colour; 3x replaces it") {
        auto d = parse("\x1b[1;255;255;255tA\x1b[0mB\x1b[1;255;255;255t\x1b[32mC");
        CHECK(d.canvas.at(0, 0).fg.index == White);
        CHECK(d.canvas.at(1, 0).fg.index == Grey);
        CHECK(d.canvas.at(2, 0).fg.index == Green);
    }
    SECTION("SGR 1 drops the 24-bit foreground and SGR 5 the background, as libansilove") {
        // libansilove (loaders/ansi.c:413-427) clears foreground24 on SGR 1 and
        // background24 on SGR 5, whatever the iCE flag
        auto d = parse("\x1b[31m\x1b[1;0;170;0tA\x1b[1mB");
        CHECK(d.canvas.at(0, 0).fg.index == Green);
        CHECK(d.canvas.at(1, 0).fg.index == LightRed);
        auto e = parse("\x1b[44m\x1b[0;0;170;0tA\x1b[5mB");
        CHECK(e.canvas.at(0, 0).bg.index == Green);
        CHECK(e.canvas.at(1, 0).bg.index == LightBlue);
        // each drops only its own side
        auto f = parse("\x1b[1;0;170;0t\x1b[0;170;0;0t\x1b[1mA\x1b[5mB");
        CHECK(f.canvas.at(0, 0).fg.index == White);
        CHECK(f.canvas.at(0, 0).bg.index == Red);
        CHECK(f.canvas.at(1, 0).fg.index == White);
        CHECK(f.canvas.at(1, 0).bg.index == DarkGrey);
        // a CSI t after the SGR still applies
        auto g = parse("\x1b[1m\x1b[1;0;170;0tA");
        CHECK(g.canvas.at(0, 0).fg.index == Green);
    }
    SECTION("iCE off: SGR 5 blinks and still drops the 24-bit background") {
        auto d = parseAnsi(withSauce("\x1b[44m\x1b[0;0;170;0tA\x1b[5mB", 80, false));
        REQUIRE_FALSE(d.ice);
        CHECK(d.canvas.at(0, 0).bg.index == Green);
        CHECK(d.canvas.at(0, 0).attrs == 0);
        CHECK(d.canvas.at(1, 0).bg.index == Blue);
        CHECK(d.canvas.at(1, 0).attrs == attr::Blink);
    }
    SECTION("an override SGR 1 or 5 drops while reversed does not come back after 27") {
        auto d = parse("\x1b[31m\x1b[1;0;170;0t\x1b[7mA\x1b[1m\x1b[27mB");
        CHECK(d.canvas.at(0, 0).fg.index == Black);
        CHECK(d.canvas.at(0, 0).bg.index == Red);
        CHECK(d.canvas.at(1, 0).fg.index == LightRed);
        auto e = parse("\x1b[44m\x1b[0;0;170;0t\x1b[7mA\x1b[5m\x1b[27mB");
        CHECK(e.canvas.at(0, 0).fg.index == Blue);
        CHECK(e.canvas.at(0, 0).bg.index == Grey);
        CHECK(e.canvas.at(1, 0).bg.index == LightBlue);
    }
    SECTION("a channel past 255 reads as 0, as libansilove") {
        // strtonum(..., 0, UCHAR_MAX) fails and gives 0 (loaders/ansi.c:493-497)
        auto d = parse("\x1b[44m\x1b[0;999;0;170tA");
        CHECK(d.canvas.at(0, 0).bg.index == Blue);
        auto e = parse("\x1b[41m\x1b[0;999;999;999tA");  // all 0: no 24-bit colour
        CHECK(e.canvas.at(0, 0).bg.index == Red);
    }
    SECTION("a selector other than 0 or 1 does nothing, as libansilove") {
        // loaders/ansi.c:499-506 acts on 0 and 1 only
        auto d = parse("\x1b[2;255;0;0tA\x1b[5;0;0;170tB");
        CHECK(d.canvas.at(0, 0).bg.index == Black);
        CHECK(d.canvas.at(0, 0).fg.index == Grey);
        CHECK(d.canvas.at(1, 0).bg.index == Black);
    }
    SECTION("missing values read as 0, as libansilove") {
        // loaders/ansi.c:492-497
        auto d = parse("\x1b[1;255;0tA\x1b[0;;170tB");
        CHECK(d.canvas.at(0, 0).fg.index == Red);  // (255,0,0)
        CHECK(d.canvas.at(1, 0).bg.index == Red);  // the empty field is skipped: (170,0,0)
    }
    SECTION("black means no 24-bit colour and drops one set before, as libansilove") {
        // a 24-bit value of 0 counts as unset (loaders/ansi.c:205-209)
        auto d = parse("\x1b[44m\x1b[0;0;0;0tA");
        CHECK(d.canvas.at(0, 0).bg.index == Blue);
        auto e = parse("\x1b[31m\x1b[1;0;170;0tA\x1b[1;0;0;0tB\x1b[1tC");
        CHECK(e.canvas.at(0, 0).fg.index == Green);
        CHECK(e.canvas.at(1, 0).fg.index == Red);
        CHECK(e.canvas.at(2, 0).fg.index == Red);
        // a black CSI t leaves an aixterm or 38;5 colour alone
        auto f = parse("\x1b[104m\x1b[0;0;0;0tA");
        CHECK(f.canvas.at(0, 0).bg.index == LightBlue);
    }
    SECTION("SGR 38;2 and 38;5 quantise the same way") {
        auto d = parse("\x1b[38;2;250;80;90;48;5;12mA");
        CHECK(d.canvas.at(0, 0).fg.index == LightRed);
        CHECK(d.canvas.at(0, 0).bg.index == LightBlue);  // xterm 12 = bright blue
        auto e = parse("\x1b[38;5;196;48;5;21mA");          // (255,0,0) and (0,0,255)
        CHECK(e.canvas.at(0, 0).fg.index == Red);
        CHECK(e.canvas.at(0, 0).bg.index == Blue);
    }
}

TEST_CASE("Erase in line (CSI K) is skipped, as libansilove", "[parse]") {
    auto d = parse("ABCDEF\x1b[3D\x1b[K");
    CHECK(d.canvas.at(2, 0).ch == U'C');
    CHECK(d.canvas.at(3, 0).ch == U'D');
    for (const char* mode : {"", "0", "1", "2"}) {
        INFO("mode '" << mode << "'");
        auto e = parse(std::string("\x1b[41mABC\r\x1b[") + mode + "K");
        CHECK(e.canvas.at(0, 0).ch == U'A');
        CHECK(e.canvas.at(0, 0).bg.index == Red);
        CHECK(e.canvas.at(79, 0) == Cell{});
    }
}

TEST_CASE("2J clears and homes, as ANSI.SYS", "[parse]") {
    auto e = parse("junk\r\nmore\x1b[2JX");
    CHECK(e.canvas.height() == 1);
    CHECK(e.canvas.at(0, 0).ch == U'X');
    CHECK(e.canvas.at(1, 0).ch == U' ');
}

TEST_CASE("Moving far down and clearing the screen costs no work per row", "[parse][slow-input]") {
    // 2000 x (CUD to row 4999, a glyph, 2J) at width 400, 24 KB: rows exist
    // only once drawn on, so this is as quick as any 24 KB file (it took
    // 14 s when every row down to the cursor was allocated and zeroed)
    std::string body;
    for (int i = 0; i < 2000; ++i)
        body += "\x1b[4999B@\x1b[2J";
    body += "X";
    const auto t0 = std::chrono::steady_clock::now();
    auto d = parseAnsi(withSauce(body, 400, true));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(d.canvas.width() == 400);
    CHECK(d.canvas.height() == 1);
    CHECK(d.canvas.at(0, 0).ch == U'X');
    CHECK(ms < 1000);
    // what was drawn before the last 2J is gone; TInfo2 still sets the minimum height
    Sauce s = Sauce::fresh(400, 3);
    const std::string tall = "\x1b[4999B@\x1b[2JX";
    auto e = parseAnsi(tall + encodeSauce(s, static_cast<std::uint32_t>(tall.size())));
    CHECK(e.canvas.height() == 3);
    CHECK(e.canvas.at(0, 0).ch == U'X');
    CHECK(e.canvas.at(0, 2).ch == U' ');
}

TEST_CASE("Height is the last row drawn on", "[parse]") {
    CHECK(parse("A\r\nB\r\n").canvas.height() == 2);
    CHECK(parse("A\r\n\r\n\r\n").canvas.height() == 1);
    CHECK(parse("A\r\n \r\n").canvas.height() == 2);
}

TEST_CASE("Unknown and private sequences are skipped", "[parse]") {
    auto d = parse("\x1b[?7h\x1b[=1hA\x1b[1 qB");
    CHECK(d.canvas.at(0, 0).ch == U'A');
    CHECK(d.canvas.at(1, 0).ch == U'B');
    CHECK(d.canvas.at(2, 0).ch == U' ');
}

TEST_CASE("An ESC that starts no CSI skips only the ESC, as libansilove", "[parse]") {
    SECTION("a lone ESC before a CSI") {
        auto d = parse("\x1b[31mA\x1b\x1b[32mB");
        CHECK(d.canvas.at(0, 0).ch == U'A');
        CHECK(d.canvas.at(0, 0).fg.index == Red);
        CHECK(d.canvas.at(1, 0).ch == U'B');
        CHECK(d.canvas.at(1, 0).fg.index == Green);
        CHECK(d.canvas.at(2, 0).ch == U' ');
    }
    SECTION("a charset designation draws its bytes") {
        auto d = parse("\x1b(BHELLO");
        CHECK(d.canvas.at(0, 0).ch == U'(');
        CHECK(d.canvas.at(1, 0).ch == U'B');
        CHECK(d.canvas.at(6, 0).ch == U'O');
    }
    SECTION("an ESC before a control byte lets it act") {
        auto d = parse("A\x1b\nB");
        REQUIRE(d.canvas.height() == 2);
        CHECK(d.canvas.at(0, 1).ch == U'B');
    }
}

TEST_CASE("A malformed CSI doesn't swallow what follows it", "[parse]") {
    SECTION("an ESC inside a CSI ends it and starts a new escape") {
        auto d = parse("A\x1b[31\x1b[32mB");
        CHECK(d.canvas.at(1, 0).ch == U'B');
        CHECK(d.canvas.at(1, 0).fg.index == Green);
        CHECK(d.canvas.at(2, 0).ch == U' ');
    }
    SECTION("a control byte ends it and still acts") {
        auto d = parse("A\x1b[31\nB");
        REQUIRE(d.canvas.height() == 2);
        CHECK(d.canvas.at(0, 1).ch == U'B');
        CHECK(d.canvas.at(0, 1).fg.index == Grey);
    }
}

TEST_CASE("Bad input is refused with a message", "[parse]") {
    CHECK_THROWS_AS(parse(""), AnsiError);
    CHECK_THROWS_AS(parse("\x1b[0m\r\n"), AnsiError);
    CHECK_THROWS_AS(parse("A", 0), AnsiError);
    CHECK_THROWS_AS(parseAnsi(withSauce("A", 1000, true)), AnsiError);
    CHECK_THROWS_AS(parse("\x1b[6000BA"), AnsiError);
    // huge parameters saturate instead of overflowing
    auto d = parse("\x1b[99999999999999999999CA");
    CHECK(d.canvas.at(0, 1).ch == U'A');
}
