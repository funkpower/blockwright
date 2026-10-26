#pragma once
// The IBM PC bitmap fonts bundled from VileR's Ultimate Oldschool PC Font
// Pack (CC BY-SA 4.0, see third_party/oldschool-pc-fonts/): 256 CP437
// glyphs each, 8 px wide, one byte a row, bit 7 leftmost. The renderer draws
// cells with the VGA font; the `block` and `vga` text faces (face.h) read
// both.

#include <cstdint>
#include <span>

namespace bw {

// IBM PC BIOS 8x8: 8 bytes a glyph.
std::span<const std::uint8_t, 256 * 8> ibmBios8x8();
// IBM VGA 8x16: 16 bytes a glyph.
std::span<const std::uint8_t, 256 * 16> ibmVga8x16();

} // namespace bw
