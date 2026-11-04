#pragma once
// .ans files: CP437 (or UTF-8) text with ANSI escapes, plus SAUCE.

#include "blockwright/canvas.h"
#include "blockwright/sauce.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw {

// A font a file carries (XBIN): 256 glyphs 8 px wide, `height` bytes each,
// one per row, bit 7 leftmost.
struct FileFont {
    int height = 16;
    std::string bits;  // 256 * height bytes

    bool operator==(const FileFont&) const = default;
};

// The character set of an .ans file's text: CP437, a byte a glyph, or
// UTF-8, the way a terminal (or a paste from one) writes it.
enum class TextEncoding { Cp437, Utf8 };

// What a UTF-8 .ans read couldn't keep (PLAN §16, Reading). Every other
// read keeps everything.
struct ReadLoss {
    // Zero-width code points dropped past a cluster's kMaxClusterCodePoints,
    // the clusters they would have joined, and the first one's cell.
    int marksDropped = 0;
    int markClusters = 0;
    int markX = -1, markY = -1;
    // Clusters read as their base, their marks dropped: the pool was full.
    int poolRefused = 0;

    bool lossy() const { return marksDropped || poolRefused; }
};

struct AnsiDocument {
    Canvas canvas{Canvas::kDefaultWidth, 1};
    // What parseAnsi read the text as. writeAnsi doesn't follow it: its
    // caller picks the encoding (the app's Save keeps this one).
    TextEncoding encoding = TextEncoding::Cp437;
    std::optional<Sauce> sauce;
    bool ice = true;  // blink = bright background
    // What an XBIN carries besides its cells, kept so that saving it as XBIN
    // writes them back. Nothing draws with them yet: the editor and renderer
    // use kPalette and the bundled IBM VGA font. Only XBIN can hold them, so
    // the other writers report them lost (AnsiOutput, customPalette).
    std::optional<std::array<Rgb, 16>> palette;
    std::optional<FileFont> font;
    // What parseAnsi dropped reading it.
    ReadLoss readLoss;
};

// Whether `doc` has a palette other than kPalette, which only XBIN can write.
bool customPalette(const AnsiDocument& doc);

class AnsiError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ParseOptions {
    int defaultWidth = 80;  // when there is no SAUCE width
    // The text's character set; nothing: what detectEncoding finds.
    std::optional<TextEncoding> encoding;
    // The cap on the cluster pool a UTF-8 read interns with
    // (ClusterPool::intern), lowered by tests.
    std::size_t poolCap = ClusterPool::kLossyCap;
};

// The character set of an .ans file's text, `content` being the bytes
// before its SAUCE record and first EOF (splitSauce). UTF-8 when `content`
// starts with a UTF-8 byte order mark (EF BB BF), or when every byte of it
// from 0x80 up is part of a well-formed UTF-8 sequence (nextUtf8) and at
// least one of those sequences is three or four bytes long. Otherwise CP437.
// The three-byte rule keeps CP437 bytes that happen to pair up as two-byte
// sequences (╔╗ is C9 BB, U+027B) reading as CP437; the block and box
// elements terminal art is drawn with (U+2500-U+259F) are all three bytes.
TextEncoding detectEncoding(std::string_view content);

// Guards against a broken file's cursor jumps or SAUCE eating memory.
inline constexpr int kMaxColumns = 400;
inline constexpr int kMaxRows = 5000;

// Parses an .ans file. The canvas is the SAUCE width (or defaultWidth) wide
// and as tall as the last row anything was drawn on, or the SAUCE height
// (TInfo2, capped at kMaxRows) if that is taller. iCE comes from SAUCE and
// defaults to on. 24-bit colours (PabloDraw CSI t, SGR 38/48) are quantised
// to the palette. The text is read as opts.encoding, or as detectEncoding
// finds it (AnsiDocument::encoding says which). As UTF-8, a byte under 0x80
// is read as it is in CP437 (escapes and controls act, 0x01-0x1F and 0x7F
// other than those are the CP437 pictographs, NUL a space); each well-formed
// sequence of two to four bytes is a code point, and any other byte from
// 0x80 up is U+FFFD; and a byte order mark at the start of the content is
// skipped. Code points are laid out by their width (codePointWidth, PLAN §16,
// Reading): a narrow one, or a control, takes a cell; a wide one a pair, its
// lead and the continuation, wrapping first when the cursor is on the last
// column (as xterm does) and read as U+FFFD on a one-column canvas; and a
// zero-width one joins the cluster in the last cell written (a pair's lead)
// unless the cursor has moved since (a line break, CR, TAB, any escape but
// SGR and CSI t), nothing has been written yet, or that cell holds a C1
// control, U+2028 or U+2029: then it takes a cell of its own. A cluster keeps at most
// kMaxClusterCodePoints; the code points past that are dropped, and a
// cluster the pool refuses (past opts.poolCap) is read as its base. Both
// are counted in readLoss. A glyph written over half of a pair leaves the
// other half a space. SAUCE's text fields are CP437
// either way, as the SAUCE spec has them. Throws AnsiError on input it
// can't make a canvas from.
AnsiDocument parseAnsi(std::string_view file, const ParseOptions& opts = {});

// What a writer loses of a cell's glyph (PLAN §16, Writing).
enum class Loss : std::uint8_t {
    NoCode,        // no byte for it (CP437), written '?'
    MarksDropped,  // a cluster written as its base (`written`), its marks dropped
    Wide,          // a lead, written '?' and its continuation a space
    Control,       // a byte .ans reads as a control code (CP437), or a code
                   // point utf8Writable refuses (UTF-8), written '?'
    // UTF-8 only (utf8LoneLoss), each written '?' so the row keeps its columns:
    LoneWide,  // a two-column glyph with no continuation after it
    LoneMark,  // a zero-width base the reader would join to the cluster before it
};

struct Unwritable {
    int x, y;
    Glyph ch;  // never the continuation, which is written as a space
    Loss loss = Loss::NoCode;
    char32_t written = U'?';  // what the cell was written as
    // A lead written '?', its continuation as a space, by any writer
    // (Loss::Wide; in UTF-8 and IRC text Loss::Control when a code point is a
    // control, which CP437, XBIN and BIN don't tell apart)
    bool lead = false;
};


struct AnsiOutput {
    std::string bytes;
    // The text encoding the glyphs were written in: UTF-8 only for an .ans
    // writeAnsi wrote as UTF-8. XBIN and BIN are CP437 bytes.
    TextEncoding encoding = TextEncoding::Cp437;
    // Cells not written as their glyph, in cell order. CP437 (cp437Glyph):
    // a lead, a glyph or cluster base with no CP437 code, a cluster written
    // as its base, and (.ans only) a glyph or base whose byte .ans reads as
    // a control code. UTF-8: a cell holding a code point utf8Writable
    // refuses, a lead whose glyph doesn't take two columns (glyphColumns),
    // written '?' and its continuation a space, and a lone wide cell or a
    // lone mark (utf8LoneLoss), written '?'.
    std::vector<Unwritable> unwritable;
    // Blinking cells written without their blink: the file is written with
    // iCE colours on, where blink means a bright background. Only cells whose
    // blink shows count: blink hides and shows the foreground, so on a blank
    // glyph (blankGlyph) it shows nothing and nothing is lost.
    int blinkDropped = 0;
    // The document's palette (customPalette) or font, which this format
    // can't hold, left out.
    bool paletteDropped = false;
    bool fontDropped = false;
    // SAUCE fields an XBIN has no place for (its record's TFlags and TInfoS
    // are 0, and its header has neither), left out by writeXbin: 9 px
    // letter spacing (an XBIN draws 8 px), the aspect ratio (SAUCE's value,
    // 1 legacy or 2 square), and a font name other than the IBM VGA an XBIN
    // means without one. 8 px and an unset flag are what an XBIN draws, so
    // they lose nothing.
    bool letterSpacingDropped = false;
    int aspectDropped = 0;
    std::string fontNameDropped;

    bool lossy() const {
        return !unwritable.empty() || blinkDropped || paletteDropped || fontDropped || letterSpacingDropped ||
               aspectDropped || !fontNameDropped.empty();
    }
};

// Writes the canvas in canonical form: ESC[0m first; per row, trailing
// black spaces trimmed, a full SGR (0;[1;][5;]3x;4x) whenever the colours
// change, ESC[0m at the end of each row and CR LF unless the row is full width
// (it wraps on its own); an empty row is a single space. Then SAUCE, with the
// document's record if it has one (width, height, iCE and FileSize updated)
// or a fresh one. iCE is the document's, except that a canvas with a bright
// background is written with iCE on (blinking cells then lose their blink,
// counted in `blinkDropped` unless the glyph is blank; see writtenIce). Glyphs
// with no CP437 byte, or whose byte is a control code the parser would act
// on, are written as '?' and listed in `unwritable`. A custom palette or a
// font is left out (paletteDropped, fontDropped).
//
// With `encoding` UTF-8 the escapes, rows and SAUCE are the same, and each
// glyph is written as UTF-8: one of 0x20-0x7E as its byte, NUL as a space,
// anything else utf8Writable takes as its code point's sequence (so every
// CP437 glyph, ○ ◙ ♪ → ← included, and any code point CP437 lacks), and
// the rest (C0 and C1 controls, DEL, surrogates) as '?', listed in
// `unwritable`. So are a lone wide cell and a lone mark the reader would
// join to the cell before it (utf8LoneLoss), so the row keeps its columns
// when it is read back or drawn by a terminal. The content starts with a byte
// order mark only when it would not be read back as UTF-8 without one
// (detectEncoding: no sequence of three bytes or more), so parseAnsi of the
// bytes always gives the cells back, read as UTF-8. SAUCE's text fields stay
// CP437 bytes, and FileSize counts the UTF-8 content, mark and all. The
// document's own `encoding` is not consulted.
AnsiOutput writeAnsi(const AnsiDocument& doc, TextEncoding encoding = TextEncoding::Cp437);

// Whether writeAnsi writes `ch` as itself in a UTF-8 .ans: NUL (a blank),
// 0x20-0x7E, and every Unicode scalar value from U+00A0 up (no surrogate,
// nothing past U+10FFFF). The other C0 controls and DEL are not: a byte
// under 0x80 reads back as its CP437 glyph or acts as a control. Nor are
// the C1 controls U+0080-U+009F, which a terminal that honours them acts on
// (C2 9B is a CSI), as the IRC export writes them as '?', nor U+2028 and
// U+2029, which the width table refuses as it does C1 (PLAN §16, Width).
bool utf8Writable(char32_t ch);

// Whether a UTF-8 .ans (and IRC text) writes a lead holding `g` whole: the
// width table makes it two columns and utf8Writable takes every code point
// of it. The table changes only what is written, never the columns.
bool utf8WideGlyph(Glyph g);

// What a UTF-8 .ans (writeAnsi) loses of a cell holding `g` that isn't a
// lead and whose code points utf8Writable all takes (PLAN §16, Writing):
// Loss::LoneWide when the table makes it two columns, which a terminal draws
// over the next cell too and the reader lays out as a pair, and
// Loss::LoneMark when its base is zero-width and `afterCluster` (something
// was written before it on the row, or the row before it was written full
// width, with its wrap pending), as the reader joins it to the cluster
// before it and a terminal draws it there. Either way the cell is written
// '?'. A lone mark at the start of the file or of a row after a line break
// is written as it is: it reads back as a cell of its own.
std::optional<Loss> utf8LoneLoss(Glyph g, bool afterCluster);

// The iCE colours writeAnsi, writeXbin and writeBin write `doc` with: its
// own, or on when a cell has a bright background. With them on, the cells
// whose blink shows (not on a blank glyph) lose it, and `blinkDropped` (if
// given) is set to how many.
bool writtenIce(const AnsiDocument& doc, int* blinkDropped = nullptr);

// A cell as it shows, and is saved, with iCE colours `ice`. Blink and a
// bright background are one bit here, as in Moebius and PabloDraw: with iCE
// on, a blinking cell shows a bright background (its background index gains
// 8 and it loses the blink attribute); with iCE off, a bright background
// shows as blink on the dark colour (the index loses 8, the cell blinks).
// Other cells come back as they were. A 24-bit background counts as its
// palette index, as writeAnsi counts it. The editor keeps cells as drawn and
// shows and saves them through this, so its iCE toggle changes how they
// look, not what they hold.
Cell iceCell(Cell c, bool ice);

// What openAsEditor changed.
struct Opened {
    bool iceInferred = false;  // iCE colours turned on for a bright background
    int blinkDropped = 0;      // blinking cells that lost their blink for it, as
                               // writeAnsi counts them (not on a blank glyph)
};

// Makes a parsed document what the editor opens, and `cli shade` shades: a
// file with iCE colours off that holds a bright background anyway (SGR
// 100-107, a 24-bit colour) is opened with them on, its SAUCE flag too, as
// writeAnsi would write it. Its blinking cells lose their blink and keep
// their dark background, as writeAnsi writes them then, so iceCell doesn't
// show or save them bright; those whose blink showed (not on a blank glyph)
// are counted, as writeAnsi counts them, and marked (Cell::lostBlink). A file
// with iCE colours on, or without a bright background, is left as it is. The
// blink lost is lost on open, so a save should still count it, for each
// marked cell it still writes without a blink (lostBlinkCount).
Opened openAsEditor(AnsiDocument& doc);

// The cells of `doc` marked as having lost their blink on open
// (Cell::lostBlink) that writeAnsi still writes without one, with the
// document's iCE colours: not a blank glyph or a hole. A cell drawn over
// with something else, or erased, isn't marked any more.
int lostBlinkCount(const AnsiDocument& doc);

// The CP437 byte a writer writes for a cell holding `g` whose right
// neighbour holds `right`, and what it loses (PLAN §16, Writing): a
// continuation is a space; a lead (any glyph before a continuation) '?',
// Wide; a code point with no byte '?', NoCode; NUL a space in an .ans and
// 0x00 in XBIN and BIN; a cluster whose NFC form is one CP437 glyph
// (cp437Composed) that byte; any other cluster its base's byte,
// MarksDropped, or '?', NoCode, when the base has none. With `ans`, a byte
// .ans reads as a control code (ansControlName), a glyph's or a cluster
// base's, is '?', Control; XBIN and BIN keep it.
struct Cp437Glyph {
    std::uint8_t byte;
    std::optional<Loss> loss;
    // What the cell reads as: the base for MarksDropped, else '?'.
    char32_t written() const;
};
Cp437Glyph cp437Glyph(Glyph g, Glyph right, bool ans);

// Whether writeAnsi writes `a` and `b` alike with any iCE setting: the same
// CP437 byte (cp437Glyph: so ß and β are one glyph, and a glyph it can't
// write is '?' either way), the same palette colours and blink, and both
// transparent or neither. Other attribute bits and the lost-blink mark don't
// count.
bool writesAlike(const Cell& a, const Cell& b);
// The same, with the glyph each cell has on its right, so a lead (a cell
// before a continuation) is judged as written: '?'. The two-cell form judges
// both as having none.
bool writesAlike(const Cell& a, Glyph aRight, const Cell& b, Glyph bRight);

// Whether writeAnsi writes `ch` as itself: it has a CP437 byte and that byte
// isn't one the parser acts on (TAB, LF, CR, SUB, ESC) or NUL.
bool isWritableGlyph(char32_t ch);

// The control code the ANSI parser reads CP437 byte `b` as instead of
// drawing it ("TAB", "LF", "CR", "SUB" or "ESC" for 0x09, 0x0A, 0x0D, 0x1A,
// 0x1B), or nullptr for a byte it draws.
const char* ansControlName(std::uint8_t b);

// The CP437 byte of `ch` when it is one ansControlName names: CP437 has the
// glyph (○ ◙ ♪ → ←) but .ans can't hold its byte, so writeAnsi writes '?'
// for it. XBIN and BIN keep it. Nothing for any other glyph.
std::optional<std::uint8_t> ansControlByte(char32_t ch);
// The same for a cell's glyph: nothing for a cluster or the continuation.
std::optional<std::uint8_t> ansControlByte(Glyph g);

// The names (ansControlName) of the control codes the cells in `cells` lost
// as a control have bytes for (a cluster by its base's), each once, in the
// order they are first met, joined by ", " ("TAB, ESC, LF"); empty if none
// has one.
std::string ansControlNames(const std::vector<Unwritable>& cells);

// How many of `cells` were lost as a control (Loss::Control).
int ansControlCount(const std::vector<Unwritable>& cells);

// Whether `ch` draws no foreground: a space, NUL or 0xFF (U+00A0). Blink
// (with iCE off) hides and shows the foreground, so on these it shows nothing.
// A cluster or the continuation is never blank.
bool blankGlyph(Glyph g);

} // namespace bw
