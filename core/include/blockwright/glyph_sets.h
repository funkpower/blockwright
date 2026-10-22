#pragma once
// The F-key glyph sets: F1-F10 type the glyphs of the current set. The
// sets are Moebius's sixteen, which it took from PabloDraw, in its order;
// Moebius starts on the block and shade set, here kDefaultGlyphSet.

#include <array>
#include <cstddef>

namespace bw {

inline constexpr int kGlyphSetCount = 16;
inline constexpr int kGlyphSetKeys = 10;  // F1-F10
inline constexpr int kDefaultGlyphSet = 5;

using GlyphSet = std::array<char32_t, kGlyphSetKeys>;

// Set `set` (0-based, kept in range by wrapping) as codepoints.
const GlyphSet& glyphSet(int set);

// The glyph F<key + 1> types in `set`.
inline char32_t glyphSetGlyph(int set, int key) { return glyphSet(set)[static_cast<std::size_t>(key)]; }

} // namespace bw
