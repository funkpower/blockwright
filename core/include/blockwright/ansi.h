#pragma once
// .ans files: CP437 text with ANSI escapes, plus SAUCE.

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

struct AnsiDocument {
    Canvas canvas{Canvas::kDefaultWidth, 1};
    std::optional<Sauce> sauce;
    bool ice = true;  // blink = bright background
    // What an XBIN carries besides its cells, kept so that saving it as XBIN
    // writes them back. Nothing draws with them yet: the editor and renderer
    // use kPalette and the bundled IBM VGA font. Only XBIN can hold them, so
    // the other writers report them lost (AnsiOutput, customPalette).
    std::optional<std::array<Rgb, 16>> palette;
    std::optional<FileFont> font;
};

// Whether `doc` has a palette other than kPalette, which only XBIN can write.
bool customPalette(const AnsiDocument& doc);

class AnsiError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ParseOptions {
    int defaultWidth = 80;  // when there is no SAUCE width
};

// Guards against a broken file's cursor jumps or SAUCE eating memory.
inline constexpr int kMaxColumns = 400;
inline constexpr int kMaxRows = 5000;

// Parses an .ans file. The canvas is the SAUCE width (or defaultWidth) wide
// and as tall as the last row anything was drawn on, or the SAUCE height
// (TInfo2, capped at kMaxRows) if that is taller. iCE comes from SAUCE and
// defaults to on. 24-bit colours (PabloDraw CSI t, SGR 38/48) are quantised
// to the palette. Throws AnsiError on input it can't make a canvas from.
AnsiDocument parseAnsi(std::string_view file, const ParseOptions& opts = {});

struct Unwritable {
    int x, y;
    char32_t ch;
};

struct AnsiOutput {
    std::string bytes;
    // Cells written as '?': a glyph with no CP437 code, or (.ans only) one
    // whose byte .ans reads as a control code (ansControlByte).
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
AnsiOutput writeAnsi(const AnsiDocument& doc);

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

// Whether writeAnsi writes `a` and `b` alike with any iCE setting: the same
// CP437 byte (so ß and β are one glyph, and a glyph it can't write is '?'
// either way), the same palette colours and blink, and both transparent or
// neither. Other attribute bits and the lost-blink mark don't count.
bool writesAlike(const Cell& a, const Cell& b);

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

// The names (ansControlName) of the control codes the glyphs in `cells` have
// bytes for, each once, in the order they are first met, joined by ", "
// ("TAB, ESC, LF"); empty if none has one.
std::string ansControlNames(const std::vector<Unwritable>& cells);

// How many of `cells` hold a glyph ansControlByte has a byte for; the rest
// have no CP437 code.
int ansControlCount(const std::vector<Unwritable>& cells);

// Whether `ch` draws no foreground: a space, NUL or 0xFF (U+00A0). Blink
// (with iCE off) hides and shows the foreground, so on these it shows nothing.
bool blankGlyph(char32_t ch);

} // namespace bw
