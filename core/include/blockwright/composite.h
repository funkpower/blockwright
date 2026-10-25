#pragma once
// The compositor (PLAN §5): a layer stack's visible layers, bottom first,
// combined cell by cell into one Canvas, the type the renderer, writeAnsi,
// the IRC encoder and the lint take. These rules are defined here only;
// exports always write the visible composite (flatten).
//
// Each canvas cell starts as the ground (a blank cell, a space on black) and
// takes each visible layer over it in turn, bottom first. Hidden layers,
// reference layers, and the parts of a layer off the canvas add nothing;
// locked and alpha-locked layers composite like any other. A transparent
// cell adds nothing either, so what is under it shows. An opaque cell:
// - of a cells, text or generator layer (a generator's cached output,
//   generator.h), by the layer's mode: normal replaces the cell,
//   chars-only writes its glyph over the colours and attributes under it,
//   colours-only its colours and attributes under the glyph. Nothing is
//   re-picked or quantised, so 24-bit colours pass through as they are.
// - of a text layer, in normal or chars-only mode, makes the cell text: no
//   pixel layer above covers it (plot doesn't erase text: text goes on
//   last). Whether a cell is text goes with its glyph: a cells layer above
//   that writes its glyph (normal or chars-only) makes it a cells cell
//   again, and pixels above can cover it; colours-only leaves it as it
//   was. An opaque space in a text layer is text too, and blocks pixels.
// - of a pixel layer, pixels: the halves it sets replace those halves of
//   the cell under it and the cell is written again as half-block pixels
//   (halfBlockCell): one colour a space or █, two a ▀ or ▄, the way that
//   keeps the background dark. A half it leaves transparent shows the half
//   under it (halvesUnder): a pixel cell's own half, a shade's main colour
//   rather than the solid fallback that leaves a speck, and any other
//   cell's background, as halfblock.h reads it. Under-colours are palette
//   colours, so a 24-bit cell a pixel half covers is quantised, as the
//   half-block tools do.
//
// iCE (CompositeOptions::ice, iceCell in ansi.h): cells under pixels are read
// by the colours they show, so with iCE off a bright background is its dark
// colour. With iCE on any two colours fit a cell (a bright background where
// needed). With iCE off only one bright colour does, and a pixel, on top,
// keeps its colour: where it would share a cell with a different bright
// colour under it, the half under shows dark instead, a shade's background
// (which iCE off always shows dark) or a pixel cell's half's dark
// counterpart. (Plotting into a flat canvas, setHalfBlock, keeps the half
// already there instead.) Where a pixel layer sets both halves to two
// bright colours, the top keeps its colour (as halfBlockCell writes). A
// cell a pixel layer covers entirely has no attributes; one it half covers
// keeps those of the cell under it that the colours don't use (Blink with
// iCE off), the blink only where the half under was blinking (it showed the
// foreground, a colour other than the background's) and keeps its colour:
// it stays the foreground, ▀ or ▄ as needed, unless the pixel is bright
// with iCE off and has to be. A pixel doesn't blink, and nor does a half
// under that showed the background.
//
// A cell depends only on the layers' cells at that cell, so recompositing
// the cells an edit changed (dirtyRect in layer.h) brings a composite up to
// date.

#include "blockwright/canvas.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"

namespace bw {

struct CompositeOptions {
    // The document's iCE colours setting.
    bool ice = true;
    // What shows where no layer has an opaque cell.
    Cell ground{};
};

// The stack's visible composite, the stack's size.
Canvas composite(const LayerStack& stack, const CompositeOptions& options = {});

// Composites the cells of `rect` (clipped to the canvas) into `out`, which
// must be the stack's size (LayerError otherwise), leaving the rest of it
// alone: the same cells composite() gives there. For interactive painting,
// with the rect of what changed: an edit's dirtyRect, a layer's bounds()
// for a change of visibility, mode or kind or a move up or down the
// stack, both bounds for a move across the canvas, united clipped to the
// canvas (unite(before, after, stack.canvasRect())) so a far offset loses
// no cells.
void recomposite(const LayerStack& stack, Canvas& out, CellRect rect, const CompositeOptions& options = {});

// One composited cell; the ground off the canvas.
Cell compositeCell(const LayerStack& stack, int x, int y, const CompositeOptions& options = {});
// One cell composited from the visible layers under the layer at `index`
// (the bottom `index` layers), the ground if there are none: what a clear
// cell of that layer shows through to.
Cell compositeCellBelow(const LayerStack& stack, std::size_t index, int x, int y,
                        const CompositeOptions& options = {});

// What exports write: the visible composite. (LayerStack::fromCanvas of it
// is the stack flattened to one layer.)
inline Canvas flatten(const LayerStack& stack, const CompositeOptions& options = {}) {
    return composite(stack, options);
}

// The colours the halves of `below` show to a pixel layer laid over it,
// with iCE `ice`: a pixel cell's halves (halfBlocks); for ░ its background
// and for ▓ its foreground in both halves (the colour the shade mostly
// shows); for ▒, the darker of its two colours on top and the lighter below,
// by the usual convention for a cut shade; for any other cell its
// background in both.
HalfPair halvesUnder(const Cell& below, bool ice = true);

} // namespace bw
