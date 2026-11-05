#pragma once
// GNU Unifont, the renderer's font for glyphs with no CP437 code (PLAN §16,
// Rendering): third_party/unifont/unifont.bin.z (or the one built from the
// system's .hex files), compiled in and decoded on first use.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace bw {

// A Unifont glyph: 16 rows, bit x of a row the pixel in column x, 8 columns
// wide or 16.
struct UnifontGlyph {
    bool wide = false;
    const std::uint8_t* bits = nullptr;  // 16 bytes, or 32 when wide, as in the .hex file

    bool pixel(int x, int y) const {
        return wide ? (bits[y * 2 + x / 8] >> (7 - x % 8)) & 1 : (bits[y] >> (7 - x)) & 1;
    }
};

// The glyph Unifont has for `cp`, or nothing. Thread-safe; the first call
// decodes the table. Throws std::runtime_error if the compiled-in table is
// missing or damaged.
std::optional<UnifontGlyph> unifontGlyph(char32_t cp);

// How many glyphs the table holds.
std::size_t unifontGlyphCount();

} // namespace bw
