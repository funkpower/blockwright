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

// Whether writeAnsi writes `ch` as itself: it has a CP437 byte and that byte
// isn't one the parser acts on (TAB, LF, CR, SUB, ESC) or NUL.
bool isWritableGlyph(char32_t ch);

} // namespace bw
