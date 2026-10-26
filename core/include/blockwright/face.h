#pragma once
// Faces (PLAN §15): one interface whatever the source. A face turns text
// into a letter mask, which half-block pixels are ink, and, for TheDraw
// fonts, the glyphs' own source cells. The Text tool's settings (case,
// extra letter and line spacing, alignment) and the text layer are built on
// top of this; here a line of text is laid out with the face's own letter
// spacing, wrapped at a width in columns.
//
// The bundled pixel faces are `block` (IBM PC BIOS 8x8), `vga` (IBM VGA
// 8x16) and `mini` (funkpower's, 4 cell rows high). They draw printable
// ASCII (U+0020 to U+007E); anything else is missing.

#include "blockwright/selection.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bw {

// Half-block pixels: `width` columns (one a cell) by `height` pixel rows (two
// a cell, the top half first), row-major.
struct InkMask {
    int width = 0, height = 0;
    std::vector<std::uint8_t> ink;  // 1 = ink

    InkMask() = default;
    InkMask(int w, int h) : width(w), height(h), ink(static_cast<size_t>(w) * h) {}

    // False outside the mask.
    bool at(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height && ink[static_cast<size_t>(y) * width + x];
    }
    void set(int x, int y, bool on = true) { ink[static_cast<size_t>(y) * width + x] = on; }
    int count() const;

    bool operator==(const InkMask&) const = default;
};

// Whether a source cell is ink: it is unless it is blank as transparent
// paste reads blank (isBlank: a hole, or a space, NUL or U+00A0 on black
// without blink). A space on any other background is ink.
bool sourceCellInk(const Cell& c);
// The mask of a block of source cells: both pixels of each ink cell.
InkMask maskOfCells(const CellBlock& cells);

// One character of a face. The mask is `face.height()` pixel rows high, its
// width the glyph's advance before letter spacing; its bottom row is the
// face's baseline row (descenders included). `cells`, for faces built from
// cells (TheDraw), holds the glyph's source cells, width x height/2, and
// `mask` is maskOfCells(*cells).
struct Glyph {
    InkMask mask;
    std::optional<CellBlock> cells;
};

// Two characters of a face that misread for each other at 1x (`a` comes
// first in its set).
struct ConfusablePair {
    char32_t a = 0, b = 0;
    bool operator==(const ConfusablePair&) const = default;
};

class Face {
public:
    virtual ~Face() = default;

    virtual std::string_view name() const = 0;
    // Glyph height in pixel rows: even, two a cell row.
    virtual int height() const = 0;
    // Blank columns between two glyphs.
    virtual int letterSpacing() const = 0;
    // The glyph for `ch`, or nothing when the face can't draw it.
    virtual std::optional<Glyph> glyph(char32_t ch) const = 0;
    // The confusable pairs of this face in kConfusableSets: shipped as data
    // for the bundled faces, screened on load for a user font.
    virtual std::vector<ConfusablePair> confusables() const = 0;
};

// The sets screened for confusables: letters that misread at 1x.
inline constexpr std::array<std::u32string_view, 3> kConfusableSets = {U"ZB8S5O0I1", U"Z2zMWKAH", U"l23456_"};

// The confusable rule: two masks, aligned left and on the baseline (their
// bottom rows) and padded to the wider, differ in at most 2 pixels, or are
// the same apart from a one-pixel shift (left, right, up or down).
bool confusableMasks(const InkMask& a, const InkMask& b);
// Every pair within each of kConfusableSets that `face` draws both of and
// whose masks are confusable, in set order. (No two sets share two
// characters, so no pair comes from two sets.)
std::vector<ConfusablePair> screenConfusables(const Face& face);
// The face's confusable pairs with both characters in `text`.
std::vector<ConfusablePair> confusablesIn(const Face& face, std::u32string_view text);

struct LayoutOptions {
    // Wrap width in columns; 0 doesn't wrap.
    int wrapWidth = 0;
};

// Laid-out text. Line i is at pixel row i * face.height(), starting at
// column 0; '\n' ends a line. Lines wrap at spaces, and a word longer than
// the wrap width breaks at the character that would cross it (a line holds
// at least one character). The spaces between two words at a wrap are
// dropped; a paragraph's leading and ending spaces are laid out like any
// character, at a wrap too, and are no place to wrap. A character the face
// can't draw takes the width of its space, blank, and is listed in `missing`.
struct TextLayout {
    struct Line {
        size_t first = 0, count = 0;  // the characters of the text on this line
        int width = 0;                // columns, without trailing letter spacing
    };
    InkMask mask;  // as wide as the widest line
    // The source cells, transparentCell() where no glyph has any; only when
    // a glyph laid out has them.
    std::optional<CellBlock> cells;
    std::vector<Line> lines;
    std::vector<size_t> missing;  // indexes into the text
};

TextLayout layoutText(const Face& face, std::u32string_view text, const LayoutOptions& opts = {});

// The bundled pixel faces.
const Face& blockFace();
const Face& vgaFace();
const Face& miniFace();
// All of them, in that order.
std::span<const Face* const> bundledFaces();
// A bundled face by name, or null.
const Face* bundledFace(std::string_view name);

} // namespace bw
