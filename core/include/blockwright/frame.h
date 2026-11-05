#pragma once
// Frame styles (#345): a border drawn to any size from a small source, as a
// nine-slice. The source is a canvas cut by four insets into a 3x3 grid:
// the four corners are drawn as they are, the four edges repeat along their
// length (several cells thick when the inset is), and the centre is the
// fill, repeated both ways, or left transparent when the style has none.
//
// The repeat starts at the corner a strip begins from (the top left for the
// top edge and the left edge, the top right for the right edge, the bottom
// left for the bottom edge), so a pattern meets the top and left corners as
// the source shows it and is cut wherever the length runs out.
//
// A frame smaller than its minimum (minimumWidth/minimumHeight: the
// corners, or the style's own larger minimum) isn't drawn: drawFrame
// returns nothing, and the Frame tool grows a drag to the minimum instead.
// Nor is one larger than a canvas can be (kMaxColumns x kMaxRows).
//
// A style's source is at most kMaxFrameSourceColumns x kMaxFrameSourceRows
// (so its insets are too), and its JSON form at most kMaxFrameStyleBytes:
// a frame is a border, and a source that size already holds corners a
// screen wide. Its declared minimum may be as large as a canvas.
//
// A cell may keep the background under it: the Frame tool writes its glyph
// and foreground and leaves the colour the art there already has, so a line
// dropped on a coloured scene doesn't cut a black band round itself. Line
// glyphs, half blocks and shades take it; a style whose own background is
// part of the look (a fill, a bevel's dark half, a stripe's colour) doesn't.
#include "blockwright/canvas.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace bw {

inline constexpr int kMaxFrameSourceColumns = 100;
inline constexpr int kMaxFrameSourceRows = 100;
inline constexpr std::size_t kMaxFrameStyleBytes = std::size_t(1) << 20;

struct FrameStyle {
    std::string id;   // a short name, unique among the styles ("double")
    std::string name; // what the picker shows ("Double line")
    // The cells: corners, edges and fill. A transparent cell stays
    // transparent wherever it is repeated to.
    Canvas source{3, 3};
    // The corners' size: the columns on the left and right, the rows on top
    // and at the bottom. The source is wider than left + right and taller
    // than top + bottom, so every edge has at least one cell to repeat.
    int left = 1, right = 1, top = 1, bottom = 1;
    // Whether the centre is drawn; without, the inside is transparent.
    bool fill = false;
    // The smallest frame the style is drawn at, past its corners; 0 for none.
    int minWidth = 0, minHeight = 0;
    // The source cells that keep the background under them (above), row by
    // row; empty when none does.
    std::vector<bool> keepBg;

    bool keepsBackground(int x, int y) const;

    int minimumWidth() const;
    int minimumHeight() const;
    // Whether the insets fit the source as above, keepBg is empty or one a
    // source cell, the source is no larger than kMaxFrameSourceColumns x
    // kMaxFrameSourceRows and the minimum no larger than a canvas. Checked
    // without overflow, whatever the fields hold.
    bool valid() const;
};

// The style drawn width x height cells; nothing below its minimum, above
// kMaxColumns x kMaxRows (checked before anything is allocated) or for a
// style that isn't valid(). `keepBg`, when given, is set to which of the
// drawn cells keep the background under them, row by row.
std::optional<Canvas> drawFrame(const FrameStyle& style, int width, int height,
                                std::vector<bool>* keepBg = nullptr);

// The source column (or row) cell `i` of a strip `length` cells long takes
// when the source is cut by `lead` and `trail` round a middle `middle` cells
// long (at least 1): the lead corner as it is, the middle over and over from
// it, then the trail corner. drawFrame's cut, for one that cuts a source past
// a frame style's limit (a resizable stamp, stamp_slice.h).
int frameSourceIndex(int i, int length, int lead, int middle, int trail);

// `frame` with every cell that isn't transparent in `fg` on `bg`, for the
// Frame tool's "Pen colours". A style's own shading (a bevel's light and dark
// sides) goes flat.
Canvas recolourFrame(Canvas frame, Color fg, Color bg);

// The style as JSON and back:
//   {"id": "double", "name": "Double line", "insets": [l, r, t, b],
//    "fill": false, "min": [w, h],
//    "rows": ["╔═╗", "║ ║", "╚═╝"],   one glyph a cell, UTF-8
//    "fg": ["fff", ...], "bg": ["000", ...],   a hex palette index a cell
//    "keepBg": ["111", "1.1", "111"]}   1 for a cell that keeps the background
// "min", "fill" and "keepBg" may be left out (no cell keeps the
// background). In "bg" a '.' marks a transparent cell. The colours are
// palette indexes (RGB isn't kept). Reading refuses anything else: a key it
// doesn't know, rows of different lengths, a source with no cells, a wide
// glyph, JSON longer than kMaxFrameStyleBytes, a source (or insets) past
// kMaxFrameSourceColumns x kMaxFrameSourceRows, a minimum larger than a
// canvas, a style that isn't valid(); `error` says what. The sizes are
// checked before the rows are copied.
std::string frameStyleJson(const FrameStyle& style);
std::optional<FrameStyle> frameStyleFromJson(const std::string& json, std::string* error = nullptr);

// A style cut from `cells` (a selection saved as a frame style): `id`,
// `name`, the insets and fill as given, its blank cells (isBlank, the ones
// transparent paste skips) transparent, and, when `keepBg`, every other
// cell keeping the background under it. Nothing, and `error`, when the
// insets leave no edge to repeat (valid()), the style would draw no cell,
// or a cell it draws holds what the JSON form can't: a glyph that isn't one
// code point one cell wide, a colour past the 16 of the ANSI palette, or an
// attribute (blink), or the selection is larger than a source may be. A cell of the middle is only checked when `fill`.
std::optional<FrameStyle> frameStyleFromCells(const Canvas& cells, std::string id, std::string name, int left,
                                              int right, int top, int bottom, bool fill, bool keepBg,
                                              std::string* error = nullptr);

// The styles that come with the app, in the picker's order.
const std::vector<FrameStyle>& bundledFrameStyles();
const FrameStyle* findFrameStyle(const std::string& id);

} // namespace bw
