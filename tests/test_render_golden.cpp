// Renders compared with libansilove's PNGs (tests/data/render/ansilove/,
// listed with the ansilove invocation in MANIFEST).
//
// Tolerance, and only this: libansilove's built-in "80x25" font is not the
// IBM VGA ROM font we bundle. 28 glyphs differ (kFontDiffers); a cell holding
// one of them may differ from the reference, but then each side is checked on
// its own: every reference pixel must be the cell's foreground or background
// colour, and our cell must be exactly the VGA bitmap for that glyph in those
// colours, 9th column included. The bitmaps are written out below, not taken
// from the renderer's table (test_render.cpp pins that table's digest).
// Everything else must be pixel-identical. Our canvas can be taller than
// libansilove's image (SAUCE TInfo2 is a minimum height for us, libansilove
// stops at the last drawn row); the extra rows must be empty black cells.

#include "util.h"

#include "blockwright/render.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <sstream>

using namespace bw;
namespace fs = std::filesystem;

namespace {

// CP437 codes whose bitmap differs between libansilove's font_pc_80x25.h
// (@ ced66a0) and the IBM VGA 8x16 from the Oldschool PC Font Pack: ` f v |
// ~ Ç ç Å É æ ¢ ƒ ª º ½ ¼ π Σ µ Φ Θ ± ⌡ ÷ · ⁿ ² ■. Each with its VGA bitmap,
// 16 rows, bit 7 leftmost, from Bm437_IBM_VGA_8x16.FON in the v2.2 pack.
const std::map<int, std::array<std::uint8_t, 16>> kFontDiffers = {
    {0x60, {0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x66, {0x00, 0x00, 0x38, 0x6c, 0x64, 0x60, 0xf0, 0x60, 0x60, 0x60, 0x60, 0xf0, 0x00, 0x00, 0x00, 0x00}},
    {0x76, {0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00}},
    {0x7C, {0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00}},
    {0x7E, {0x00, 0x00, 0x76, 0xdc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x80, {0x00, 0x00, 0x3c, 0x66, 0xc2, 0xc0, 0xc0, 0xc0, 0xc2, 0x66, 0x3c, 0x0c, 0x06, 0x7c, 0x00, 0x00}},
    {0x87, {0x00, 0x00, 0x00, 0x00, 0x3c, 0x66, 0x60, 0x60, 0x66, 0x3c, 0x0c, 0x06, 0x3c, 0x00, 0x00, 0x00}},
    {0x8F, {0x38, 0x6c, 0x38, 0x00, 0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00}},
    {0x90, {0x18, 0x30, 0x60, 0x00, 0xfe, 0x66, 0x60, 0x7c, 0x60, 0x60, 0x66, 0xfe, 0x00, 0x00, 0x00, 0x00}},
    {0x91, {0x00, 0x00, 0x00, 0x00, 0x00, 0xcc, 0x76, 0x36, 0x7e, 0xd8, 0xd8, 0x6e, 0x00, 0x00, 0x00, 0x00}},
    {0x9B, {0x00, 0x18, 0x18, 0x3c, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3c, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00}},
    {0x9F, {0x00, 0x0e, 0x1b, 0x18, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18, 0xd8, 0x70, 0x00, 0x00}},
    {0xA6, {0x00, 0x3c, 0x6c, 0x6c, 0x3e, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xA7, {0x00, 0x38, 0x6c, 0x6c, 0x38, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xAB, {0x00, 0xc0, 0xc0, 0xc2, 0xc6, 0xcc, 0x18, 0x30, 0x60, 0xdc, 0x86, 0x0c, 0x18, 0x3e, 0x00, 0x00}},
    {0xAC, {0x00, 0xc0, 0xc0, 0xc2, 0xc6, 0xcc, 0x18, 0x30, 0x66, 0xce, 0x9e, 0x3e, 0x06, 0x06, 0x00, 0x00}},
    {0xE3, {0x00, 0x00, 0x00, 0x00, 0xfe, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00}},
    {0xE4, {0x00, 0x00, 0x00, 0xfe, 0xc6, 0x60, 0x30, 0x18, 0x30, 0x60, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00}},
    {0xE6, {0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x7c, 0x60, 0x60, 0xc0, 0x00, 0x00, 0x00}},
    {0xE8, {0x00, 0x00, 0x00, 0x7e, 0x18, 0x3c, 0x66, 0x66, 0x66, 0x3c, 0x18, 0x7e, 0x00, 0x00, 0x00, 0x00}},
    {0xE9, {0x00, 0x00, 0x00, 0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0x6c, 0x38, 0x00, 0x00, 0x00, 0x00}},
    {0xF1, {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00}},
    {0xF5, {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xd8, 0xd8, 0xd8, 0x70, 0x00, 0x00, 0x00, 0x00}},
    {0xF6, {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x7e, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xFA, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xFC, {0x00, 0xd8, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xFD, {0x00, 0x70, 0xd8, 0x30, 0x60, 0xc8, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xFE, {0x00, 0x00, 0x00, 0x00, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

struct Ref {
    std::string png, piece;
    int width;
};

std::vector<Ref> manifest() {
    std::vector<Ref> out;
    std::istringstream in(bwtest::readFile(bwtest::dataDir() / "render" / "ansilove" / "MANIFEST"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        Ref r;
        ls >> r.png >> r.piece >> r.width;
        out.push_back(r);
    }
    return out;
}

QImage loadRef(const Ref& r) {
    const fs::path p = bwtest::dataDir() / "render" / "ansilove" / r.png;
    return QImage(QString::fromStdString(p.string())).convertToFormat(QImage::Format_RGB32);
}

// Glyph g, one of kFontDiffers, drawn in fg on bg, gw px wide, from the
// bitmap above. None of them is in 0xC0-0xDF, so the 9th column is background.
QImage vgaCell(std::uint8_t g, QRgb fg, QRgb bg, int gw) {
    const auto& rows = kFontDiffers.at(g);
    QImage img(gw, 16, QImage::Format_RGB32);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < gw; ++x)
            img.setPixel(x, y, x < 8 && rows[y] & (0x80 >> x) ? fg : bg);
    return img;
}

QImage solid(QRgb c, int gw) {
    QImage img(gw, 16, QImage::Format_RGB32);
    img.fill(c);
    return img;
}

// Compares our render of `doc` with libansilove's, within the tolerance
// above. Returns the first difference, or "" if there is none; counts the
// cells the font tolerance let through in *fontCells.
std::string compare(const AnsiDocument& doc, const QImage& ours, const QImage& ref, int gw, int* fontCells = nullptr) {
    std::ostringstream why;
    if (ref.isNull())
        return "no reference image";
    if (ours.width() != ref.width() || ours.height() < ref.height() || ref.height() % 16) {
        why << "ours is " << ours.width() << "x" << ours.height() << ", the reference " << ref.width() << "x"
            << ref.height();
        return why.str();
    }
    const int refRows = ref.height() / 16;
    for (int cy = 0; cy < doc.canvas.height(); ++cy) {
        for (int cx = 0; cx < doc.canvas.width(); ++cx) {
            const Cell& c = doc.canvas.at(cx, cy);
            const QRect cell(cx * gw, cy * 16, gw, 16);
            why << "cell at column " << cx + 1 << ", row " << cy + 1 << ": ";
            if (cy >= refRows) {
                if (ours.copy(cell) != solid(qRgb(0, 0, 0), gw))
                    return why.str() + "past libansilove's last row and not empty black";
                why.str("");
                continue;
            }
            if (ours.copy(cell) == ref.copy(cell)) {
                why.str("");
                continue;
            }
            const std::uint8_t g = glyphIndex(c.ch);
            if (!kFontDiffers.count(g))
                return why.str() + "differs from the reference";
            const Rgb f = resolveColor(c.fg), b = resolveColor(c.bg);
            const QRgb fg = qRgb(f.r, f.g, f.b), bg = qRgb(b.r, b.g, b.b);
            for (int y = cell.top(); y <= cell.bottom(); ++y)
                for (int x = cell.left(); x <= cell.right(); ++x)
                    if (const QRgb p = ref.pixel(x, y); p != fg && p != bg)
                        return why.str() + "the reference has a colour that is neither foreground nor background";
            if (ours.copy(cell) != vgaCell(g, fg, bg, gw))
                return why.str() + "ours is not the VGA glyph in the cell's colours";
            why.str("");
            if (fontCells)
                ++*fontCells;
        }
    }
    return "";
}

// Paints every cell holding glyph g with `fill(cell image, fg, bg)`, fg and bg
// being the colours the parsed document gives that cell.
template <typename F>
QImage corrupt(const AnsiDocument& doc, QImage img, int gw, std::uint8_t g, F fill) {
    for (int cy = 0; cy < doc.canvas.height(); ++cy)
        for (int cx = 0; cx < doc.canvas.width(); ++cx)
            if (const Cell& c = doc.canvas.at(cx, cy); glyphIndex(c.ch) == g) {
                const QRect r(cx * gw, cy * 16, gw, 16);
                const Rgb f = resolveColor(c.fg), b = resolveColor(c.bg);
                const QImage cell = fill(img.copy(r), qRgb(f.r, f.g, f.b), qRgb(b.r, b.g, b.b));
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < gw; ++x)
                        img.setPixel(r.left() + x, r.top() + y, cell.pixel(x, y));
            }
    return img;
}

// Whether some cell holding glyph g has differing foreground and background.
bool visible(const AnsiDocument& doc, std::uint8_t g) {
    for (int cy = 0; cy < doc.canvas.height(); ++cy)
        for (int cx = 0; cx < doc.canvas.width(); ++cx)
            if (const Cell& c = doc.canvas.at(cx, cy); glyphIndex(c.ch) == g && resolveColor(c.fg) != resolveColor(c.bg))
                return true;
    return false;
}

} // namespace

TEST_CASE("renders match libansilove's") {
    const auto refs = manifest();
    REQUIRE(refs.size() >= 20);
    int fontCells = 0;
    for (const Ref& r : refs) {
        INFO(r.png);
        const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / r.piece));
        CHECK(compare(doc, renderDocument(doc, {r.width, 1}), loadRef(r), r.width, &fontCells) == "");
    }
    // Most pieces carry the signature's 'f' and 'v', so some cells differ
    CHECK(fontCells > 0);
}

TEST_CASE("the golden tolerance checks our cell as well as the reference") {
    // Every piece's 'f' (0x66, a glyph the tolerance covers) corrupted three
    // ways; each must fail the comparison, at both glyph widths
    int tried = 0, shown = 0;
    for (const Ref& r : manifest()) {
        INFO(r.png);
        const AnsiDocument doc = parseAnsi(bwtest::readFile(bwtest::dataDir() / r.piece));
        const QImage ours = renderDocument(doc, {r.width, 1}), ref = loadRef(r);
        int fontCells = 0;
        REQUIRE(compare(doc, ours, ref, r.width, &fontCells) == "");
        const QImage magenta = corrupt(doc, ours, r.width, 0x66, [](QImage c, QRgb, QRgb) {
            c.fill(qRgb(255, 0, 255));
            return c;
        });
        if (magenta == ours)
            continue;  // no 'f' in this piece
        ++tried;
        const QImage blank = corrupt(doc, ours, r.width, 0x66, [](QImage c, QRgb, QRgb bg) {
            c.fill(bg);
            return c;
        });
        const QImage swapped = corrupt(doc, ours, r.width, 0x66, [](QImage c, QRgb fg, QRgb bg) {
            for (int y = 0; y < c.height(); ++y)
                for (int x = 0; x < c.width(); ++x)
                    c.setPixel(x, y, c.pixel(x, y) == fg ? bg : fg);
            return c;
        });
        if (visible(doc, 0x66)) {
            ++shown;
            CHECK(swapped != blank);
        }
        CHECK_THAT(compare(doc, magenta, ref, r.width), Catch::Matchers::ContainsSubstring("ours is not the VGA glyph"));
        CHECK_THAT(compare(doc, blank, ref, r.width), Catch::Matchers::ContainsSubstring("ours is not the VGA glyph"));
        CHECK_THAT(compare(doc, swapped, ref, r.width), Catch::Matchers::ContainsSubstring("ours is not the VGA glyph"));
    }
    CHECK(tried > 0);
    CHECK(shown > 0);
}
