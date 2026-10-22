// Renderer unit tests: glyph width, the 9th column, iCE vs blink colours,
// 24-bit colours, scaling and the bundled font.

#include "util.h"

#include "blockwright/render.h"

#include <catch2/catch_test_macros.hpp>

#include <QCryptographicHash>

#include <stdexcept>

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

TEST_CASE("glyphs without a CP437 byte draw as '?'") {
    CHECK(glyphIndex(U'A') == 0x41);
    CHECK(glyphIndex(U'█') == 0xDB);
    CHECK(glyphIndex(U'★') == '?');
    const Color fg = Color::indexed(Grey), bg = Color::indexed(Black);
    CHECK(renderCanvas(oneCell(U'★', fg, bg), 8) == renderCanvas(oneCell(U'?', fg, bg), 8));
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
