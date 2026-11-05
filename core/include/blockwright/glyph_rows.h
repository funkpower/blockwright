#pragma once
// A cell's pixels as rows of bits, and what bold, italic and underline do to
// them (#350). The renderer draws formatted cells with formatRows, and the
// lint's wasted-format compares a cell's rows with and without it, so the two
// agree on what formatting draws.

#include <array>
#include <cstdint>

namespace bw {

// 16 rows, bit x of a row lit in column x, bit 8 the ninth column of 9 px
// mode.
using GlyphRows = std::array<std::uint16_t, 16>;

// CP437 glyph `g` in the IBM VGA 8x16, `glyphWidth` (8 or 9) px wide: the
// ninth column repeats the eighth for the line-drawing glyphs 0xC0-0xDF and
// is background otherwise, as VGA draws it.
GlyphRows vgaRows(std::uint8_t g, int glyphWidth);

// `rows`, `glyphHeight` (16 at most) high and `glyphWidth` wide, with
// `attrs`' formatting: bold ORs each row with itself one pixel to the right,
// italic then shears the glyph one pixel right for every four rows above the
// bottom one, and underline lights the bottom row; a pixel pushed past the
// cell's right edge is dropped. Other attribute bits draw nothing.
GlyphRows formatRows(GlyphRows rows, std::uint8_t attrs, int glyphWidth, int glyphHeight);

} // namespace bw
