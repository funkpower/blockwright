#pragma once
// CP437 <-> Unicode, as the glyphs are drawn on screen: 0x01-0x1F and 0x7F
// are the DOS pictographs (☺ ♥ ► ⌂ …), not control characters.

#include "blockwright/glyph.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
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

// A cluster of a base and one mark that NFC composes to one CP437 glyph
// (e U+0301 → é, 0x82).
struct Cp437Composition {
    char32_t base, mark;
    std::uint8_t byte;
};
// Every such pair, sorted by base then mark: a fixed table generated from
// Unicode's normalization data (core/src/cp437_compose.inc says how), so
// what the writers write doesn't depend on the build's Qt.
std::span<const Cp437Composition> cp437Compositions();
// The CP437 byte a cluster's NFC form is, when it is one CP437 glyph;
// nothing for any other glyph (a single code point, the continuation, a
// cluster of three or more).
std::optional<std::uint8_t> cp437Composed(Glyph g);

// Whether `cp`, printed on its own, acts or shows nothing visible: a C0 or
// C1 control or DEL (general category Cc), a format character (Cf: the bidi
// marks, embeddings, overrides and isolates U+200E-F, U+202A-E, U+2066-9,
// the zero-width characters, the byte order mark, the tags ...), the line
// and paragraph separators U+2028 and U+2029, a non-spacing or enclosing
// mark (Mn, Me: a combining accent such as U+0301, which lands on whatever
// precedes it, the variation selectors U+FE00-FE0F and U+E0100-E01EF, the
// combining grapheme joiner U+034F), a Hangul filler (U+115F, U+1160,
// U+3164, U+FFA0), a noncharacter (U+FDD0-FDEF, U+xFFFE and U+xFFFF), a
// code point Unicode leaves unassigned (Cn, as Qt knows it), a surrogate or
// anything past U+10FFFF. Spaces (Zs) and private-use code points are
// printed as they are.
bool unprintableCodePoint(char32_t cp);

// `cp` as text to quote in a message: its UTF-8, or "U+XXXX" (at least four
// hex digits) when it is unprintableCodePoint, so a control, bidi override
// or invisible character taken from a file is never printed raw.
std::string quotedGlyph(char32_t cp);
// A cell's glyph quoted the same way: a cluster as its code points run
// together, set off by a space either side of a U+XXXX ("e U+0301");
// empty for the continuation.
std::string quotedGlyph(Glyph g);
// A cell's glyph as its code points' "U+XXXX", space separated; empty for the
// continuation.
std::string codePointNames(Glyph g);

// The UTF-8 sequence at `i` in `s`, advancing `i` past it. Only well-formed
// sequences count (RFC 3629: no overlong forms, no surrogates, nothing past
// U+10FFFF, none cut short by the end of `s`); for any other byte, nothing,
// and `i` moves past that one byte.
std::optional<char32_t> nextUtf8(std::string_view s, std::size_t& i);
// CP437 bytes (a SAUCE title, say) as UTF-8.
std::string cp437ToUtf8(std::string_view bytes);

} // namespace bw
