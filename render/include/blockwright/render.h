#pragma once
// Renders a document to an image with the bundled IBM VGA 8x16 font, the way
// libansilove does: one glyph per cell, 8 or 9 px wide, 16 px high. Glyphs
// with no CP437 code draw from GNU Unifont (PLAN §16, Rendering).

#include "blockwright/ansi.h"
#include "blockwright/cp437.h"

#include <QImage>

#include <array>
#include <cstdint>
#include <span>

namespace bw {

// A 256-glyph CP437 bitmap font, 8 px wide: `height` bytes per glyph, one per
// row, bit 7 leftmost.
struct BitmapFont {
    int height = 16;
    std::span<const std::uint8_t> bits;  // 256 * height bytes

    bool pixel(std::uint8_t glyph, int x, int y) const {
        return bits[static_cast<size_t>(glyph) * height + y] & (0x80 >> x);
    }
};

// IBM VGA 8x16 from The Ultimate Oldschool PC Font Pack (CC BY-SA 4.0, see
// third_party/oldschool-pc-fonts/).
const BitmapFont& vgaFont();

struct RenderOptions {
    // Glyph cell width: 8 or 9, or 0 to follow the SAUCE letter-spacing flag
    // (9 px only when it says 9 px; 8 px when it says 8 px, is unset or there
    // is no SAUCE, as in libansilove).
    int fontWidth = 0;
    int scale = 1;  // integer, nearest neighbour, 1-8
};

inline constexpr int kMaxScale = 8;

// The cell width `opts` gives for `doc`.
int glyphWidth(const AnsiDocument& doc, const RenderOptions& opts = {});

// The CP437 byte of a code point, '?' if CP437 has none.
std::uint8_t glyphIndex(char32_t ch);
// The CP437 byte of a glyph: a cluster as '?' and the continuation as a
// space. What a cell draws is cellImage's.
std::uint8_t glyphIndex(Glyph g);

// The pixels of a cell the CP437 font doesn't draw: 16 rows, bit x of a row
// lit in column x, bit 8 the ninth column of 9 px mode.
using GlyphRows = std::array<std::uint16_t, 16>;

// What a cell draws: the font's glyph `index`, or `rows` when that isn't
// null. Rows live as long as the process, so their address names them.
struct CellImage {
    const GlyphRows* rows = nullptr;
    std::uint8_t index = ' ';
};

namespace detail {
CellImage slowCellImage(const Canvas& canvas, int x, int y, int fontHeight, bool lead);
}

// What the cell at x,y draws with a CP437 font `fontHeight` px high (PLAN
// §16, Rendering). A code point with a CP437 byte that isn't a lead draws
// that byte from the font. With a 16 px font everything else draws from
// Unifont, the VGA 8x16 giving a cluster's base when it has a byte: a lead
// (a cell whose right neighbour is a continuation) the glyph's left 8
// columns, its ninth repeating its eighth, and the continuation after it the
// right 8, its ninth background; a continuation after no lead draws nothing.
// Any other cell draws its glyph whole, a 16-column one squeezed to 8 by
// OR-ing each pair of columns. A cluster draws its NFC form when that is one
// code point the fonts have, else its base with each drawn mark (Mn or Me
// and not default-ignorable) OR-ed over it, a narrow mark centred on a wide
// base; the joiners and variation selectors draw nothing. A glyph neither
// font has draws as '?'. With any other font height a continuation draws
// nothing and everything else its glyphIndex. Thread-safe.
inline CellImage cellImage(const Canvas& canvas, int x, int y, int fontHeight) {
    const Glyph g = canvas.at(x, y).ch;
    const bool lead = x + 1 < canvas.width() && canvas.at(x + 1, y).ch.isContinuation();
    if (!lead)
        if (const std::optional<char32_t> cp = g.codePoint())
            if (const std::optional<std::uint8_t> b = unicodeToCp437(*cp))
                return {nullptr, *b};
    return detail::slowCellImage(canvas, x, y, fontHeight, lead);
}

// The colour a cell colour renders as: a palette entry or its 24-bit value.
Rgb resolveColor(const Color& c);

// The whole canvas as an RGB32 image, width * glyphWidth * scale by
// height * 16 * scale. Colours are drawn as the cells hold them: with iCE on
// the parser has already folded blink into a bright background, and with iCE
// off a blinking cell shows its "on" phase (glyph visible, dark background),
// as libansilove draws it. In 9 px mode the 9th column repeats the 8th for
// the line-drawing glyphs 0xC0-0xDF and is background otherwise, as VGA does.
// Each cell draws its cellImage in its own colours.
// The SAUCE aspect-ratio flag is not applied: pixels are always square.
// Transparent cells draw like any other (layers come later).
// Throws std::invalid_argument for a bad width or scale or an empty (0-column
// or 0-row) canvas, and
// std::length_error if the image would be too large to allocate.
QImage renderDocument(const AnsiDocument& doc, const RenderOptions& opts = {});
QImage renderCanvas(const Canvas& canvas, int glyphWidth, int scale = 1, const BitmapFont& font = vgaFont());

} // namespace bw
