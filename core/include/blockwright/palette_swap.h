#pragma once
// Palette swaps and colour remaps (#497, PLAN §8, Palettes). A swap changes
// the document's palette and no cell: every cell holding index n shows the
// new entry n. A remap changes the cells: index n becomes index map[n], in
// the foreground and the background alike, and the palette stays. Fitting
// to a palette is a remap that keeps the look: each index becomes the entry
// of the new palette nearest its colour in the old one. An RGB cell (an
// eyedropped or IRC colour) is not an index and neither touches it.

#include "blockwright/canvas.h"
#include "blockwright/edit.h"
#include "blockwright/layer.h"
#include "blockwright/palette.h"
#include "blockwright/selection.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bw {

// What each index 0-15 becomes.
using ColourMap = std::array<std::uint8_t, 16>;

// Every index as itself.
ColourMap identityColourMap();
// `a` and `b` swapped.
ColourMap swapColourMap(std::uint8_t a, std::uint8_t b);

// A map as the CLI's --map and the Remap dialog's text take it: pairs
// "from=to" separated by commas ("3=11,5=13"; each index 0-15 in decimal,
// the rest as themselves), or sixteen hex digits, the one at position n
// what n becomes ("0123456789abcdef" is the identity). Spaces are ignored.
// Nothing, and `error` says why, for anything else, or an index named twice
// on the left.
std::optional<ColourMap> parseColourMap(std::string_view text, std::string* error = nullptr);
// The map as pairs for the indices it moves ("3=11,11=3"), or "" for the
// identity.
std::string colourMapText(const ColourMap& map);

// `c` with its indexed foreground and background mapped; its glyph, RGB
// colours, attributes and marks as they were.
Cell remapCell(Cell c, const ColourMap& map);

// The edit that maps the cells of `canvas` in `area` (all of it when
// nothing), each change its before and after: none for a cell the map
// leaves as it was (one in RGB colours, one whose indices map to
// themselves, or a transparent cell). With `transparentToo` a transparent
// cell is mapped as well: a pixel layer keeps a half pixel in the
// foreground of a transparent ▀ or ▄ (layer.h).
CellEdit remapEdit(const Canvas& canvas, const ColourMap& map, std::optional<CellRect> area = std::nullopt,
                   bool transparentToo = false);
// The same, done to the canvas in place; how many cells changed.
int remapCanvas(Canvas& canvas, const ColourMap& map);

// Snap to Micron Colours (#491 slice 3, PLAN §17): `c` with each RGB colour,
// foreground and background, the nearest twelve-bit colour
// (micron::nearestTwelveBit); an indexed colour (one of the 16) stays the
// index it is, and its glyph, attributes and marks as they were.
Cell micronSnapCell(Cell c);
// The edit that snaps the cells of `canvas` in `area` (all of it when
// nothing), as remapEdit takes them: none for a cell already in twelve-bit
// or indexed colours, or a transparent one unless `transparentToo`.
CellEdit micronSnapEdit(const Canvas& canvas, std::optional<CellRect> area = std::nullopt, bool transparentToo = false);

// Whether a remap of every layer maps `layer`'s cells: a cells or pixels
// layer, or a text layer without a source. A live text layer, a generator
// layer and a reference layer draw their cells from something else.
bool remappableLayer(const Layer& layer);
// The edits a remap of every layer by `map` makes, each layer's in its own
// cells (a pixel layer's half pixels too), for the layers whose cells
// change. `skipped` (if given) counts the layers it leaves that have cells
// (not a reference layer).
std::vector<std::pair<LayerId, CellEdit>> remapLayers(const LayerStack& stack, const ColourMap& map,
                                                      int* skipped = nullptr);

// `style` with the colours it names mapped: a plate's, a shadow's, an
// outline's and a flat fill's. A glow, shade, bevel or ramp fill draws from
// a named ramp of indices, which no map changes.
TextStyle remapStyle(TextStyle style, const ColourMap& map);
// The styles a remap of every layer by `map` changes, each the layer's
// style after, for the layers remapLayers maps whose style names a colour
// the map moves.
std::vector<std::pair<LayerId, TextStyle>> remapStyles(const LayerStack& stack, const ColourMap& map);

// The map that fits `from` to `to`: each index the entry of `to` nearest
// its colour in `from` (nearestIndex, ties to the lower index). An index
// whose colour `to` holds exactly keeps that colour.
ColourMap fitColourMap(const Palette& from, const Palette& to);
// The cells of `canvas` the fit changes the look of: a cell whose indexed
// foreground or background shows another colour under `to` after the map
// than under `from` before it. Transparent cells don't count.
int fitLoss(const Canvas& canvas, const Palette& from, const Palette& to, const ColourMap& map);
// The cells of `before` (shown in `from`) that `after` (the same size,
// shown in `to`) shows otherwise: a cell in one colour all over (a blank
// without underline, a full block, a half block or shade in one colour
// twice) in another colour or in two; any other in another glyph,
// attributes, foreground or background colour; a cell transparent in one
// and not the other. For a
// stack's composites before and after a fit, so the cells of the layers and
// ramps a fit leaves as they were count as well.
int lookChanged(const Canvas& before, const Palette& from, const Canvas& after, const Palette& to);

// A palette file (Colours > Palette > From File..., the CLI's --palette):
// an XBIN's palette (the standard one for an XBIN that carries none); a
// GIMP palette ("GIMP Palette" first, then "R G B [name]" lines, comments
// starting '#' and "Name:"/"Columns:" lines skipped); a JASC palette
// ("JASC-PAL", "0100", a count, then "R G B" lines); or one colour a line
// as six hex digits, with or without '#' (a Lospec .hex), blank lines
// skipped. The first 16 colours are the palette, in CGA order; `colours`
// (if given) gets how many the file holds. Nothing, and `error` says why,
// for a file with fewer than 16 or one none of these reads.
std::optional<Palette> readPaletteFile(std::string_view path, std::string_view bytes, std::string* error = nullptr,
                                       int* colours = nullptr);

} // namespace bw
