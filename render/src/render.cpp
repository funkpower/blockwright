#include "blockwright/render.h"

#include "blockwright/cp437.h"

#include <array>
#include <stdexcept>
#include <string>

namespace bw {

namespace {

constexpr std::array<std::uint8_t, 256 * 16> kVgaBits = {
#include "font_ibm_vga_8x16.inc"
};

// Images past this many pixels are refused rather than half-allocated
// (QImage gives up at 2 GiB; a 400x5000 canvas at 9 px is 288 Mpx at 1x).
constexpr qint64 kMaxPixels = qint64(1) << 28;

QRgb toQRgb(Rgb c) { return qRgb(c.r, c.g, c.b); }

} // namespace

const BitmapFont& vgaFont() {
    static const BitmapFont font{16, kVgaBits};
    return font;
}

int glyphWidth(const AnsiDocument& doc, const RenderOptions& opts) {
    if (opts.fontWidth)
        return opts.fontWidth;
    return doc.sauce && doc.sauce->letterSpacing() == 2 ? 9 : 8;
}

std::uint8_t glyphIndex(char32_t ch) { return unicodeToCp437(ch).value_or('?'); }

Rgb resolveColor(const Color& c) { return c.isIndexed() ? kPalette[c.index & 15] : c.rgb; }

QImage renderCanvas(const Canvas& canvas, int gw, int scale, const BitmapFont& font) {
    if (gw != 8 && gw != 9)
        throw std::invalid_argument("font width must be 8 or 9");
    if (scale < 1 || scale > kMaxScale)
        throw std::invalid_argument("scale must be 1-" + std::to_string(kMaxScale));
    const int gh = font.height;
    const qint64 w = qint64(canvas.width()) * gw * scale, h = qint64(canvas.height()) * gh * scale;
    if (w <= 0 || h <= 0 || w * h > kMaxPixels)
        throw std::length_error("image too large");
    QImage img(int(w), int(h), QImage::Format_RGB32);
    if (img.isNull())
        throw std::length_error("image too large");

    for (int cy = 0; cy < canvas.height(); ++cy) {
        for (int cx = 0; cx < canvas.width(); ++cx) {
            const Cell& cell = canvas.at(cx, cy);
            const std::uint8_t g = glyphIndex(cell.ch);
            const QRgb fg = toQRgb(resolveColor(cell.fg)), bg = toQRgb(resolveColor(cell.bg));
            const bool lineDrawing = g >= 0xC0 && g <= 0xDF;
            for (int y = 0; y < gh; ++y) {
                for (int s = 0; s < scale; ++s) {
                    auto* line = reinterpret_cast<QRgb*>(img.scanLine((cy * gh + y) * scale + s));
                    QRgb* px = line + qint64(cx) * gw * scale;
                    for (int x = 0; x < gw; ++x) {
                        const bool on = x < 8 ? font.pixel(g, x, y) : lineDrawing && font.pixel(g, 7, y);
                        for (int t = 0; t < scale; ++t)
                            *px++ = on ? fg : bg;
                    }
                }
            }
        }
    }
    return img;
}

QImage renderDocument(const AnsiDocument& doc, const RenderOptions& opts) {
    return renderCanvas(doc.canvas, glyphWidth(doc, opts), opts.scale);
}

} // namespace bw
