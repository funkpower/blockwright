// The Micron writer (micron.h): colours, escaping, glyphs, centring, the
// header, and the golden pages under tests/data/golden-micron/, which the
// micron_nomadnet test reads back with NomadNet's own parser.

#include "util.h"

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"
#include "blockwright/micron.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>

using namespace bw;
using namespace bwtest;
namespace fs = std::filesystem;

namespace {

Cell cell(char32_t ch, std::uint8_t fg, std::uint8_t bg, std::uint8_t attrs = 0) {
    Cell c;
    c.ch = ch;
    c.fg = Color::indexed(fg);
    c.bg = Color::indexed(bg);
    c.attrs = attrs;
    return c;
}

Canvas row(const std::vector<Cell>& cells) {
    Canvas c(int(cells.size()), 1);
    for (std::size_t x = 0; x < cells.size(); ++x)
        c.at(int(x), 0) = cells[x];
    return c;
}

Canvas text(std::u32string_view s, std::uint8_t fg = Grey, std::uint8_t bg = Black) {
    std::vector<Cell> cells;
    for (const char32_t ch : s)
        cells.push_back(cell(ch, fg, bg));
    return row(cells);
}

std::string line(const Canvas& c, bool ice = true, const micron::Options& o = {}) {
    return micron::encodeLine(c, 0, kPalette, ice, o);
}

std::string utf8(std::u32string_view s) {
    std::string out;
    for (const char32_t cp : s)
        appendUtf8(out, cp);
    return out;
}

std::string hex(Rgb c) {
    static constexpr char d[] = "0123456789abcdef";
    std::string s = "#";
    for (const std::uint8_t v : {c.r, c.g, c.b}) {
        s += d[v >> 4];
        s += d[v & 15];
    }
    return s;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        if (ch == '"' || ch == '\\')
            out += '\\';
        out += ch;
    }
    return out + "\"";
}

// What NomadNet should draw for `canvas`, worked out from the cells, not the
// writer: the micron_parser_check.py format. Glyphs as CP437 shows them (NUL
// a space, a C0 control or DEL its glyph), colours from `palette` as the
// editor shows them with iCE `ice`, a colour the cell doesn't show null (a
// blank's foreground unless underlined, the background of a full block
// unless italic), and the formatting toggles on as "biu" letters.
std::string expectedCells(const Canvas& canvas, const std::array<Rgb, 16>& palette, bool ice, bool centre) {
    std::string s = std::string("{\"align\": \"") + (centre ? "center" : "left") + "\", \"rows\": [\n";
    for (int y = 0; y < canvas.height(); ++y) {
        s += y ? ",\n [" : " [";
        for (int x = 0; x < canvas.width(); ++x) {
            Cell c = canvas.at(x, y);
            if (c.transparent)
                c = Cell{};
            if (x)
                s += ", ";
            if (c.ch.isContinuation()) {
                s += "[\"\", null, null, \"\"]";
                continue;
            }
            std::string t;
            for (char32_t cp : codePoints(c.ch)) {
                if (cp == 0)
                    cp = U' ';
                else if (cp < 0x20 || cp == 0x7f)
                    cp = cp437ToUnicode(std::uint8_t(cp));
                appendUtf8(t, cp);
            }
            const Cell d = c.bg.isIndexed() ? iceCell(c, ice) : c;
            const auto rgb = [&](const Color& k) { return k.isIndexed() ? palette[k.index] : k.rgb; };
            const bool blank = (t == " " || t == " ") && !(d.attrs & attr::Underline);
            const bool solid = t == "█" && !(d.attrs & attr::Italic);
            std::string fmt;
            if (d.attrs & attr::Bold)
                fmt += 'b';
            if (d.attrs & attr::Italic)
                fmt += 'i';
            if (d.attrs & attr::Underline)
                fmt += 'u';
            s += "[" + jsonString(t) + ", " + (blank ? "null" : jsonString(hex(rgb(d.fg)))) + ", " +
                 (solid ? "null" : jsonString(hex(rgb(d.bg)))) + ", " + jsonString(fmt) + "]";
        }
        s += "]";
    }
    return s + "\n]}\n";
}

// Checks `got` against tests/data/golden-micron/<name>, or writes it there
// when BW_MICRON_REGEN is set.
void golden(const std::string& name, const std::string& got) {
    const fs::path p = dataDir() / "golden-micron" / name;
    if (std::getenv("BW_MICRON_REGEN")) {
        std::ofstream(p, std::ios::binary) << got;
        return;
    }
    INFO(p.string());
    CHECK(readFile(p) == got);
}

// A piece with every colour as foreground and background, iCE bright
// backgrounds, every escape case, a wide glyph and the CP437 controls.
AnsiDocument swatch() {
    AnsiDocument d;
    d.ice = true;
    d.canvas = Canvas(32, 12);
    Canvas& c = d.canvas;
    const auto put = [&](int x, int y, std::u32string_view s, std::uint8_t fg, std::uint8_t bg) {
        for (const char32_t ch : s)
            c.at(x++, y) = cell(ch, fg, bg);
    };
    for (int i = 0; i < 16; ++i) {
        put(i * 2, 0, U"A▄", std::uint8_t(i), Black);              // every foreground
        put(i * 2, 1, U" ░", std::uint8_t(15 - i), std::uint8_t(i));  // every background
        Cell blink = cell(U'▒', std::uint8_t(i), std::uint8_t(i & 7), attr::Blink);
        c.at(i, 2) = blink;  // iCE: blink is the bright background
    }
    // a line may not start with anything Micron reads as markup
    const std::u32string_view leads[] = {U">heading", U"-", U"-=", U"#comment", U"<reset", U"`=", U"\\esc",
                                         U"`t table", U"[link`x]"};
    for (int i = 0; i < 9; ++i)
        put(i < 5 ? 0 : 20, 3 + (i % 5), leads[i], std::uint8_t(9 + i % 6), Black);
    put(10, 3, U"a`b\\c``", Yellow, Blue);
    put(10, 4, U"#!bg=abc", White, Black);
    put(10, 5, U"\\`!_*", LightCyan, Red);
    // the wide glyph, the CP437 controls, NUL, DEL and a no-break space
    c.at(0, 8) = cell(U'中', Yellow, Red);
    c.at(1, 8) = cell(U' ', Yellow, Red);
    c.at(1, 8).ch = Glyph::continuation();
    put(3, 8, U"\u0001\u0002\u0003\u001b\u007f", LightGreen, Blue);
    c.at(9, 8) = cell(U'\0', White, Magenta);
    c.at(10, 8) = cell(U' ', White, Magenta);
    put(12, 8, U"██▀▄▌▐░▒▓", LightRed, Cyan);
    // trailing blanks in colour, and a blank row
    put(0, 9, U"end", White, Black);
    put(3, 9, U"     ", White, Blue);
    put(0, 11, U"underlined blank:", Grey, Black);
    c.at(18, 11) = cell(U' ', LightMagenta, Black, attr::Underline);
    c.at(19, 11) = cell(U'b', White, Black, attr::Bold);
    // full blocks in each formatting: only italic shows the background
    c.at(21, 11) = cell(U'█', Yellow, Blue, attr::Italic);
    c.at(22, 11) = cell(U'i', White, Red, attr::Italic);
    c.at(23, 11) = cell(U'█', LightGreen, Magenta, attr::Bold);
    c.at(24, 11) = cell(U'█', LightCyan, Brown, attr::Underline);
    c.at(25, 11) = cell(U'█', White, Green, attr::Bold | attr::Italic | attr::Underline);
    return d;
}

AnsiDocument load(const fs::path& p) {
    const std::string data = readFile(p);
    return parseArt(data, artFormatOf(p.string(), data));
}

} // namespace

TEST_CASE("Micron writes each colour in three digits when it can, else in six") {
    CHECK(micron::colourCode(kPalette[Black]) == "000");
    CHECK(micron::colourCode(kPalette[Brown]) == "a50");
    CHECK(micron::colourCode(kPalette[LightBlue]) == "55f");
    CHECK(micron::colourCode(kPalette[White]) == "fff");
    for (const Rgb& c : kPalette)
        CHECK(micron::colourCode(c).size() == 3);
    CHECK(micron::colourCode({0x12, 0x34, 0x56}) == "T123456");
    CHECK(micron::colourCode({0x11, 0x22, 0x34}) == "T112234");
    CHECK(micron::colourCode({0xfc, 0x00, 0xa8}) == "Tfc00a8");
}

TEST_CASE("Micron writes a colour code only where the colour changes") {
    CHECK(line(text(U"abc", Yellow, Blue)) == "`Fff5`B00aabc``");
    CHECK(line(row({cell(U'a', Red, Blue), cell(U'b', Red, Green), cell(U'c', White, Green)})) ==
          "`Fa00`B00aa`B0a0b`Ffffc``");
    // a colour the cell doesn't show isn't written: a blank's foreground, a
    // full block's background
    CHECK(line(row({cell(U'x', Red, Black), cell(U' ', White, Black), cell(U'x', Red, Black)})) == "`Fa00`B000x x``");
    CHECK(line(row({cell(U' ', White, Blue), cell(U'█', Red, Green), cell(U' ', Grey, Blue)})) ==
          "`B00a `Fa00█ ``");
    // a blank row is its background and its spaces
    CHECK(line(text(U"    ")) == "`B000    ``");
}

TEST_CASE("Micron keeps trailing blanks, black ones too") {
    CHECK(line(text(U"ab   ")) == "`Faaa`B000ab   ``");
    CHECK(line(row({cell(U'a', White, Black), cell(U' ', White, Blue), cell(U' ', White, Black)})) ==
          "`Ffff`B000a`B00a `B000 ``");
}

TEST_CASE("Micron escapes backticks and backslashes, and #! so no text reads as a header") {
    CHECK(line(text(U"a`b")) == "`Faaa`B000a\\`b``");
    CHECK(line(text(U"a\\b")) == "`Faaa`B000a\\\\b``");
    CHECK(line(text(U"``")) == "`Faaa`B000\\`\\```");
    CHECK(line(text(U"#!bg=abc")) == "`Faaa`B000#\\!bg=abc``");
    CHECK(line(text(U"!#!")) == "`Faaa`B000!#\\!``");
    // a colour change between # and ! breaks the header already
    CHECK(line(row({cell(U'#', Red, Black), cell(U'!', White, Black)})) == "`Fa00`B000#`Ffff!``");
    // what only formatting mode reads is plain text
    CHECK(line(text(U"[a`b]<c>")) == "`Faaa`B000[a\\`b]<c>``");
}

TEST_CASE("No Micron line starts with a character Micron gives meaning to") {
    for (const std::u32string_view s : {U">h", U">>h", U"-", U"-x", U"#c", U"#!c=0", U"<", U"\\", U"`=", U"`t", U"`{",
                                        U"`(", U"`+>", U"=", U"[", U" "}) {
        INFO(utf8(s));
        const std::string l = line(text(s));
        CHECK(l.rfind("`", 0) == 0);
        CHECK((l[1] == 'F' || l[1] == 'B'));
        CHECK(l != "`=");
        const std::string centred = line(text(s), true, {.centre = true});
        CHECK(centred.rfind("`c`", 0) == 0);
    }
}

TEST_CASE("Micron draws iCE bright backgrounds as the editor shows them") {
    const Cell blink = cell(U'x', White, Blue, attr::Blink);
    CHECK(line(row({blink}), true) == "`Ffff`B55fx``");
    CHECK(line(row({blink}), false) == "`Ffff`B00ax``");
    const Cell bright = cell(U'x', White, LightBlue);
    CHECK(line(row({bright}), true) == "`Ffff`B55fx``");
    CHECK(line(row({bright}), false) == "`Ffff`B00ax``");
    for (std::uint8_t i = 8; i < 16; ++i) {
        const Cell c = cell(U'▀', i, i);
        CHECK(line(row({c})) == "`F" + micron::colourCode(kPalette[i]) + "`B" + micron::colourCode(kPalette[i]) +
                                     "▀``");
    }
}

TEST_CASE("Micron writes a piece in its own XBIN palette") {
    AnsiDocument d;
    d.canvas = text(U"ab", Blue, Red);
    std::array<Rgb, 16> pal = kPalette;
    pal[Blue] = {0x10, 0x20, 0x30};
    pal[Red] = {0x44, 0x88, 0xcc};
    d.palette = pal;
    CHECK(micron::encode(d) == "`FT102030`B48cab``\n");
    const AnsiDocument tux = load(dataDir() / "xbin" / "tux-palette-font.xb");
    REQUIRE(customPalette(tux));
    CHECK(micron::encode(tux).find("`FT") != std::string::npos);
    // an RGB cell keeps its colour
    Cell rgb = cell(U'x', White, Black);
    rgb.fg = Color::fromRgb({0x12, 0x34, 0x56});
    rgb.bg = Color::fromRgb({0xaa, 0xbb, 0xcc});
    CHECK(line(row({rgb})) == "`FT123456`Babcx``");
}

TEST_CASE("Micron writes glyphs as the IRC export does") {
    // CP437 controls as their glyphs, NUL a space, DEL its house
    CHECK(line(text(U"\u0001\u001b\u007f")) == "`Faaa`B000☺←⌂``");
    CHECK(line(text(std::u32string_view(U"a\0b", 3))) == "`Faaa`B000a b``");
    // a wide pair once, its continuation nothing
    Canvas wide = text(U"中 x");
    wide.at(1, 0).ch = Glyph::continuation();
    CHECK(line(wide) == "`Faaa`B000中x``");
    // a transparent cell is blank
    Canvas t = text(U"ab");
    t.at(0, 0).transparent = true;
    CHECK(line(t) == "`B000 `Faaab``");
    CHECK(micron::unwritable(wide).empty());
}

TEST_CASE("Micron writes '?' for a code point NomadNet drops, keeping the columns") {
    CHECK(micron::droppedByNomadNet(0x200b));
    CHECK(micron::droppedByNomadNet(0x0301));
    CHECK(micron::droppedByNomadNet(0xfe0f));
    CHECK(micron::droppedByNomadNet(0x261d));
    CHECK(micron::droppedByNomadNet(0x85));
    CHECK_FALSE(micron::droppedByNomadNet(U'☺'));
    CHECK_FALSE(micron::droppedByNomadNet(U'█'));
    CHECK_FALSE(micron::droppedByNomadNet(U' '));
    CHECK_FALSE(micron::droppedByNomadNet(U'`'));
    Canvas c = text(U"a​c");
    const std::vector<char32_t> mark{U'e', 0x301};
    c.at(2, 0).ch = *clusterPool().intern(mark, PoolCap::Uncapped);
    CHECK(line(c) == "`Faaa`B000a??``");
    const auto lost = micron::unwritable(c);
    REQUIRE(lost.size() == 2);
    CHECK(lost[0].x == 1);
    CHECK(lost[0].loss == Loss::NoCode);
    CHECK(lost[1].x == 2);
    // a C1 control, as IRC text writes it
    CHECK(line(text(U"\u0085")) == "`Faaa`B000?``");
}

TEST_CASE("Micron writes formatting toggles where they change") {
    CHECK(line(row({cell(U'a', White, Black, attr::Bold), cell(U'b', White, Black, attr::Bold | attr::Underline),
                    cell(U'c', White, Black)})) == "`Ffff`B000`!a`_b`!`_c``");
    // an underlined blank shows its foreground
    CHECK(line(row({cell(U' ', Red, Black, attr::Underline)})) == "`Fa00`B000`_ ``");
}

TEST_CASE("Micron writes the background of an italic full block, sheared off it") {
    CHECK(line(row({cell(U'X', Grey, Red), cell(U'█', Grey, Blue, attr::Italic)})) == "`Faaa`Ba00X`B00a`*█``");
    CHECK(line(row({cell(U'X', Grey, Red), cell(U'█', Grey, Blue, attr::Bold | attr::Italic)})) ==
          "`Faaa`Ba00X`B00a`!`*█``");
    // bold and underline leave a full block covering its cell
    CHECK(line(row({cell(U'X', Grey, Red), cell(U'█', Grey, Blue, attr::Bold)})) == "`Faaa`Ba00X`!█``");
    CHECK(line(row({cell(U'X', Grey, Red), cell(U'█', Grey, Blue, attr::Underline)})) == "`Faaa`Ba00X`_█``");
}

TEST_CASE("Micron centres on request and writes the cache header first") {
    const Canvas c = text(U"hi");
    CHECK(micron::encodeCanvas(c, kPalette, true) == "`Faaa`B000hi``\n");
    CHECK(micron::encodeCanvas(c, kPalette, true, {.centre = true}) == "`c`Faaa`B000hi`f`b\n");
    CHECK(micron::encodeCanvas(c, kPalette, true, {.cacheSeconds = 0u}) == "#!c=0\n`Faaa`B000hi``\n");
    CHECK(micron::encodeCanvas(c, kPalette, true, {.centre = true, .cacheSeconds = 3600u}) ==
          "#!c=3600\n`c`Faaa`B000hi`f`b\n");
    // a centred line closes what it turned on without resetting the alignment
    CHECK(line(row({cell(U'a', White, Black, attr::Bold | attr::Underline)}), true, {.centre = true}) ==
          "`c`Ffff`B000`!`_a`!`_`f`b");
}

TEST_CASE("Micron golden pages") {
    const AnsiDocument s = swatch();
    golden("swatch.mu", micron::encode(s));
    golden("swatch.json", expectedCells(s.canvas, kPalette, true, false));
    golden("swatch-centre.mu", micron::encode(s, {.centre = true, .cacheSeconds = 0u}));
    golden("swatch-centre.json", expectedCells(s.canvas, kPalette, true, true));
    AnsiDocument off = s;
    off.ice = false;
    golden("swatch-ice-off.mu", micron::encode(off));
    golden("swatch-ice-off.json", expectedCells(off.canvas, kPalette, false, false));
    for (const char* name : {"durin-gate", "darkmage"}) {
        const AnsiDocument d = load(dataDir() / "golden" / (std::string(name) + ".ans"));
        CHECK(micron::unwritable(d.canvas).empty());
        golden(std::string(name) + ".mu", micron::encode(d));
        golden(std::string(name) + ".json", expectedCells(d.canvas, kPalette, d.ice, false));
    }
    const AnsiDocument tux = load(dataDir() / "xbin" / "tux-palette-font.xb");
    golden("tux-palette-font.mu", micron::encode(tux));
    golden("tux-palette-font.json", expectedCells(tux.canvas, *tux.palette, tux.ice, false));
}

// Opening a page the writer wrote gives back the cells it was written from,
// as NomadNet shows them: glyphs, colours and formatting (the golden .json),
// and writing it again gives the same page.
TEST_CASE("Micron golden pages open to the cells they were written from") {
    for (const char* name : {"swatch", "swatch-centre", "swatch-ice-off", "durin-gate", "darkmage", "tux-palette-font"}) {
        INFO(name);
        const bool centre = std::string(name) == "swatch-centre";
        const std::string mu = readFile(dataDir() / "golden-micron" / (std::string(name) + ".mu"));
        const AnsiDocument back = micron::parse(mu);
        CHECK(expectedCells(back.canvas, kPalette, true, centre) ==
              readFile(dataDir() / "golden-micron" / (std::string(name) + ".json")));
        CHECK(micron::encode(back, {.centre = centre, .cacheSeconds = centre ? std::optional(0u) : std::nullopt}) ==
              mu);
        if (centre) {
            CHECK(back.readLoss.micron.aligned.count == back.canvas.height());
            CHECK(micron::readNotes(back.readLoss).size() == 1);
        } else {
            CHECK_FALSE(back.readLoss.lossy());
        }
    }
}

TEST_CASE("Micron pages of random cells open to the same cells") {
    static constexpr char32_t kGlyphs[] = U" A z0`\\[]<>#-=!*_|:{}()\u00a0\u2588\u2580\u2584\u258c\u2590\u2591\u2592\u2593\u263a\u2302";
    std::uint32_t seed = 0x4d75u;
    const auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int i = 0; i < 400; ++i) {
        AnsiDocument d;
        d.ice = next() % 2;
        d.canvas = Canvas(1 + int(next() % 24), 1 + int(next() % 6));
        Canvas& c = d.canvas;
        for (int y = 0; y < c.height(); ++y) {
            for (int x = 0; x < c.width(); ++x) {
                const std::uint8_t attrs = std::uint8_t(next() % 16) &
                                           (attr::Bold | attr::Italic | attr::Underline | attr::Blink);
                const bool wide = x + 1 < c.width() && next() % 12 == 0;
                c.at(x, y) = cell(wide ? U'\u4e2d' : kGlyphs[next() % (std::size(kGlyphs) - 1)],
                                  std::uint8_t(next() % 16), std::uint8_t(next() % 16), attrs);
                if (wide) {
                    c.at(x + 1, y) = c.at(x, y);
                    c.at(++x, y).ch = Glyph::continuation();
                }
            }
        }
        REQUIRE(micron::unwritable(c).empty());
        const std::string mu = micron::encode(d);
        INFO(mu);
        const AnsiDocument back = micron::parse(mu);
        CHECK(back.canvas.width() == c.width());
        CHECK(back.canvas.height() == c.height());
        CHECK(expectedCells(back.canvas, kPalette, true, false) == expectedCells(c, kPalette, d.ice, false));
        CHECK_FALSE(back.readLoss.lossy());
    }
}
