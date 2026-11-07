#pragma once
// The F-key glyph sets: F1-F10 type the glyphs of the current set. The
// first sixteen are Moebius's, which it took from PabloDraw, in its order;
// Moebius starts on the block and shade set, here kDefaultGlyphSet. Five
// Unicode sets follow them (17-20 #350 slice 2, 21 the sparkles #399),
// offered only under the Unicode typing policy (glyphSetCount). A new set
// goes at the end, so the stored set index and the glyphSet<N> action names
// that key kept shortcuts keep their meaning.

#include "blockwright/target.h"

#include <array>
#include <cstddef>

namespace bw {

inline constexpr int kCp437GlyphSetCount = 16;  // Moebius's
inline constexpr int kGlyphSetCount = 21;       // with the Unicode sets
inline constexpr int kGlyphSetKeys = 10;        // F1-F10
inline constexpr int kDefaultGlyphSet = 5;

using GlyphSet = std::array<char32_t, kGlyphSetKeys>;

// How many sets `policy` offers, the first that many: Moebius's 16 under
// CP437, all 21 under Unicode.
constexpr int glyphSetCount(TypingPolicy policy) {
    return policy == TypingPolicy::Unicode ? kGlyphSetCount : kCp437GlyphSetCount;
}

// Set `set` (0-based, kept in range by wrapping) as codepoints.
const GlyphSet& glyphSet(int set);

// The glyph F<key + 1> types in `set`.
inline char32_t glyphSetGlyph(int set, int key) { return glyphSet(set)[static_cast<std::size_t>(key)]; }

} // namespace bw
