#pragma once
// Binary text: BIN (.bin), bare character/attribute pairs whose width comes
// from SAUCE, and XBIN (.xb), the same pairs after a header that gives the
// size and can carry a palette and a font, optionally run-length compressed
// (the XBin specification; Moebius's libtextmode reads it the same way). Both
// read into and write from an AnsiDocument, as .ans does.

#include "blockwright/ansi.h"
#include "blockwright/sauce.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace bw {

enum class ArtFormat { Ansi, Xbin, Bin };

// The format a file named `path` is written in: .xb or .xbin is XBIN, .bin
// is BIN (any case), anything else .ans.
ArtFormat artFormatFor(std::string_view path);

// The format the bytes of a file named `path` are read as: XBIN if they
// start with the XBIN magic ("XBIN" 0x1A), whatever the name; otherwise as
// artFormatFor names it.
ArtFormat artFormatOf(std::string_view path, std::string_view file);

// parseAnsi (with `ansi`), parseXbin or parseBin. XBIN and BIN are bytes,
// a glyph each, so `ansi` doesn't apply to them.
AnsiDocument parseArt(std::string_view file, ArtFormat format, const ParseOptions& ansi = {});
// writeAnsi (in `ansi`), writeXbin or writeBin. XBIN and BIN are CP437
// bytes whatever `ansi` says.
AnsiOutput writeArt(const AnsiDocument& doc, ArtFormat format, TextEncoding ansi = TextEncoding::Cp437);

// A file's SAUCE record and the bytes its art takes (`content`), as each
// format bounds them. `.ans`: splitSauce. XBIN: the cells first, from the
// header's size, palette and font flags and the runs that make the cells,
// and SAUCE only in what follows them; `content` is the header through the
// last cell, 0x1A bytes and all. A file cut short of its cells takes a SAUCE
// record at its end as one, as splitSauce does, and its content is what was
// read before it. BIN: splitSauce, except that a record whose FileSize is
// more than the bytes before it is cells, not SAUCE; `content` is the pairs,
// an odd last byte (SAUCE's EOF) dropped. `body` is everything before the
// record (and COMNT) in each.
SauceSplit splitArtSauce(std::string_view file, ArtFormat format);

// A BIN without SAUCE (or whose SAUCE isn't BinaryText) is this wide.
inline constexpr int kDefaultBinWidth = 160;

// Parses an XBIN: the header's width, height and iCE colours (its non-blink
// flag), its palette (6-bit values, widened to 8 as v << 2 | v >> 4, so
// writeXbin gives them back) and font, and the cells, compressed or not. A
// compressed run may cross the end of a row. Cells the data stops short of
// are blank; bytes after the last cell (SAUCE's EOF, say) are ignored. SAUCE
// is kept as read, and is found only after the last cell (splitArtSauce).
// Throws AnsiError for a file that isn't XBIN, a size of 0 or past
// kMaxColumns × kMaxRows, a palette or font cut short, a font height outside
// 1-32, or a 512-character font (its glyphs have no CP437 byte).
AnsiDocument parseXbin(std::string_view file);

struct XbinOptions {
    bool compress = true;
};

// Writes the canvas as XBIN: the header (iCE as writtenIce gives it, the
// font height or 16 without a font), the document's palette and font if it
// has them, then the cells, each row compressed on its own (no run crosses a
// row end) as few bytes as the four run kinds allow, or uncompressed. Then
// SAUCE: the document's record or a fresh one, as XBin (DataType 6, FileType
// 0, TInfo1-2 the width and height, TInfo3-4, TFlags and TInfoS 0), FileSize
// the content's length. The record's 9 px letter spacing, aspect ratio and
// a font name other than IBM VGA, which an XBIN has no place for, are
// reported left out (letterSpacingDropped, aspectDropped, fontNameDropped).
// Glyphs with no CP437 byte are written as '?' and listed in `unwritable`;
// every other glyph, NUL and control codes included, is its byte.
AnsiOutput writeXbin(const AnsiDocument& doc, const XbinOptions& opts = {});

struct BinOptions {
    int defaultWidth = kDefaultBinWidth;
};

// Parses a BIN: with a BinaryText SAUCE (DataType 5) the width is FileType × 2
// (defaultWidth when it is 0) and iCE comes from its flags; otherwise the
// width is defaultWidth and iCE is on, as .ans. As many rows as the pairs
// fill, the last one padded with blank cells. A 0x1A that leaves the data
// one byte past a whole pair is SAUCE's EOF and is dropped, as is any other
// odd last byte. A record whose FileSize is past the bytes before it is read
// as cells (splitArtSauce). Throws AnsiError for a width outside
// 1-kMaxColumns, no pairs, or more than kMaxRows rows.
AnsiDocument parseBin(std::string_view file, const BinOptions& opts = {});

// Writes the canvas as BIN, the cells row by row (glyphs as writeXbin writes
// them, iCE as writtenIce gives it), then SAUCE as BinaryText (DataType 5,
// FileType the width / 2, TInfo1-4 0; the record's flags with iCE set, and
// its font name). BIN can only be an even number of columns wide, so an odd
// width gains a blank column on the right. A custom palette or a font is
// left out (paletteDropped, fontDropped).
AnsiOutput writeBin(const AnsiDocument& doc);

// One row's character/attribute pairs (2 bytes a cell) as XBIN runs, in the
// fewest bytes. Of runs that cost the same, the first of: both repeated,
// the character repeated, the attribute repeated, none; then the longest.
std::string xbinCompressRow(std::span<const std::uint8_t> pairs);

} // namespace bw
