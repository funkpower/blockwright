#pragma once
// Renders a document to an image with the bundled IBM VGA 8x16 font, the way
// libansilove does: one glyph per cell, 8 or 9 px wide, 16 px high.

#include "blockwright/ansi.h"

#include <QImage>

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

// The CP437 glyph a cell shows: its codepoint's byte, '?' if CP437 has none.
std::uint8_t glyphIndex(char32_t ch);

// The colour a cell colour renders as: a palette entry or its 24-bit value.
Rgb resolveColor(const Color& c);

// The whole canvas as an RGB32 image, width * glyphWidth * scale by
// height * 16 * scale. Colours are drawn as the cells hold them: with iCE on
// the parser has already folded blink into a bright background, and with iCE
// off a blinking cell shows its "on" phase (glyph visible, dark background),
// as libansilove draws it. In 9 px mode the 9th column repeats the 8th for
// the line-drawing glyphs 0xC0-0xDF and is background otherwise, as VGA does.
// The SAUCE aspect-ratio flag is not applied: pixels are always square.
// Transparent cells draw like any other (layers come later).
// Throws std::invalid_argument for a bad width or scale or an empty (0-column
// or 0-row) canvas, and
// std::length_error if the image would be too large to allocate.
QImage renderDocument(const AnsiDocument& doc, const RenderOptions& opts = {});
QImage renderCanvas(const Canvas& canvas, int glyphWidth, int scale = 1, const BitmapFont& font = vgaFont());

} // namespace bw
