#pragma once
// A document's target (#350, PLAN §6): what it is drawn for. ANSI is an
// .ans: 16 colours, as every document was before. IRC is IRC text, which
// carries mIRC's 99 colours as well. The target changes what the editor
// offers, not what any cell holds, so a reader that drops it shows the same
// art.

#include "blockwright/cell.h"

#include <optional>
#include <string_view>

namespace bw {

enum class Target : std::uint8_t { Ansi, Irc };

enum class TextEncoding;  // ansi.h

// What typing and the F-key sets offer (#350 slice 2, PLAN §16, Cursor and
// typing). CP437: the glyphs a CP437 .ans holds, as every document had
// before. Unicode: any narrow glyph a UTF-8 .ans and IRC text hold.
enum class TypingPolicy : std::uint8_t { Cp437, Unicode };

// Unicode under the IRC target or a UTF-8 .ans encoding; CP437 only for an
// ANSI document saved as a CP437 .ans.
TypingPolicy typingPolicy(Target target, TextEncoding encoding);

// "ansi" or "irc": the project manifest's value and the CLI's --target.
std::string_view targetName(Target t);
// The target a name means, in any case; nothing for any other string.
std::optional<Target> parseTarget(std::string_view name);

// The palette dock's swatches: under ANSI the 16 colours, under IRC mIRC's
// 99 as well, 16-98 after the 16.
constexpr int swatchCount(Target t) { return t == Target::Irc ? 99 : 16; }

// Swatch `i`'s colour: 0-15 the palette's own (indexed), 16-98 mIRC colour
// `i` as its exact RGB, which irc::mircColor maps back to `i` (or, for the
// six that share an RGB with one of 0-15, to that one: 52 is 4, 54 8, 58 11,
// 62 13, 88 1 and 98 0, the shorter code). Out of range is clamped.
Color swatchColor(int i);

// The swatch showing `c` under `t`: an indexed colour its index; an RGB one
// under IRC the first of 16-98 with exactly its RGB, else (and always under
// ANSI) the palette entry nearest it, as an .ans saves it.
int swatchOf(const Color& c, Target t);

} // namespace bw
