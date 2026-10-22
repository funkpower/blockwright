#pragma once
// CP437 <-> Unicode, as the glyphs are drawn on screen: 0x01-0x1F and 0x7F
// are the DOS pictographs (☺ ♥ ► ⌂ …), not control characters.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bw {

// Byte -> codepoint for all 256 codes. 0x00 maps to U+0000.
const std::array<char32_t, 256>& cp437Table();

inline char32_t cp437ToUnicode(std::uint8_t byte) { return cp437Table()[byte]; }

// Codepoint -> byte, or nothing when CP437 has no such glyph (★, …, “ ”).
// Never substitutes '?': callers decide what to do and report it.
std::optional<std::uint8_t> unicodeToCp437(char32_t cp);

void appendUtf8(std::string& out, char32_t cp);
// CP437 bytes (a SAUCE title, say) as UTF-8.
std::string cp437ToUtf8(std::string_view bytes);

} // namespace bw
