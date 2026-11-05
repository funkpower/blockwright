#include "blockwright/render.h"

#include "blockwright/cp437.h"
#include "blockwright/pc_fonts.h"
#include "unifont.h"

#include <QChar>
#include <QString>

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace bw {

namespace {

// Images past this many pixels are refused rather than half-allocated
// (QImage gives up at 2 GiB; a 400x5000 canvas at 9 px is 288 Mpx at 1x).
constexpr qint64 kMaxPixels = qint64(1) << 28;

QRgb toQRgb(Rgb c) { return qRgb(c.r, c.g, c.b); }

// A glyph's pixels before they are fitted to a cell: 16 rows, bit x column
// x, 8 or 16 columns.
struct Bitmap {
    int width = 8;
    std::array<std::uint16_t, 16> rows{};
};

// Which part of a glyph a cell draws: all of it, or the half of a pair.
enum class Part : std::uint8_t { Whole, Left, Right };

std::optional<Bitmap> codePointBitmap(char32_t cp) {
    Bitmap b;
    if (const std::optional<std::uint8_t> byte = unicodeToCp437(cp)) {
        const BitmapFont& vga = vgaFont();
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x)
                if (vga.pixel(*byte, x, y))
                    b.rows[y] |= std::uint16_t(1u << x);
        return b;
    }
    const std::optional<UnifontGlyph> u = unifontGlyph(cp);
    if (!u)
        return std::nullopt;
    b.width = u->wide ? 16 : 8;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < b.width; ++x)
            if (u->pixel(x, y))
                b.rows[y] |= std::uint16_t(1u << x);
    return b;
}

// 16 columns to 8, each pair OR-ed into one.
std::uint16_t squeeze(std::uint16_t row) {
    std::uint16_t out = 0;
    for (int i = 0; i < 8; ++i)
        if ((row >> (2 * i)) & 3)
            out |= std::uint16_t(1u << i);
    return out;
}

// A mark the renderer draws over its base: a nonspacing or enclosing one
// that isn't default-ignorable (CGJ, the Khmer inherent vowels, the
// Mongolian and Unicode variation selectors draw nothing).
bool drawnMark(char32_t cp) {
    const QChar::Category cat = QChar::category(cp);
    if (cat != QChar::Mark_NonSpacing && cat != QChar::Mark_Enclosing)
        return false;
    return !(cp == 0x034F || (cp >= 0x17B4 && cp <= 0x17B5) || (cp >= 0x180B && cp <= 0x180D) || cp == 0x180F ||
             (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF));
}

// A glyph's bitmap (cellImage): '?' when the fonts lack it.
Bitmap glyphBitmap(Glyph g) {
    const GlyphCodePoints cps = codePoints(g);
    std::optional<Bitmap> b;
    if (cps.size == 1) {
        b = codePointBitmap(cps[0]);
    } else if (cps.size > 1) {
        const QList<uint> nfc =
            QString::fromUcs4(cps.begin(), cps.size).normalized(QString::NormalizationForm_C).toUcs4();
        if (nfc.size() == 1)
            b = codePointBitmap(nfc[0]);
        if (!b && !nfc.isEmpty() && (b = codePointBitmap(nfc[0]))) {
            for (qsizetype i = 1; i < nfc.size(); ++i) {
                if (!drawnMark(nfc[i]))
                    continue;
                std::optional<Bitmap> mark = codePointBitmap(nfc[i]);
                if (!mark)
                    continue;
                if (mark->width > b->width)
                    for (auto& row : mark->rows)
                        row = squeeze(row);
                const int shift = mark->width < b->width ? 4 : 0;
                for (int y = 0; y < 16; ++y)
                    b->rows[y] |= std::uint16_t(mark->rows[y] << shift);
            }
        }
    }
    return b ? *b : *codePointBitmap(U'?');
}

GlyphRows partRows(const Bitmap& b, Part part) {
    GlyphRows rows{};
    for (int y = 0; y < 16; ++y) {
        const std::uint16_t r = b.rows[y];
        switch (part) {
        case Part::Whole:
            rows[y] = b.width == 16 ? squeeze(r) : r;
            break;
        case Part::Left:
            rows[y] = std::uint16_t((r & 0xff) | ((r >> 7) & 1) << 8);
            break;
        case Part::Right:
            rows[y] = b.width == 16 ? std::uint16_t(r >> 8) : 0;
            break;
        }
    }
    return rows;
}

// The rows of each (glyph, part) drawn so far, made once and kept for the
// process, so a cell's rows are a lookup and their address a cache key.
CellImage rowsFor(Glyph g, Part part) {
    static std::mutex mutex;
    static std::unordered_map<std::uint64_t, const GlyphRows*> index;
    static std::deque<GlyphRows> store;
    const std::uint64_t key = std::uint64_t(g.bits()) << 2 | std::uint64_t(part);
    {
        const std::lock_guard lock(mutex);
        if (const auto it = index.find(key); it != index.end())
            return {it->second};
    }
    const GlyphRows rows = partRows(glyphBitmap(g), part);
    const std::lock_guard lock(mutex);
    if (const auto it = index.find(key); it != index.end())
        return {it->second};
    const GlyphRows* p = &store.emplace_back(rows);
    index.emplace(key, p);
    return {p};
}

} // namespace

CellImage detail::slowCellImage(const Canvas& canvas, int x, int y, int fontHeight, bool lead) {
    const Glyph g = canvas.at(x, y).ch;
    if (g.isContinuation()) {
        if (x == 0 || fontHeight != 16)
            return {nullptr, ' '};
        const Glyph left = canvas.at(x - 1, y).ch;
        if (left.isContinuation())
            return {nullptr, ' '};
        return rowsFor(left, Part::Right);
    }
    if (fontHeight != 16)
        return {nullptr, glyphIndex(g)};
    return rowsFor(g, lead ? Part::Left : Part::Whole);
}

const BitmapFont& vgaFont() {
    static const BitmapFont font{16, ibmVga8x16()};
    return font;
}

int glyphWidth(const AnsiDocument& doc, const RenderOptions& opts) {
    if (opts.fontWidth)
        return opts.fontWidth;
    return doc.sauce && doc.sauce->letterSpacing() == 2 ? 9 : 8;
}

std::uint8_t glyphIndex(char32_t ch) { return unicodeToCp437(ch).value_or('?'); }

std::uint8_t glyphIndex(Glyph g) {
    if (g.isContinuation())
        return ' ';
    const std::optional<char32_t> cp = g.codePoint();
    return cp ? glyphIndex(*cp) : std::uint8_t('?');
}

Rgb resolveColor(const Color& c) { return c.isIndexed() ? kPalette[c.index & 15] : c.rgb; }

GlyphRows formattedRows(const CellImage& ci, std::uint8_t attrs, int gw, const BitmapFont& font) {
    const int gh = std::min(font.height, 16);
    GlyphRows rows{};
    if (ci.rows) {
        rows = *ci.rows;
    } else {
        const std::uint8_t g = ci.index;
        const bool lineDrawing = g >= 0xC0 && g <= 0xDF;
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                if (x < 8 ? font.pixel(g, x, y) : lineDrawing && font.pixel(g, 7, y))
                    rows[std::size_t(y)] |= std::uint16_t(1u << x);
    }
    return formatRows(rows, attrs, gw, gh);
}

QImage renderCanvas(const Canvas& canvas, int gw, int scale, const BitmapFont& font) {
    if (gw != 8 && gw != 9)
        throw std::invalid_argument("font width must be 8 or 9");
    if (scale < 1 || scale > kMaxScale)
        throw std::invalid_argument("scale must be 1-" + std::to_string(kMaxScale));
    if (canvas.width() == 0 || canvas.height() == 0)
        throw std::invalid_argument("empty canvas");
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
            CellImage ci = cellImage(canvas, cx, cy, gh);
            const std::uint8_t g = ci.index;
            const QRgb fg = toQRgb(resolveColor(cell.fg)), bg = toQRgb(resolveColor(cell.bg));
            GlyphRows formatted;
            if (cell.attrs & attr::Formatting) {
                formatted = formattedRows(ci, cell.attrs, gw, font);
                ci.rows = &formatted;
            }
            if (ci.rows) {
                for (int y = 0; y < gh; ++y) {
                    const std::uint16_t row = (*ci.rows)[std::size_t(y)];
                    for (int s = 0; s < scale; ++s) {
                        auto* line = reinterpret_cast<QRgb*>(img.scanLine((cy * gh + y) * scale + s));
                        QRgb* px = line + qint64(cx) * gw * scale;
                        for (int x = 0; x < gw; ++x) {
                            const bool on = (row >> x) & 1;
                            for (int t = 0; t < scale; ++t)
                                *px++ = on ? fg : bg;
                        }
                    }
                }
                continue;
            }
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
