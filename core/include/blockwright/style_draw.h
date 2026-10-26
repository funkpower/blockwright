#pragma once
// Drawing a text style (text_style.h, PLAN §15).
//
// A style reads the letters as a mask of half-block pixels (InkMask, two
// pixel rows a cell) and the cells they are drawn in: ink pixels in their
// own colours, and source cells (a TheDraw face's, or a cells layer's)
// where the letters have them. Each effect switched on is drawn as a layer
// of its own (cells or pixels, layer.h) and the layers are composited in
// EffectKind's order by the compositor's rules (§5) over nothing, so a later
// effect wins where two share a cell and, with iCE off, a pixel on top keeps
// its colour and the half under goes dark. A cell no effect draws stays
// transparent. A half-block pixel drawn over nothing is a ▀ or ▄ on black,
// as an unstyled live text layer draws its letters.
//
// What each effect reads and draws:
// - Plate: the text's box grown by its padding, cells of its colour.
// - Glow: the cells within its width of a cell with ink, the outline's ring
//   counted as ink when the outline is on (Chebyshev distance in cells).
//   Ring d (1 to width) is rung width - d + 1 of the ramp, and the cells
//   with ink rung width, so the half of a ▀/▄ fringe off the letter shows
//   the ring's colour; each cell as rungCell writes it.
// - Shadow: the ink, grown by the outline's ring when it is on, moved
//   `distance` pixels towards its direction (a diagonal moves that far
//   along both axes; across, a pixel is a column); an extrusion every move
//   from one pixel to `distance`. Pixels of its colour.
// - Outline: pixels: the pixels that aren't ink with ink among their eight
//   neighbours; cells: the cells without ink with ink in one of their eight
//   neighbours, whole. In its colour.
// - Fill: flat: every ink pixel in its colour, and each source cell's
//   foreground. Rows and columns: each line (a text's lines; the whole
//   layer otherwise) runs the ramp's ladder down its ink's cell rows, or
//   across its cell columns, one rung a row or column from light (top, or
//   left) to dark, never down to the ramp's darkest colour (rung 4 is the
//   last). A line with fewer rows than those rungs takes the middle of the
//   ladder; one with more repeats rungs. A full ink cell is the rung's cell
//   (rungCell); a ▀/▄ fringe takes the solid ramp colour nearest its pixel:
//   for rows, half a rung lighter for a top pixel and half a rung darker
//   for a bottom one, so on a ▒ rung the top pixel takes the lighter
//   colour; for columns the solid nearest the rung, the lighter on a tie.
//   Rainbow: each letter (a run of columns with ink in the line, so letters
//   that touch are one) one ramp colour, cycling through the ramp's
//   colours from the second (the darkest is black for every bundled ramp).
//   In each ramp mode the source cells keep their characters and their
//   colours (foreground and background) are mapped onto the ramp by rank,
//   dark to light, as ascii.py's recolour does. With Fill off the letters
//   are drawn as they are: ink pixels in their colours, source cells as
//   they are.
// - Shade: the full ink cells (a source cell only when it is █) as Fill
//   left them, if they are on the ramp, stepped down `k` rungs (stepRung),
//   `k` the cells from the letter's side facing the light (a diagonal light:
//   half the sum of both axes' distances), never below rung 4.
// - Bevel: the full ink cells (a source cell only when it is █) the rim
//   light brush (brushes/detail.h) lights with the effect's light direction,
//   one rung up, judged on the letters as Fill and Shade left them on black.
//
// iCE off: Fill and Glow walk their rungs a rung at a time (stepRung with
// iCE off), each from the one before (the first from the ramp's darkest
// colour) towards its target, and stop short of the first rung that needs
// a bright background: so no rung is skipped, and a ramp whose next rungs
// need one (fire's LightRed–Yellow step) stops below them. Shade and Bevel
// use stepRung with iCE off and so write no such rung either.

#include "blockwright/canvas.h"
#include "blockwright/face.h"
#include "blockwright/layer.h"
#include "blockwright/text_style.h"

#include <utility>
#include <vector>

namespace bw {

struct StyleInput {
    // The letters' half-block pixels: width columns, two pixel rows a cell.
    InkMask mask;
    // The ink pixels outside source cells, as a pixel layer's cells
    // (layer.h) in their colours: what Fill leaves them when it is off.
    Canvas pixels{0, 0};
    // The source cells, opaque, where the letters have them; transparent
    // elsewhere. Their pixels are ink in the mask when they aren't blank.
    Canvas source{0, 0};
    // The text's box, in cells, for the plate.
    CellRect box;
    // The lines, as ranges of pixel rows [first, end).
    std::vector<std::pair<int, int>> bands;
};

// What a style reads from a layer it draws at composite time (a cells or
// pixels layer): on a pixels layer each
// set pixel is ink in its colour; on any other each cell that isn't blank
// (isBlank, as transparent paste reads blank) is a source cell, both its
// pixels ink. The box is the ink's bounding box and the whole layer is one
// line.
StyleInput styleInputOf(const Layer& layer);

// The style drawn on `in` (see above), the canvas `margins` larger than
// the input on each side, the input at (left, top). What the effects draw
// beyond the canvas is dropped. The style must be drawable
// (textStyleProblem); a ramp this build lacks draws nothing.
Canvas drawStyle(const StyleInput& in, const TextStyle& style, bool ice, StyleMargins margins = {});

// Whether the layer's style is drawn at composite time: it has one with an
// effect switched on and no text source, so it is a cells or pixels layer
// (layerStyleProblem, text.h; a live text layer's style is drawn into its
// cells). An empty stack, or one with every effect off, composites as no
// style at all.
bool styledAtComposite(const LayerProperties& props);

// The layer's cells with its style drawn on them, the layer's size (what
// the compositor shows for a layer styledAtComposite): what the style draws
// past the layer's edges is clipped.
Canvas styledCells(const Layer& layer, bool ice);

} // namespace bw
