// Reading a Micron page (micron::parse): each of MicronParser's codes from
// the hand-written pages in tests/data/micron/, the page headers, the width
// and stripping rules, the loss report, and malformed and truncated input.
// The writer's pages reading back are in test_micron.cpp.

#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/micron.h"
#include "blockwright/palette.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

using namespace bw;
using namespace bwtest;

namespace {

AnsiDocument page(const char* name) { return micron::parse(readFile(dataDir() / "micron" / name)); }

// The row's glyphs, a wide pair as its first.
std::u32string rowText(const Canvas& c, int y) {
    std::u32string s;
    for (int x = 0; x < c.width(); ++x)
        if (!c.at(x, y).ch.isContinuation())
            for (const char32_t cp : codePoints(c.at(x, y).ch))
                s += cp;
    return s;
}

// The row without the blanks at its end.
std::u32string trimmed(const Canvas& c, int y) {
    std::u32string s = rowText(c, y);
    while (!s.empty() && s.back() == U' ')
        s.pop_back();
    return s;
}

Rgb shown(const Color& c) { return c.isIndexed() ? kPalette[c.index] : c.rgb; }

constexpr Rgb rgb(std::uint32_t v) { return Rgb{std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)}; }

void withinLimits(const AnsiDocument& d) {
    CHECK(d.canvas.width() >= 1);
    CHECK(d.canvas.width() <= kMaxColumns);
    CHECK(d.canvas.height() >= 1);
    CHECK(d.canvas.height() <= kMaxRows);
}

} // namespace

TEST_CASE("A Micron page is a file ending in .mu") {
    CHECK(micron::isPage("index.mu"));
    CHECK(micron::isPage("dir/PAGE.MU"));
    CHECK(micron::isPage(".mu"));
    CHECK_FALSE(micron::isPage("mu"));
    CHECK_FALSE(micron::isPage("index.mux"));
    CHECK_FALSE(micron::isPage("index.ans"));
    CHECK_FALSE(micron::isPage("menu"));
}

TEST_CASE("Micron colours: three digits, truecolour, greys, resets and bad codes") {
    const AnsiDocument d = page("colours.mu");
    const Canvas& c = d.canvas;
    CHECK(d.encoding == TextEncoding::Utf8);
    REQUIRE(c.height() == 6);  // "`*" draws nothing
    CHECK(c.width() == 17);
    CHECK(trimmed(c, 0) == U"R-B-T-U-G-W");
    CHECK(c.at(0, 0).fg == Color::fromRgb(rgb(0xff0000)));  // `Ff00, not in the palette
    CHECK(c.at(1, 0).fg == Color::fromRgb(rgb(0xdddddd)));  // `f: NomadNet's default text
    CHECK(c.at(1, 0).bg == Color::indexed(0));               // the terminal's own background
    CHECK(c.at(2, 0).bg == Color::fromRgb(rgb(0x0000ff)));
    CHECK(c.at(3, 0).bg == Color::indexed(0));  // `b
    CHECK(c.at(4, 0).fg == Color::fromRgb(rgb(0x102030)));  // `FT
    CHECK(c.at(6, 0).bg == Color::fromRgb(rgb(0xa0b0c0)));  // `BT
    CHECK(c.at(8, 0).fg == Color::fromRgb(rgb(0x808080)));  // g50, urwid's grey 50
    CHECK(c.at(10, 0).bg == Color::indexed(15));            // g99 is white, a palette colour
    // a colour persists to the next line until reset
    CHECK(trimmed(c, 1) == U"green");
    CHECK(c.at(0, 1).fg == Color::indexed(Green));
    CHECK(trimmed(c, 2) == U"still green");
    CHECK(c.at(0, 2).fg == Color::indexed(Green));
    // a malformed colour is the terminal's own
    CHECK(trimmed(c, 3) == U"badshort");
    CHECK(c.at(0, 3).fg == Color::indexed(Grey));
    CHECK(c.at(3, 3).bg == Color::indexed(Black));
    CHECK(trimmed(c, 4) == U"12");  // too short to be a colour: drawn as text
    CHECK(c.at(0, 4).fg == Color::fromRgb(rgb(0xdddddd)));
    CHECK(trimmed(c, 5) == U"grey on blueplain");
    CHECK(c.at(0, 5).fg == Color::indexed(DarkGrey));
    CHECK(c.at(0, 5).bg == Color::indexed(Blue));
    CHECK(c.at(0, 5).attrs == attr::Italic);  // from the line that drew nothing
    // `` resets both colours and the formatting
    CHECK(c.at(12, 5).fg == Color::fromRgb(rgb(0xdddddd)));
    CHECK(c.at(12, 5).bg == Color::indexed(Black));
    CHECK(c.at(12, 5).attrs == 0);
    // a row's blanks past its text take the colours its line ended in
    CHECK(c.at(16, 1).fg == Color::indexed(Green));
    CHECK_FALSE(d.readLoss.lossy());
}

TEST_CASE("Micron formatting: bold, underline and italic toggle and persist") {
    const Canvas c = page("formatting.mu").canvas;
    REQUIRE(c.height() == 3);
    CHECK(trimmed(c, 0) == U"bold under italic");
    CHECK(c.at(0, 0).attrs == attr::Bold);
    CHECK(c.at(4, 0).attrs == 0);
    CHECK(c.at(5, 0).attrs == attr::Underline);
    CHECK(c.at(11, 0).attrs == attr::Italic);
    const std::uint8_t all = attr::Bold | attr::Underline | attr::Italic;
    CHECK(c.at(0, 1).attrs == all);
    CHECK(c.at(0, 2).attrs == all);
    CHECK(trimmed(c, 2) == U"still all threereset");
    CHECK(c.at(15, 2).attrs == 0);
}

TEST_CASE("Micron literal mode and escapes") {
    const AnsiDocument d = page("literal.mu");
    const Canvas& c = d.canvas;
    REQUIRE(c.height() == 4);
    CHECK(trimmed(c, 0) == U"`!kept` [a`b] #not a comment");
    CHECK(c.at(2, 0).attrs == 0);
    CHECK(trimmed(c, 1) == U"`=");  // "\`=" inside literal mode
    CHECK(trimmed(c, 2) == U"`!no code\\ here");
    CHECK(c.at(1, 2).attrs == 0);
    CHECK(trimmed(c, 3) == U"a`b");
    CHECK_FALSE(d.readLoss.lossy());
}

TEST_CASE("Micron alignment: centre, right and back to left at the page width") {
    const AnsiDocument d = page("align.mu");
    const Canvas& c = d.canvas;
    REQUIRE(c.height() == 4);
    CHECK(c.width() == 14);  // "again left too"
    CHECK(rowText(c, 0) == U"      mid     ");  // urwid puts the odd column on the left
    CHECK(rowText(c, 1) == U"         right");
    CHECK(trimmed(c, 2) == U"left");
    CHECK(trimmed(c, 3) == U"again left too");  // the alignment a line ends in
    CHECK(d.readLoss.micron.aligned.count == 2);
    CHECK(d.readLoss.micron.aligned.line == 1);
}

TEST_CASE("Micron sections: headings, depth, dividers, comments and fold glyphs") {
    const AnsiDocument d = page("sections.mu");
    const Canvas& c = d.canvas;
    REQUIRE(c.height() == 10);
    CHECK(c.width() == 12);  // "text two" with its indent on both sides
    CHECK(trimmed(c, 0) == U"Top");
    CHECK(shown(c.at(0, 0).fg) == rgb(0x222222));
    CHECK(shown(c.at(0, 0).bg) == rgb(0xbbbbbb));
    CHECK(shown(c.at(11, 0).bg) == rgb(0xbbbbbb));  // a heading fills its row
    CHECK(trimmed(c, 1) == U"text one");
    CHECK(shown(c.at(0, 1).bg) == rgb(0));  // the heading's colours stay on its line
    CHECK(trimmed(c, 2) == U"  Sub");
    CHECK(shown(c.at(0, 2).bg) == rgb(0x999999));
    CHECK(rowText(c, 3) == U"  text two  ");
    CHECK(rowText(c, 4) == U"  ────────  ");
    CHECK(rowText(c, 5) == U"  ========  ");
    CHECK(trimmed(c, 6) == U"back out");  // "<" went back to depth 0 and drew nothing
    CHECK(trimmed(c, 7) == U"    Deep");
    CHECK(shown(c.at(4, 7).fg) == rgb(0));
    CHECK(shown(c.at(4, 7).bg) == rgb(0x777777));
    CHECK(trimmed(c, 8) == U"+ Open");  // "#!fold + -"
    CHECK(trimmed(c, 9) == U"- Closed");
    const MicronReadLoss& m = d.readLoss.micron;
    CHECK(m.headings.count == 5);
    CHECK(m.headings.line == 1);
    CHECK(m.dividers.count == 2);
    CHECK(m.dividers.line == 5);
    CHECK(m.sections.count == 3);
    CHECK(m.sections.line == 4);
}

TEST_CASE("Micron links, anchors and fields are drawn as their text") {
    const AnsiDocument d = page("links-fields.mu");
    const Canvas& c = d.canvas;
    REQUIRE(c.height() == 7);
    CHECK(c.width() == 31);
    CHECK(trimmed(c, 0) == U"go Home or :/page/about.mu here");
    CHECK(rowText(c, 1).substr(0, 24) == U"anon" + std::u32string(20, U' '));
    CHECK(trimmed(c, 2) == U"toolongn");  // cut at the field's width
    CHECK(trimmed(c, 3) == U"******");    // masked
    CHECK(trimmed(c, 4) == U"[X] Agree");
    CHECK(trimmed(c, 5) == U"( ) Alpha");
    CHECK(trimmed(c, 6) == U"in a heading line");  // a field line is never a heading
    const MicronReadLoss& m = d.readLoss.micron;
    CHECK(m.links.count == 2);
    CHECK(m.links.line == 1);
    CHECK(m.fields.count == 6);
    CHECK(m.fields.line == 2);
    CHECK(m.headings.count == 0);
    const std::vector<std::string> notes = micron::readNotes(d.readLoss);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0] == "2 links drawn as their labels, the first on line 1");
    CHECK(notes[1] == "6 fields drawn as their text, the first on line 2");
}

TEST_CASE("Micron tables keep their lines; partials and images are left out") {
    const AnsiDocument d = page("dropped.mu");
    const Canvas& c = d.canvas;
    REQUIRE(c.height() == 4);
    CHECK(trimmed(c, 0) == U"before");
    CHECK(trimmed(c, 1) == U"| a | b |");
    CHECK(trimmed(c, 2) == U"| c | d |");
    CHECK(trimmed(c, 3) == U"after");
    const std::vector<std::string> notes = micron::readNotes(d.readLoss);
    REQUIRE(notes.size() == 3);
    CHECK(notes[0] == "1 table kept as its lines of text, on line 2");
    CHECK(notes[1] == "1 partial left out, on line 6");
    CHECK(notes[2] == "1 image left out, on line 7");
    CHECK(d.readLoss.lossy());
}

TEST_CASE("A Micron table of one line draws nothing, as in NomadNet") {
    const AnsiDocument d = micron::parse("`t\n|a|\n`t\nafter");
    REQUIRE(d.canvas.height() == 1);
    CHECK(trimmed(d.canvas, 0) == U"after");
    CHECK_FALSE(d.readLoss.lossy());
    // nor does an empty one, or one left open at the end of the page
    CHECK(trimmed(micron::parse("`t\n`t\nafter").canvas, 0) == U"after");
    CHECK(micron::parse("before\n`t\n|a|").canvas.height() == 1);
    // the lines between are what the table buffered: not empty lines, comments or literal ones
    const AnsiDocument e = micron::parse("`t\n|a|\n\n# note\n`=\nlit\n`=\n|b|\n`t");
    const Canvas& c = e.canvas;
    REQUIRE(c.height() == 4);
    CHECK(trimmed(c, 0) == U"");     // the empty line, drawn where it is
    CHECK(trimmed(c, 1) == U"lit");  // literal lines are drawn as they come
    CHECK(trimmed(c, 2) == U"|a|");
    CHECK(trimmed(c, 3) == U"|b|");
    CHECK(e.readLoss.micron.tables.count == 1);
}

TEST_CASE("A Micron table never closed is not drawn, as in NomadNet") {
    const AnsiDocument d = micron::parse("x\n`t\n|a|b|\n|c|d|");
    REQUIRE(d.canvas.height() == 1);
    CHECK(trimmed(d.canvas, 0) == U"x");
    CHECK(micron::readNotes(d.readLoss) == std::vector<std::string>{"1 table never closed, left out, on line 2"});
    CHECK(d.readLoss.lossy());
    // one closed by a second `t is still kept as its lines
    CHECK(micron::parse("x\n`t\n|a|\n|b|\n`t").canvas.height() == 3);
}

TEST_CASE("Micron partials and images are counted when NomadNet draws one") {
    // NomadNet draws one partial (line 3) and one image (line 4); the other lines draw nothing
    const AnsiDocument d = micron::parse("`{nope\n`{url`abc}\n`{url}\n`(alt`url)\n`(nope");
    CHECK(d.canvas.height() == 1);
    CHECK(micron::readNotes(d.readLoss) ==
          std::vector<std::string>{"1 partial left out, on line 3", "1 image left out, on line 4"});
    // a refresh time is what Python's float() reads; no URL or four parts is none
    const AnsiDocument p =
        micron::parse("`{u`1e3}\n`{u` 2.5 }\n`{u`1_0}\n`{u`-INF}\n`{u`.5e-1_0}\n`{u`1__0}\n`{u``}\n`{u`1.}\n"
                      "`{`1}\n`{a`1`b`c}\n`{u`1`f}\n`{u`e5}\n`{u`1e}");
    CHECK(p.readLoss.micron.partials.count == 7);
    // an image needs a ")" after its first character and a "`" before the last ")"
    const AnsiDocument i = micron::parse("`()\n`(a)\n`(`)\n`(a`b`c)x\n`(a)`)");
    CHECK(i.readLoss.micron.images.count == 3);
    CHECK(i.readLoss.micron.images.line == 3);
}

TEST_CASE("Micron tabs go to urwid's 8-column stops") {
    // urwid 5.1.3, the one NomadNet 1.5.0 installs: "a" at 0, "b" at 8, "c" at 24
    const AnsiDocument d = micron::parse("a\tb\t\tc");
    CHECK(d.canvas.width() == 25);
    CHECK(rowText(d.canvas, 0) == U"a       b               c");
    // counted from the start of the row's text: after a section's indent,
    // across a link and a change of style
    const Canvas s = micron::parse(">>H\nx\ty\n`[ab\tc`u]\td\n`!a`!\tb").canvas;
    CHECK(rowText(s, 1).substr(0, 13) == U"  x       y  ");
    CHECK(trimmed(s, 2) == U"  ab      c       d");
    CHECK(trimmed(s, 3) == U"  a       b");
    // a heading's indent is part of its text
    CHECK(trimmed(micron::parse(">>x\ty").canvas, 0) == U"  x     y");
    // in a line with a field each part is a widget of its own
    CHECK(trimmed(micron::parse("a\t`<|n`d\te>").canvas, 0) == U"a       d       e");
    CHECK(trimmed(micron::parse("`<?|n`v`>a\tb").canvas, 0) == U"[ ] v`a       b");
    CHECK(trimmed(micron::parse("`<?|n`a\tb>").canvas, 0) == U"[ ] a       b");
}

TEST_CASE("A Micron page far past the limits holds no more than one at them") {
    SECTION("200,000 lines keep kMaxRows rows and count the rest") {
        std::string p;
        for (int i = 0; i < 200000; ++i)
            p += i % 1000 == 0 ? "`[x`y]\n" : "r\n";
        micron::ParseStats stats;
        const AnsiDocument d = micron::parse(p, &stats);
        CHECK(stats.rowsHeld <= std::size_t(kMaxRows));
        CHECK(d.canvas.height() == kMaxRows);
        CHECK(d.canvas.width() == 1);
        CHECK(trimmed(d.canvas, kMaxRows - 1) == U"r");
        CHECK(micron::readNotes(d.readLoss) ==
              std::vector<std::string>{"200 links drawn as their labels, the first on line 1",
                                       "195000 lines past row 5000 left out, the first on line 5001"});
    }
    SECTION("so do 200,000 lines of a table") {
        std::string p = "`t\n";
        for (int i = 0; i < 200000; ++i)
            p += "|r|\n";
        p += "`t\n";
        micron::ParseStats stats;
        const AnsiDocument d = micron::parse(p, &stats);
        CHECK(stats.rowsHeld <= std::size_t(kMaxRows));
        CHECK(d.canvas.height() == kMaxRows);
        CHECK(d.readLoss.micron.rows.count == 200000 - kMaxRows);
        CHECK(d.readLoss.micron.rows.line == kMaxRows + 2);
    }
    SECTION("a line of a million cells builds a row no wider than the limit") {
        for (const std::string& line :
             {std::string(1 << 20, 'a'), "x" + std::string(1 << 20, '\t') + "x", "`<?|n`v>" + std::string(1 << 20, '\t') + "x",
              ">" + std::string(1 << 20, 'h'), "`=\n" + std::string(1 << 20, 'l'), "#!fold " + std::string(1 << 20, 'g') +
              "\n`+>x"}) {
            micron::ParseStats stats;
            const AnsiDocument d = micron::parse(line, &stats);
            CHECK(stats.widestRow <= std::size_t(2 * kMaxColumns));
            CHECK(d.canvas.width() == kMaxColumns);
            CHECK(d.readLoss.micron.clipped.count == 1);
        }
    }
}

TEST_CASE("Micron page headers set the default colours") {
    const AnsiDocument d = micron::parse("#!fg=0f0\n#!bg=g10\nhello\nx`F00fy`f`bz\n");
    const Canvas& c = d.canvas;
    REQUIRE(c.height() == 2);  // the header lines are comments
    CHECK(c.at(0, 0).fg == Color::fromRgb(rgb(0x00ff00)));
    CHECK(c.at(0, 0).bg == Color::fromRgb(rgb(0x1c1c1c)));
    CHECK(trimmed(c, 1) == U"xyz");
    CHECK(c.at(2, 1).fg == Color::fromRgb(rgb(0x00ff00)));  // `f goes back to the header's
    // a value that isn't three or six long is ignored
    const AnsiDocument e = micron::parse("#!fg=abcd\nok");
    CHECK(e.canvas.at(0, 0).fg == Color::fromRgb(rgb(0xdddddd)));
}

TEST_CASE("Micron page width, stripping and limits") {
    SECTION("an empty page is one blank row of the default width") {
        for (const char* p : {"", "\n\n", "# only a comment", "`F0f0`!"}) {
            const AnsiDocument d = micron::parse(p);
            CHECK(d.canvas.width() == Canvas::kDefaultWidth);
            CHECK(d.canvas.height() == 1);
        }
    }
    SECTION("the page is stripped at both ends, CR and controls go") {
        const AnsiDocument d = micron::parse("\n\n   hi\r\nthere\x01\x7f\r\n\n  ");
        REQUIRE(d.canvas.height() == 2);
        CHECK(rowText(d.canvas, 0) == U"hi   ");
        CHECK(rowText(d.canvas, 1) == U"there");
    }
    SECTION("lines are counted from the file's first") {
        const AnsiDocument d = micron::parse("\n\n\n`[x`y]");
        CHECK(d.readLoss.micron.links.line == 4);
    }
    SECTION("marks are taken out, a tab goes to the next stop") {
        const AnsiDocument d = micron::parse("e\xcc\x81\tz");
        CHECK(rowText(d.canvas, 0) == U"e       z");
    }
    SECTION("a wide glyph takes two cells") {
        const AnsiDocument d = micron::parse("中x\nab");
        CHECK(d.canvas.width() == 3);
        CHECK(d.canvas.at(0, 0).ch == Glyph(U'中'));
        CHECK(d.canvas.at(1, 0).ch.isContinuation());
        CHECK(d.canvas.at(2, 0).ch == Glyph(U'x'));
    }
    SECTION("a line past the column limit is cut and reported") {
        const AnsiDocument d = micron::parse(std::string(500, 'a') + "\nb");
        CHECK(d.canvas.width() == kMaxColumns);
        CHECK(d.readLoss.micron.clipped.count == 1);
        CHECK(micron::readNotes(d.readLoss) ==
              std::vector<std::string>{"1 row cut at " + std::to_string(kMaxColumns) + " columns, on line 1"});
    }
    SECTION("lines past the row limit are left out and reported") {
        std::string p;
        for (int i = 0; i < kMaxRows + 2; ++i)
            p += "r\n";
        const AnsiDocument d = micron::parse(p);
        CHECK(d.canvas.height() == kMaxRows);
        CHECK(d.readLoss.micron.rows.count == 2);
        CHECK(d.readLoss.micron.rows.line == kMaxRows + 1);
    }
    SECTION("bytes that aren't UTF-8 are U+FFFD and reported") {
        const AnsiDocument d = micron::parse("ok\n\xff\xfe!");
        CHECK(trimmed(d.canvas, 1) == U"��!");
        CHECK(d.readLoss.micron.badUtf8.count == 2);
        CHECK(d.readLoss.micron.badUtf8.line == 2);
    }
}

TEST_CASE("Malformed Micron codes don't crash") {
    for (const char* p : {"`", "``", "`F", "`FT", "`FT12", "`B1", "`BT1234", "[", "[`", "[a`", "[]", "[``]",
                          "`[", "`[a`b`c`d]", "`<", "`<a", "`<a`", "`<a`b", "`<|", "`<|`>", "`<x|n`d>", "`<?|`>",
                          "`<^||||||`>", "`<-5|n`d>", "`<99999999999999999999|n`d>", ">", ">>>>>>>>>>", "-", "-\x01",
                          "`t", "`{", "`{}", "`(", "\\", "\\\\", "`=", "`+", "`+>", "`->", "<", "<<<", "<>", "#!fold",
                          "`:", "`:a", "`c", "\xf0\x9f\x95\xb5\xef\xb8\x8f", "\xe2\x98\x9d"}) {
        INFO(p);
        const AnsiDocument d = micron::parse(p);
        withinLimits(d);
    }
}

TEST_CASE("Truncated and garbage Micron pages don't crash") {
    // every prefix of the writer's pages
    for (const char* name : {"swatch.mu", "swatch-centre.mu", "durin-gate.mu"}) {
        const std::string mu = readFile(dataDir() / "golden-micron" / name);
        REQUIRE_FALSE(mu.empty());
        for (std::size_t n = 0; n <= mu.size(); n += n < 4000 ? 1 : 97)
            withinLimits(micron::parse(std::string_view(mu).substr(0, n)));
    }
    // markup-heavy garbage from a fixed seed
    static constexpr char kAlphabet[] = "`\\[]<>|!*_FBTfbcalrg:=#-{}()?^+0123456789abcdefxyz \n\r\t\xe4\xb8\xad\xff\x80";
    std::uint32_t seed = 0x6d75u;
    const auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int i = 0; i < 3000; ++i) {
        std::string s(next() % 160, ' ');
        for (char& ch : s)
            ch = next() % 8 == 0 ? char(next() & 0xff) : kAlphabet[next() % (sizeof kAlphabet - 1)];
        INFO(s);
        withinLimits(micron::parse(s));
    }
    // long runs that would make a naive search quadratic
    withinLimits(micron::parse(std::string(40000, '`') + "[" + std::string(40000, '[')));
    std::string opens;
    for (int i = 0; i < 20000; ++i)
        opens += "`<a`";
    withinLimits(micron::parse(opens));
}
