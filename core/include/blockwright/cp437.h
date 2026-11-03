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

// The UTF-8 sequence at `i` in `s`, advancing `i` past it. Only well-formed
// sequences count (RFC 3629: no overlong forms, no surrogates, nothing past
// U+10FFFF, none cut short by the end of `s`); for any other byte, nothing,
// and `i` moves past that one byte.
std::optional<char32_t> nextUtf8(std::string_view s, std::size_t& i);
// CP437 bytes (a SAUCE title, say) as UTF-8.
std::string cp437ToUtf8(std::string_view bytes);

} // namespace bw
