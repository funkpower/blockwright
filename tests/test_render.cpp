// Renderer unit tests: glyph width, the 9th column, iCE vs blink colours,
// 24-bit colours, scaling and the bundled font.

#include "util.h"

#include "blockwright/cp437.h"
#include "blockwright/glyph_sets.h"
#include "blockwright/render.h"
#include "unifont.h"  // render/src: the bitmaps the wide and cluster cases expect

#include <catch2/catch_test_macros.hpp>

#include <QCryptographicHash>
#include <QImage>

#include <stdexcept>
#include <vector>

using namespace bw;

namespace {

QRgb q(Rgb c) { return qRgb(c.r, c.g, c.b); }

// A document holding one cell.
Canvas oneCell(char32_t ch, Color fg, Color bg) {
    Canvas cv(1, 1);
    cv.at(0, 0) = Cell{ch, fg, bg, 0, false};
    return cv;
}

// `body` with a SAUCE record whose flags are `flags`.
std::string withSauce(const std::string& body, std::uint8_t flags) {
    Sauce s = Sauce::fresh(80, 1);
    s.flags = flags;
    return body + encodeSauce(s, static_cast<std::uint32_t>(body.size()));
}

// Row y of a glyph's own bitmap, bit x column x: Unifont's, or the VGA
// font's for a CP437 glyph; and how many columns it has.
std::uint16_t fontRow(char32_t cp, int y, int* width = nullptr) {
    std::uint16_t row = 0;
    if (const std::optional<std::uint8_t> b = unicodeToCp437(cp)) {
        for (int x = 0; x < 8; ++x)
            row |= std::uint16_t(vgaFont().pixel(*b, x, y) << x);
        if (width)
            *width = 8;
        return row;
    }
    const std::optional<UnifontGlyph> u = unifontGlyph(cp);
    REQUIRE(u);
    for (int x = 0; x < (u->wide ? 16 : 8); ++x)
        row |= std::uint16_t(u->pixel(x, y) << x);
    if (width)
        *width = u->wide ? 16 : 8;
    return row;
}

// Fails unless the cell at column `cx` of the one-row image `img` (cells
// `gw` wide) is `rows` (bit 8 the ninth column) in `fg` on `bg`.
void checkCell(const QImage& img, int cx, int gw, const std::array<std::uint16_t, 16>& rows, Rgb fg, Rgb bg) {
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < gw; ++x)
            if (img.pixel(cx * gw + x, y) != q((rows[y] >> x) & 1 ? fg : bg))
                FAIL("cell " << cx << " pixel " << x << "," << y << " (width " << gw << ")");
}

Glyph cluster(std::vector<char32_t> cps) {
    const std::optional<Glyph> g = clusterPool().intern(cps, PoolCap::Capped);
    REQUIRE(g);
    return *g;
}

} // namespace

TEST_CASE("glyph width follows the SAUCE letter-spacing flag") {
    AnsiDocument doc = bwtest::parse("A");
    CHECK(glyphWidth(doc) == 8);  // no SAUCE
    doc.sauce = Sauce::fresh(80, 1);
    doc.sauce->setLetterSpacing(0);
    CHECK(glyphWidth(doc) == 8);
    doc.sauce->setLetterSpacing(1);
    CHECK(glyphWidth(doc) == 8);
    doc.sauce->setLetterSpacing(2);
    CHECK(glyphWidth(doc) == 9);
    CHECK(glyphWidth(doc, {8, 1}) == 8);  // an explicit width wins
    doc.sauce->setLetterSpacing(1);
    CHECK(glyphWidth(doc, {9, 1}) == 9);

    const QImage img = renderDocument(parseAnsi(withSauce("AB", 0x05)));  // 9 px, iCE
    CHECK(img.width() == 80 * 9);
    CHECK(img.height() == 16);
}

TEST_CASE("9 px glyphs repeat column 8 only for 0xC0-0xDF") {
    const Color fg = Color::indexed(White), bg = Color::indexed(Blue);
    // ─ (0xC4), ▀ (0xDF) and └ (0xC0) carry their 8th column into the 9th
    for (char32_t ch : {U'─', U'▀', U'└', U'█'}) {
        const QImage img = renderCanvas(oneCell(ch, fg, bg), 9);
        REQUIRE(img.width() == 9);
        int lit = 0;
        for (int y = 0; y < 16; ++y) {
            CHECK(img.pixel(8, y) == img.pixel(7, y));
            lit += img.pixel(8, y) == q(kPalette[White]);
        }
        CHECK(lit > 0);
    }
    // ▓ (0xB2) and ▒ (0xB1) have the 8th column lit but the 9th stays background
    for (char32_t ch : {U'▓', U'▒'}) {
        const QImage img = renderCanvas(oneCell(ch, fg, bg), 9);
        int lit7 = 0;
        for (int y = 0; y < 16; ++y) {
            lit7 += img.pixel(7, y) == q(kPalette[White]);
            CHECK(img.pixel(8, y) == q(kPalette[Blue]));
        }
        CHECK(lit7 > 0);
    }
    // 8 px drops nothing: the same glyph is the same 8 columns
    const QImage nine = renderCanvas(oneCell(U'▓', fg, bg), 9), eight = renderCanvas(oneCell(U'▓', fg, bg), 8);
    CHECK(nine.copy(0, 0, 8, 16) == eight);
}

TEST_CASE("blink is a bright background with iCE on and a dark one with iCE off") {
    const std::string body = "\x1b[5;31;44m ";  // a blinking space: background only
    const QImage ice = renderDocument(parseAnsi(withSauce(body, 0x01)));
    const QImage noIce = renderDocument(parseAnsi(withSauce(body, 0x00)));
    const QImage noSauce = renderDocument(bwtest::parse(body));
    CHECK(ice.pixel(0, 0) == q(kPalette[LightBlue]));
    CHECK(noIce.pixel(0, 0) == q(kPalette[Blue]));
    CHECK(noSauce.pixel(0, 0) == q(kPalette[LightBlue]));  // iCE defaults to on

    // Without iCE a blinking glyph shows its "on" phase: drawn, not hidden
    const QImage glyph = renderDocument(parseAnsi(withSauce("\x1b[5;1;33;41m\xdb", 0x00)));
    CHECK(glyph.pixel(3, 8) == q(kPalette[Yellow]));
    const QImage half = renderDocument(parseAnsi(withSauce("\x1b[5;1;33;41m\xdc", 0x00)));
    CHECK(half.pixel(3, 0) == q(kPalette[Red]));
    CHECK(half.pixel(3, 15) == q(kPalette[Yellow]));
}

TEST_CASE("cells draw their palette colours, and 24-bit colours as themselves") {
    for (int i = 0; i < 16; ++i) {
        const QImage img = renderCanvas(oneCell(U'█', Color::indexed(std::uint8_t(i)), Color::indexed(Black)), 8);
        CHECK(img.pixel(4, 4) == q(kPalette[i]));
    }
    const Rgb fg{0x12, 0x34, 0x56}, bg{0xfe, 0xdc, 0xba};
    const QImage img = renderCanvas(oneCell(U'▀', Color::fromRgb(fg), Color::fromRgb(bg)), 8);
    CHECK(img.pixel(0, 0) == q(fg));
    CHECK(img.pixel(0, 15) == q(bg));
}

TEST_CASE("integer scaling is nearest neighbour") {
    const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / "golden" / "tux.ans"));
    const QImage one = renderDocument(doc);
    for (int s : {2, 3}) {
        const QImage big = renderDocument(doc, {0, s});
        REQUIRE(big.size() == one.size() * s);
        CHECK(big == one.scaled(one.size() * s, Qt::IgnoreAspectRatio, Qt::FastTransformation));
    }
}

TEST_CASE("bad widths and scales are refused") {
    const Canvas cv = oneCell(U'A', Color::indexed(Grey), Color::indexed(Black));
    CHECK_THROWS_AS(renderCanvas(cv, 7), std::invalid_argument);
    CHECK_THROWS_AS(renderCanvas(cv, 8, 0), std::invalid_argument);
    CHECK_THROWS_AS(renderCanvas(cv, 8, kMaxScale + 1), std::invalid_argument);
    CHECK_NOTHROW(renderCanvas(cv, 9, kMaxScale));
    CHECK_THROWS_AS(renderCanvas(Canvas(kMaxColumns, kMaxRows), 9, kMaxScale), std::length_error);
}

TEST_CASE("an empty canvas is refused as empty, not as too large") {
    for (const Canvas& cv : {Canvas(0, 0), Canvas(0, 1), Canvas(1, 0)}) {
        INFO(cv.width() << "x" << cv.height());
        try {
            renderCanvas(cv, 8);
            FAIL("no exception");
        } catch (const std::invalid_argument& e) {
            CHECK(std::string(e.what()) == "empty canvas");
        }
    }
    AnsiDocument doc;
    doc.canvas = Canvas(0, 0);
    CHECK_THROWS_AS(renderDocument(doc), std::invalid_argument);
}

TEST_CASE("transparent cells draw like opaque ones") {
    Canvas cv(3, 1);
    cv.at(0, 0) = Cell{U'▀', Color::indexed(LightRed), Color::indexed(Blue), 0, false};
    cv.at(1, 0) = Cell{U'A', Color::fromRgb({0x12, 0x34, 0x56}), Color::indexed(Brown), 0, false};
    cv.at(2, 0) = Cell{};
    Canvas clear = cv;
    for (int x = 0; x < 3; ++x)
        clear.at(x, 0).transparent = true;
    for (int gw : {8, 9}) {
        const QImage img = renderCanvas(clear, gw);
        CHECK(img == renderCanvas(cv, gw));
        CHECK(img.pixel(0, 0) == q(kPalette[LightRed]));
        CHECK(img.pixel(0, 15) == q(kPalette[Blue]));
    }
}

TEST_CASE("a glyph neither font has draws as '?'") {
    CHECK(glyphIndex(U'A') == 0x41);
    CHECK(glyphIndex(U'█') == 0xDB);
    CHECK(glyphIndex(U'★') == '?');
    const Color fg = Color::indexed(Grey), bg = Color::indexed(Black);
    // the private use areas are left out of the Unifont table
    CHECK(!unifontGlyph(0xE000));
    CHECK(!unifontGlyph(0x10FFFD));
    for (int gw : {8, 9}) {
        CHECK(renderCanvas(oneCell(0xE000, fg, bg), gw) == renderCanvas(oneCell(U'?', fg, bg), gw));
        CHECK(renderCanvas(oneCell(0x10FFFD, fg, bg), gw) == renderCanvas(oneCell(U'?', fg, bg), gw));
        // and a cluster on such a base
        Canvas cv(1, 1);
        cv.at(0, 0) = Cell{cluster({0xE000, 0x301}), fg, bg, 0, false};
        CHECK(renderCanvas(cv, gw) == renderCanvas(oneCell(U'?', fg, bg), gw));
    }
}

TEST_CASE("a glyph with no CP437 code draws in Unifont (PLAN §16)") {
    // the Cyrillic Zhe and the Hebrew alef are narrow
    CHECK(unifontGlyphCount() > 60000);
    const Rgb fg = kPalette[LightGreen], bg = kPalette[Blue];
    for (char32_t cp : {char32_t(0x0416), char32_t(0x05D0)}) {
        INFO("U+" << std::hex << unsigned(cp));
        int width = 0;
        fontRow(cp, 0, &width);
        REQUIRE(width == 8);
        std::array<std::uint16_t, 16> rows{};
        for (int y = 0; y < 16; ++y)
            rows[y] = fontRow(cp, y);
        for (int gw : {8, 9}) {
            const QImage img = renderCanvas(oneCell(cp, Color::indexed(LightGreen), Color::indexed(Blue)), gw);
            checkCell(img, 0, gw, rows, fg, bg);  // the ninth column is background
        }
    }
}

TEST_CASE("every glyph of the Unicode F-key sets draws in one cell (#350 slice 2)") {
    // those CP437 lacks (▚, ★) come from Unifont; the few Unifont draws 16
    // columns wide (★, ☆), though §16 counts them one cell, are squeezed
    // into it, each pair of columns OR-ed into one
    const Rgb fg = kPalette[Yellow], bg = kPalette[Black];
    int unifontOnly = 0;
    std::string squeezed;
    for (int s = kCp437GlyphSetCount; s < kGlyphSetCount; ++s)
        for (int k = 0; k < kGlyphSetKeys; ++k) {
            const char32_t cp = glyphSetGlyph(s, k);
            INFO("set " << s + 1 << " F" << k + 1 << " U+" << std::hex << unsigned(cp));
            if (!unicodeToCp437(cp)) {
                ++unifontOnly;
                REQUIRE(unifontGlyph(cp));
            }
            int width = 0;
            fontRow(cp, 0, &width);
            std::array<std::uint16_t, 16> rows{};
            bool inked = false;
            for (int y = 0; y < 16; ++y) {
                std::uint16_t row = fontRow(cp, y);
                if (width == 16) {
                    std::uint16_t out = 0;
                    for (int i = 0; i < 8; ++i)
                        if ((row >> (2 * i)) & 3)
                            out |= std::uint16_t(1u << i);
                    row = out;
                }
                inked |= (rows[y] = row) != 0;
            }
            if (width == 16)
                squeezed += QString::fromUcs4(&cp, 1).toStdString();
            CHECK(inked);
            checkCell(renderCanvas(oneCell(cp, Color::indexed(Yellow), Color::indexed(Black)), 8), 0, 8, rows, fg, bg);
        }
    CHECK(unifontOnly > 20);
    CHECK(squeezed == "★☆");
}

TEST_CASE("a wide glyph draws its left half in the lead and its right in the continuation (PLAN §16)") {
    // each half in its own cell's colours; the lead's ninth column repeats
    // its eighth, so the glyph joins across the gap, and the continuation's
    // is background
    Canvas cv(3, 1);
    cv.at(0, 0) = Cell{U'中', Color::indexed(Yellow), Color::indexed(Blue), 0, false};
    cv.at(1, 0) = Cell{Glyph::continuation(), Color::indexed(White), Color::indexed(Red), 0, false};
    cv.at(2, 0) = Cell{U'A', Color::indexed(Yellow), Color::indexed(Blue), 0, false};
    int width = 0;
    fontRow(U'中', 0, &width);
    REQUIRE(width == 16);
    std::array<std::uint16_t, 16> left{}, right{};
    for (int y = 0; y < 16; ++y) {
        const std::uint16_t r = fontRow(U'中', y);
        left[y] = std::uint16_t((r & 0xff) | ((r >> 7) & 1) << 8);
        right[y] = std::uint16_t(r >> 8);
    }
    for (int gw : {8, 9}) {
        const QImage img = renderCanvas(cv, gw);
        checkCell(img, 0, gw, left, kPalette[Yellow], kPalette[Blue]);
        checkCell(img, 1, gw, right, kPalette[White], kPalette[Red]);
        CHECK(img.copy(2 * gw, 0, gw, 16) == renderCanvas(oneCell(U'A', Color::indexed(Yellow), Color::indexed(Blue)), gw));
    }
    // a lead whose glyph is narrow draws it whole, its continuation blank
    cv.at(0, 0).ch = U'A';
    const QImage narrow = renderCanvas(cv, 9);
    CHECK(narrow.copy(0, 0, 8, 16) == renderCanvas(oneCell(U'A', Color::indexed(Yellow), Color::indexed(Blue)), 9).copy(0, 0, 8, 16));
    checkCell(narrow, 1, 9, {}, kPalette[White], kPalette[Red]);
    // a continuation with no lead (in column 0, or after another) is blank
    Canvas lone(2, 1);
    lone.at(0, 0) = Cell{Glyph::continuation(), Color::indexed(White), Color::indexed(Red), 0, false};
    lone.at(1, 0) = lone.at(0, 0);
    const QImage blank = renderCanvas(lone, 9);
    checkCell(blank, 0, 9, {}, kPalette[White], kPalette[Red]);
    checkCell(blank, 1, 9, {}, kPalette[White], kPalette[Red]);
}

TEST_CASE("a wide glyph alone in a cell is squeezed into it (PLAN §16)") {
    // no continuation after it: each pair of columns ORed into one
    for (char32_t cp : {U'中', U'★', char32_t(0x1F600)}) {
        INFO("U+" << std::hex << unsigned(cp));
        int width = 0;
        fontRow(cp, 0, &width);
        REQUIRE(width == 16);
        std::array<std::uint16_t, 16> rows{};
        for (int y = 0; y < 16; ++y) {
            const std::uint16_t r = fontRow(cp, y);
            for (int x = 0; x < 8; ++x)
                if ((r >> (2 * x)) & 3)
                    rows[y] |= std::uint16_t(1u << x);
        }
        for (int gw : {8, 9})
            checkCell(renderCanvas(oneCell(cp, Color::indexed(White), Color::indexed(Black)), gw), 0, gw, rows,
                      kPalette[White], kPalette[Black]);
    }
}

TEST_CASE("a cluster draws its NFC form, else its base with its marks over it (PLAN §16)") {
    const Color fg = Color::indexed(LightCyan), bg = Color::indexed(Magenta);
    auto draw = [&](Glyph g, int gw) {
        Canvas cv(1, 1);
        cv.at(0, 0) = Cell{g, fg, bg, 0, false};
        return renderCanvas(cv, gw);
    };
    for (int gw : {8, 9}) {
        INFO("width " << gw);
        // e + acute is é, CP437 0x82, drawn from the VGA font
        CHECK(draw(cluster({U'e', 0x301}), gw) == renderCanvas(oneCell(U'é', fg, bg), gw));
        // q + tilde has no precomposed form: the VGA q with Unifont's tilde over it
        std::array<std::uint16_t, 16> rows{};
        for (int y = 0; y < 16; ++y)
            rows[y] = fontRow(U'q', y) | fontRow(0x303, y);
        checkCell(draw(cluster({U'q', 0x303}), gw), 0, gw, rows, kPalette[LightCyan], kPalette[Magenta]);
        // the marks in any order normalise to the same drawing
        CHECK(draw(cluster({U'q', 0x323, 0x303}), gw) == draw(cluster({U'q', 0x303, 0x323}), gw));
        // a variation selector and a joiner draw nothing: a joined sequence
        // draws its first base
        CHECK(draw(cluster({U'e', 0xFE0F}), gw) == renderCanvas(oneCell(U'e', fg, bg), gw));
        CHECK(draw(cluster({U'a', 0x200D, U'b'}), gw) == renderCanvas(oneCell(U'a', fg, bg), gw));
    }
    // a cluster on a wide base in a pair draws across both cells, the mark
    // centred over the base
    Canvas pair(2, 1), plain(2, 1);
    pair.at(0, 0) = Cell{cluster({U'中', 0x301}), fg, bg, 0, false};
    pair.at(1, 0) = Cell{Glyph::continuation(), fg, bg, 0, false};
    plain.at(0, 0) = Cell{U'中', fg, bg, 0, false};
    plain.at(1, 0) = pair.at(1, 0);
    const QImage got = renderCanvas(pair, 8), base = renderCanvas(plain, 8);
    for (int y = 0; y < 16; ++y) {
        const std::uint16_t mark = std::uint16_t(fontRow(0x301, y) << 4);
        for (int x = 0; x < 16; ++x)
            CHECK((got.pixel(x, y) == q(kPalette[LightCyan])) ==
                  ((base.pixel(x, y) == q(kPalette[LightCyan])) || ((mark >> x) & 1)));
    }
}

TEST_CASE("wide glyphs and clusters match their golden render", "[golden]") {
#ifdef BW_BUNDLED_UNIFONT
    // A row of the cases above, at 9 and 8 pixels: tests/data/render/unifont/,
    // made from the bundled Unifont 18.0.01 (a system Unifont may draw them
    // otherwise). BW_WRITE_GOLDENS=1 writes them afresh.
    Canvas cv(10, 2);
    const Color fg = Color::indexed(White), bg = Color::indexed(Blue);
    const Glyph row[10] = {U'中', Glyph::continuation(), U'文', Glyph::continuation(), U'中',
                           cluster({U'q', 0x303}), cluster({U'e', 0x301}), U'Ж', char32_t(0x1F600), U'A'};
    for (int x = 0; x < 10; ++x) {
        cv.at(x, 0) = Cell{row[x], fg, bg, 0, false};
        cv.at(x, 1) = Cell{row[x], Color::indexed(Black), Color::indexed(x % 2 ? Grey : Yellow), 0, false};
    }
    cv.at(9, 1).ch = 0xE000;
    for (int gw : {8, 9}) {
        const QImage img = renderCanvas(cv, gw);
        const auto path = bwtest::dataDir() / "render" / "unifont" / ("wide-" + std::to_string(gw) + ".png");
        if (qEnvironmentVariableIsSet("BW_WRITE_GOLDENS"))
            REQUIRE(img.save(QString::fromStdString(path.string())));
        const QImage want(QString::fromStdString(path.string()));
        REQUIRE(!want.isNull());
        CHECK(img == want.convertToFormat(img.format()));
    }
#else
    SKIP("built against a system Unifont");
#endif
}

TEST_CASE("the bundled font is the IBM VGA 8x16") {
    const BitmapFont& f = vgaFont();
    REQUIRE(f.height == 16);
    REQUIRE(f.bits.size() == 256 * 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 8; ++x) {
            CHECK(f.pixel(0xDB, x, y));      // █
            CHECK(!f.pixel(0x20, x, y));     // space
            CHECK(f.pixel(0xDC, x, y) == (y >= 7));  // ▄
            CHECK(f.pixel(0xDD, x, y) == (x < 4));   // ▌
        }
    // 'A' and the broken bar the IBM ROM draws for '|'
    const std::uint8_t a[16] = {0, 0, 0x10, 0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0xc6, 0, 0, 0, 0};
    const std::uint8_t bar[16] = {0, 0, 0x18, 0x18, 0x18, 0x18, 0, 0x18, 0x18, 0x18, 0x18, 0x18, 0, 0, 0, 0};
    for (int y = 0; y < 16; ++y) {
        CHECK(f.bits[0x41 * 16 + y] == a[y]);
        CHECK(f.bits[0x7C * 16 + y] == bar[y]);
    }
}

TEST_CASE("the bundled font table is the one in VileR's pack, byte for byte") {
    // SHA-256 of the 4096 bytes fon2inc.py extracts from Bm437_IBM_VGA_8x16.FON
    // in oldschool_pc_font_pack_v2.2_FULL.zip (third_party/oldschool-pc-fonts/README.md)
    const BitmapFont& f = vgaFont();
    REQUIRE(f.bits.size() == 256 * 16);
    const QByteArray bytes(reinterpret_cast<const char*>(f.bits.data()), static_cast<qsizetype>(f.bits.size()));
    CHECK(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString() ==
          "a8bad6fd78475a6bc2a05438c19207a9bb8c0f4f4099f60384e158cdc3eba580");
}

TEST_CASE("glyphIndex, for a font of one byte a glyph, keeps a space for the continuation and '?' for the rest") {
    // what the 8-pixel fonts and the CP437 fast path draw
    CHECK(glyphIndex(Glyph::continuation()) == ' ');
    CHECK(glyphIndex(cluster({U'e', 0x301})) == '?');
    CHECK(glyphIndex(Glyph(U'░')) == 0xB0);
    CHECK(glyphIndex(Glyph(U'中')) == '?');
}
