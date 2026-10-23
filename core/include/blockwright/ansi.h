#pragma once
// .ans files: CP437 text with ANSI escapes, plus SAUCE.

#include "blockwright/canvas.h"
#include "blockwright/sauce.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw {

struct AnsiDocument {
    Canvas canvas{Canvas::kDefaultWidth, 1};
    std::optional<Sauce> sauce;
    bool ice = true;  // blink = bright background
};

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
    std::vector<Unwritable> unwritable;  // cells written as '?'
    // Blinking cells written without their blink: the file is written with
    // iCE colours on, where blink means a bright background.
    int blinkDropped = 0;

    bool lossy() const { return !unwritable.empty() || blinkDropped; }
};

// Writes the canvas in canonical form: ESC[0m first; per row, trailing
// black spaces trimmed, a full SGR (0;[1;][5;]3x;4x) whenever the colours
// change, ESC[0m at the end of each row and CR LF unless the row is full width
// (it wraps on its own); an empty row is a single space. Then SAUCE, with the
// document's record if it has one (width, height, iCE and FileSize updated)
// or a fresh one. iCE is the document's, except that a canvas with a bright
// background is written with iCE on (blinking cells then lose their blink,
// counted in `blinkDropped`). Glyphs with no CP437 byte, or whose byte is a
// control code the parser would act on, are written as '?' and listed in
// `unwritable`.
AnsiOutput writeAnsi(const AnsiDocument& doc);

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
    int blinkDropped = 0;      // blinking cells that lost their blink for it
};

// Makes a parsed document what the editor opens, and `cli shade` shades: a
// file with iCE colours off that holds a bright background anyway (SGR
// 100-107, a 24-bit colour) is opened with them on, its SAUCE flag too, as
// writeAnsi would write it. Its blinking cells lose their blink and keep
// their dark background, as writeAnsi writes them then, so iceCell doesn't
// show or save them bright. A file with iCE colours on, or without a bright
// background, is left as it is. The blink lost is lost on open, so a save
// should still count it (Document::encode, `cli shade`).
Opened openAsEditor(AnsiDocument& doc);

// Whether writeAnsi writes `ch` as itself: it has a CP437 byte and that byte
// isn't one the parser acts on (TAB, LF, CR, SUB, ESC) or NUL.
bool isWritableGlyph(char32_t ch);

} // namespace bw
