#pragma once
// Micron, the markup of NomadNet pages (.mu): a canvas as UTF-8 Micron text
// that NomadNet's parser (nomadnet/ui/textui/MicronParser.py, 1.5) draws
// cell for cell as the editor shows it (PLAN §8, Micron).

#include "blockwright/ansi.h"
#include "blockwright/palette.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bw::micron {

struct Options {
    // Start every line with `c, so NomadNet centres the piece; else it is
    // left-aligned.
    bool centre = false;
    // Write the page's cache header, "#!c=N", as the first line: how many
    // seconds a peer may keep the page (0: never cache it). NomadNet reads
    // it only from the first line of a page.
    std::optional<unsigned> cacheSeconds;
};

// A colour as Micron writes it after `F or `B: three hex digits when each
// channel is a digit doubled (aa5500 as "a50"), as every colour of kPalette
// is, else "T" and six digits ("T1f2e3d"), the parser's truecolour form.
std::string colourCode(Rgb c);

// The twelve-bit colour with hex digits `r`, `g` and `b` (each clamped to
// 0-15), each a channel's nibble doubled: what `Frgb writes (PLAN §17).
Rgb twelveBit(int r, int g, int b);
// Whether `c` is a twelve-bit colour, which colourCode writes in three digits.
bool isTwelveBit(Rgb c);

// Whether NomadNet drops code point `cp` from a page before it draws it:
// what the browser's strip_modifiers and the parser's STRIP_CONTROL_RE take
// out (marks, format characters, zero-width joiners, variation selectors,
// emoji skin tones, C0 and C1 controls, bidi controls, specials), and the
// one glyph it draws as a space (U+261D).
bool droppedByNomadNet(char32_t cp);

// Row `y` of `canvas` as one Micron line, without the line ending: the
// cells as the editor shows them with iCE colours `ice` (iceCell; a cell
// with an RGB background keeps it), indexed colours from `palette`. Each
// line opens with its colours (after `c when centred), so no line can start
// with a character Micron gives meaning to, writes a colour or formatting
// code only where it changes and the cell shows it (no foreground for a
// blank that isn't underlined, no background for a full block █ unless
// italic shears it off its background), escapes ` and \ with \, writes a !
// after a # escaped too, so no text reads as a page header, and ends with `` (every
// formatting reset), or when centred with the formatting codes still on and
// `f`b, since `` would reset the alignment and NomadNet aligns a line by the
// state at its end. Trailing blanks are kept: they show their background,
// which the viewer's own may not match, and centring counts them. Glyphs are
// written as the IRC encoder writes them (irc::mircRow: CP437 controls as
// their glyphs, a wide pair whole, an unwritable code point as '?'); a glyph
// holding a code point NomadNet drops (droppedByNomadNet) is written '?' too,
// a wide one '?' and a space, so the columns stay.
std::string encodeLine(const Canvas& canvas, int y, const std::array<Rgb, 16>& palette, bool ice,
                       const Options& opts = {});

// The canvas as a .mu page: the header line if asked for, then one line per
// row (every row, blank ones included), each followed by '\n'.
std::string encodeCanvas(const Canvas& canvas, const std::array<Rgb, 16>& palette, bool ice,
                         const Options& opts = {});

// encodeCanvas of a document: its own palette (an XBIN's) or kPalette, and
// its iCE setting.
std::string encode(const AnsiDocument& doc, const Options& opts = {});

// The cells, in cell order, whose colour encodeLine writes in the
// truecolour form (`FTrrggbb / `BTrrggbb) since it isn't twelve-bit: a
// colour the cell shows (a foreground on a glyph that draws one, a
// background not covered by a full block), read as encodeLine reads it
// with `palette` and `ice`. The colour is written as it is, not rounded.
std::vector<std::pair<int, int>> truecolourCells(const Canvas& canvas, const std::array<Rgb, 16>& palette, bool ice);

// The cells Micron writes as something else, in cell order: irc::unwritable's,
// and each glyph written '?' for a code point NomadNet drops (Loss::NoCode,
// `lead` set on a wide one).
std::vector<Unwritable> unwritable(const Canvas& canvas);

} // namespace bw::micron
