// Drawing with a document's own palette and font (#497, PLAN §8, Palettes):
// render, the canvas renderer, and a golden PNG of an XBIN with a palette
// and a font of its own.

#include "util.h"

#include "blockwright/canvas_renderer.h"
#include "blockwright/render.h"
#include "blockwright/xbin.h"

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QImage>

#include <map>

using namespace bw;

namespace {

std::string xbinFixture() { return bwtest::readFile(bwtest::dataDir() / "xbin" / "tux-palette-font.xb"); }

QRgb q(Rgb c) { return qRgb(c.r, c.g, c.b); }

// `img`, drawn in kPalette, with each standard colour swapped for the entry of
// `palette` at its index: what the same cells drawn in `palette` look like.
QImage recoloured(QImage img, const Palette& palette) {
    std::map<QRgb, QRgb> to;
    for (std::size_t i = 0; i < 16; ++i)
        to[q(kPalette[i]) | 0xff000000u] = q(palette[i]) | 0xff000000u;
    for (int y = 0; y < img.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x)
            line[x] = to.at(line[x] | 0xff000000u);
    }
    return img;
}

} // namespace

TEST_CASE("an XBIN with its own palette renders in it", "[render][palette]") {
    AnsiDocument d = parseXbin(xbinFixture());
    REQUIRE(customPalette(d));
    d.font.reset();  // the palette alone
    AnsiDocument standard = d;
    standard.palette.reset();
    const QImage own = renderDocument(d), plain = renderDocument(standard);
    CHECK(own != plain);
    CHECK(own == recoloured(plain, *d.palette));
    // and a palette given to renderCanvas is the same thing
    CHECK(renderCanvas(d.canvas, 8, 1, vgaFont(), *d.palette) == own);
    CHECK(renderCanvas(d.canvas, 8) == plain);
    CHECK(resolveColor(Color::indexed(3), *d.palette) == (*d.palette)[3]);
    CHECK(resolveColor(Color::fromRgb({1, 2, 3}), *d.palette) == Rgb{1, 2, 3});
}

TEST_CASE("an XBIN with its own font renders in it, its cells as high as the font", "[render][font]") {
    const AnsiDocument d = parseXbin(xbinFixture());
    REQUIRE(d.font);
    REQUIRE(d.font->height == 8);
    const QImage own = renderDocument(d);
    CHECK(own.size() == QSize(d.canvas.width() * 8, d.canvas.height() * 8));
    const BitmapFont f = fileFont(*d.font);
    CHECK(f.height == 8);
    CHECK(f.bits.size() == 256 * 8);
    CHECK(documentFont(d).bits.data() == f.bits.data());
    CHECK(own == renderCanvas(d.canvas, 8, 1, f, *d.palette));
    // each pixel is the font's: cell (x, y)'s row r is glyph g's byte r
    const Palette& pal = *d.palette;
    for (int cy = 0; cy < d.canvas.height(); ++cy)
        for (int cx = 0; cx < d.canvas.width(); ++cx) {
            const Cell& c = d.canvas.at(cx, cy);
            const std::uint8_t g = glyphIndex(c.ch);
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const bool on = std::uint8_t(d.font->bits[std::size_t(g) * 8 + y]) & (0x80 >> x);
                    const QRgb want = q(resolveColor(on ? c.fg : c.bg, pal)) | 0xff000000u;
                    if ((own.pixel(cx * 8 + x, cy * 8 + y) | 0xff000000u) != want) {
                        FAIL_CHECK("cell " << cx << "," << cy << " pixel " << x << "," << y);
                        return;
                    }
                }
        }
    // without it, the VGA font's 16 px
    AnsiDocument vga = d;
    vga.font.reset();
    CHECK(renderDocument(vga).height() == d.canvas.height() * 16);
    CHECK(documentFont(vga).bits.data() == vgaFont().bits.data());
}

TEST_CASE("a font over 16 px renders, its formatting left out", "[render][font]") {
    FileFont tall{32, std::string(256 * 32, '\xff')};
    Canvas cv(2, 1);
    cv.at(0, 0).attrs = attr::Underline | attr::Bold;
    cv.at(0, 0).ch = U'A';
    cv.at(1, 0).ch = U'B';
    const QImage img = renderCanvas(cv, 9, 1, fileFont(tall));
    CHECK(img.size() == QSize(18, 32));
    // every glyph pixel on: the foreground, grey, everywhere but column 9 of a non-line glyph
    CHECK(img.pixel(0, 31) == (q(kPalette[7]) | 0xff000000u));
    CHECK(img.pixel(8, 0) == (q(kPalette[0]) | 0xff000000u));
}

TEST_CASE("the golden render of an XBIN with its own palette and font", "[render][palette][font]") {
    const AnsiDocument d = parseXbin(xbinFixture());
    const QImage golden(QString::fromStdString((bwtest::dataDir() / "xbin" / "tux-palette-font.png").string()));
    REQUIRE_FALSE(golden.isNull());
    CHECK(renderDocument(d) == golden.convertToFormat(QImage::Format_RGB32));
}

TEST_CASE("the canvas renderer draws in the palette it is given and redraws on a change", "[render][palette]") {
    const AnsiDocument d = parseXbin(xbinFixture());
    AtlasRenderer r;
    CHECK(r.palette() == kPalette);
    r.setCanvas(&d.canvas, 8);
    r.flush();
    CHECK(r.cache() == renderCanvas(d.canvas, 8));
    r.setPalette(*d.palette);
    CHECK(r.palette() == *d.palette);
    CHECK(r.dirtyCells() == QRegion(0, 0, d.canvas.width(), d.canvas.height()));
    r.flush();
    CHECK(r.cache() == renderCanvas(d.canvas, 8, 1, vgaFont(), *d.palette));
    // the palette holds across a new canvas
    r.setCanvas(&d.canvas, 9);
    r.flush();
    CHECK(r.cache() == renderCanvas(d.canvas, 9, 1, vgaFont(), *d.palette));
    r.setPalette(kPalette);
    r.flush();
    CHECK(r.cache() == renderCanvas(d.canvas, 9));
    // the same palette again changes nothing
    r.setPalette(kPalette);
    CHECK(r.dirtyCells().isEmpty());
}

TEST_CASE("the canvas renderer draws in the font it is given, its cells that font's height", "[render][font]") {
    const AnsiDocument d = parseXbin(xbinFixture());
    AtlasRenderer r;
    CHECK(r.font().bits.data() == vgaFont().bits.data());
    // before a canvas: kept for it
    const BitmapFont f = fileFont(*d.font);
    r.setFont(f);
    r.setCanvas(&d.canvas, 8);
    CHECK(r.cellSize() == QSize(8, 8));
    CHECK(r.imageSize() == QSize(d.canvas.width() * 8, d.canvas.height() * 8));
    r.flush();
    CHECK(r.cache() == renderCanvas(d.canvas, 8, 1, f));
    // with a canvas: the cache is remade at the new height, every cell dirty
    r.setPalette(*d.palette);
    r.setFont(vgaFont());
    CHECK(r.cellSize() == QSize(8, 16));
    CHECK(r.dirtyCells() == QRegion(0, 0, d.canvas.width(), d.canvas.height()));
    r.flush();
    CHECK(r.cache() == renderCanvas(d.canvas, 8, 1, vgaFont(), *d.palette));
    r.setFont(f);
    r.flush();
    CHECK(r.cache() == renderDocument(d));
    // a font too tall for the canvas is refused and the old one kept
    Canvas big(400, 5000);
    FileFont tall{32, std::string(256 * 32, '\0')};
    r.setFont(vgaFont());
    r.setCanvas(&big, 8);
    CHECK_THROWS_AS(r.setFont(fileFont(tall)), std::length_error);
    CHECK(r.font().bits.data() == vgaFont().bits.data());
    CHECK(r.cellSize() == QSize(8, 16));
}
