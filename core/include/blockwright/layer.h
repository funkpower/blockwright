#pragma once
// Layers (PLAN §5): the kinds, their properties and the ordered stack. The
// compositor that turns a stack into one Canvas is in composite.height.
//
// Every layer keeps its cells in a Canvas of its own size, placed on the
// document's canvas at its offset; it can be smaller than the canvas, run
// off it, and move. A cell is opaque or transparent (Cell::transparent);
// there is no blending. A new layer is all transparent cells.
//
// The cells mean different things by kind:
// - cells: glyph, colours and attributes, as a Canvas holds them.
// - text: the same cells, read as text: they composite above shading and a
//   pixel layer never covers them (composite.height). A live text layer has
//   a source in its properties (LayerProperties::text: the text, the Text
//   tool's settings and the face) that its cells are drawn from (text.h);
//   tools don't edit those cells, a change of the source redraws them. The
//   compositor and the project format's .cells file only ever see the
//   cells. A text layer without a source is cells read as text, as before
//   live text. bake() turns a text layer into a cells layer.
// - pixels: half-block pixels, two to a cell, each a palette colour or
//   transparent, stored in one cell (layerPixels, pixelLayerCell): an opaque
//   cell is a pixel cell as halfblock.height writes it with iCE on (space, █, ▀,
//   ▄), both halves set; a transparent ▀ has only its top pixel (in its
//   foreground), a transparent ▄ only its bottom one; any other transparent
//   cell has none. So a pixel layer is edited, undone and saved through
//   CellEdits and Canvas cells like the others. Its pixels are resolved to
//   ▀ ▄ █ or a space against what is under them when the stack is
//   composited, not when they are plotted.
// - generator: the cells a program made (generator.h), its cached output,
//   composited as a cells layer's are. Tools don't edit them; running the
//   generator again replaces them. bake() turns it into a cells layer.
// - reference: a picture to trace (ReferenceImage, below), drawn by the
//   views over the art. It has no cells (0 x 0), takes no paint, and the
//   compositor skips it, so it is never exported.
//
// Groups (pass-through only in PLAN §5) are not modelled yet: the stack is
// flat.

#include "blockwright/brush.h"
#include "blockwright/canvas.h"
#include "blockwright/edit.h"
#include "blockwright/selection.h"
#include "blockwright/text_source.h"
#include "blockwright/text_style.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bw {

enum class LayerKind : std::uint8_t { Cells, Pixels, Text, Generator, Reference };

// How a layer's opaque cells combine with what is under them. Normal
// replaces the cell; chars-only writes the glyph and keeps the colours and
// attributes under it; colours-only writes the colours and attributes and
// keeps the glyph (repaint colours without touching glyphs). Pixel layers
// always composite as pixels: a pixel is a colour, so the mode doesn't
// apply to them.
enum class LayerMode : std::uint8_t { Normal, CharsOnly, ColoursOnly };

// Stable names for the project format (layers/<id>.json): cells, pixels,
// text, generator, reference; normal, chars-only, colours-only.
std::string_view layerKindName(LayerKind kind);
std::optional<LayerKind> layerKindFromName(std::string_view name);
std::string_view layerModeName(LayerMode mode);
std::optional<LayerMode> layerModeFromName(std::string_view name);

// A layer's id: unique in its stack and never reused by it, so undo and the
// project format can name a layer across adds, removes and moves. 0 is no
// layer.
using LayerId = std::uint32_t;

// The cells in both (CellRect from selection.h); empty if none. Unlike
// CellRect::intersected these work in 64 bits, so a layer far off the
// canvas never overflows.
CellRect intersect(CellRect a, CellRect b);
// The smallest rectangle holding both (an empty one adds nothing). Its
// width and height saturate at INT_MAX, so two rects far apart (a layer
// moved to a far offset) can lose cells: unite them clipped (below) when
// the result is for recomposite.
CellRect unite(CellRect a, CellRect b);
// Both clipped to `clip` first, then united: the region to recomposite
// after a move (clip the canvas, a and b the layer's bounds before and
// after), exact at any offset.
CellRect unite(CellRect a, CellRect b, CellRect clip);

// A reference layer's picture (PLAN §5, #178): a PNG or JPEG kept as its
// file's bytes, which the core never decodes, and where it is drawn. Its
// place and size are in canvas pixels (the renderer's 1x pixels, a cell
// being its glyph cell), not cells: (x, y) is its top-left corner, either
// may be negative or past the canvas, and width by height the area it is
// scaled across, its aspect kept or not. Opacity is a percentage.
struct ReferenceImage {
    static constexpr int kMaxPlace = 1 << 24;  // |x|, |y|, width and height at most
    std::shared_ptr<const std::string> bytes;
    int x = 0, y = 0;
    int width = 1, height = 1;
    int opacity = 50;  // 0-100
    // The bytes by value, so a copy read back from a file is equal.
    bool operator==(const ReferenceImage& o) const;
};

// "png" or "jpg" by the file's signature; empty for anything else.
std::optional<std::string_view> referenceImageType(std::string_view bytes);

// Everything about a layer but its cells: what layers/<id>.json holds.
struct LayerProperties {
    LayerId id = 0;
    std::string name;
    LayerKind kind = LayerKind::Cells;
    // The canvas cell the layer's top-left cell sits on; either may be
    // negative or past the canvas.
    int x = 0, y = 0;
    bool visible = true;
    // Locked: no tool edits it. Alpha-locked: tools edit only its opaque
    // cells (StrokeOptions::lockTransparent, layerStrokeOptions). Neither
    // changes how it composites.
    bool locked = false;
    bool alphaLocked = false;
    LayerMode mode = LayerMode::Normal;
    // A live text layer's source (text.h): only a text layer has one.
    std::optional<TextSource> text;
    // A text style (text_style.h, PLAN §15), on a cells, pixels or text
    // layer only. A live text layer's is drawn into its cells with its text
    // (text.h); any other's is drawn by the compositor over its cells
    // (style_draw.h), which stay as they were painted.
    std::optional<TextStyle> style;
    // A reference layer's picture: only a reference layer has one, and one
    // without is empty (an older reader that dropped it).
    std::optional<ReferenceImage> reference;

    bool operator==(const LayerProperties&) const = default;
};

struct Layer {
    LayerProperties props;
    // The layer's own cells, its size (width, height); cell (0, 0) is at
    // props.x, props.y on the canvas. What layers/<id>.cells holds.
    Canvas cells{0, 0};

    int width() const { return cells.width(); }
    int height() const { return cells.height(); }
    // The canvas cells the layer covers, on the canvas or not.
    CellRect bounds() const { return {props.x, props.y, width(), height()}; }
    // The layer's cell at canvas cell (cx, cy); empty outside the layer.
    const Cell* cellAt(int cx, int cy) const;

    bool operator==(const Layer&) const = default;
};

// A transparent cell: what a new layer is made of.
Cell transparentCell();

// A layer of `kind`, width by height transparent cells, at (x, y). Its id is
// 0 until a stack adds it.
Layer makeLayer(LayerKind kind, int width, int height, std::string name = {}, int x = 0, int y = 0);

// A text or generator layer as a cells layer: the same cells (a live text
// layer's drawn text and style, a generator's cached output) and
// properties, kind cells, without a text source or style. A cells or pixels
// layer with a style becomes a cells layer of its styled cells (styledCells,
// with iCE colours `ice`), without the style (a pixel layer's in the normal
// mode, which it composited in whatever its own; it then covers text cells
// under it, as its pixels didn't); one whose style has no effect
// switched on only loses the style. Every other layer comes back as it is. The generator's files (scripts/<id>/ in a project) are left
// alone.
Layer bake(Layer layer, bool ice = true);

// The stroke options a tool editing this layer uses: `base` with the
// layer's alpha lock as the transparent lock, and for a pixel layer
// StrokeOptions::pixelLayer, so the half-block tools write its pixels with
// setLayerPixel and read the lock per pixel (layerPixelPaintable): a cell
// with one pixel set is a transparent cell whose set half is paintable.
StrokeOptions layerStrokeOptions(const Layer& layer, StrokeOptions base = {});
// Whether a tool may write the layer's cell at canvas cell (cx, cy): in the
// layer, the layer not locked nor a live text layer (its cells are drawn
// from its source), and under the alpha lock an opaque cell (a pixel layer:
// a cell with a pixel set).
bool layerPaintable(const Layer& layer, int cx, int cy);
// Whether a tool may write the pixel at canvas pixel (cx, py) of a pixel
// layer (cell row py / 2, the bottom half when py is odd): in the layer,
// not locked, and under the alpha lock a pixel that is set.
bool layerPixelPaintable(const Layer& layer, int cx, int py);

// ---- pixel layers ----------------------------------------------------------

// A pixel layer cell's two pixels, top and bottom; empty is transparent.
struct LayerPixels {
    std::optional<std::uint8_t> top, bottom;  // palette colours
    bool operator==(const LayerPixels&) const = default;
};
// The pixels a pixel layer's cell holds (see above). An opaque cell that
// isn't a pixel cell reads as its background in both halves, as halfblock.height
// reads it.
LayerPixels layerPixels(const Cell& cell);
// The cell a pixel layer stores for these pixels.
Cell pixelLayerCell(LayerPixels pixels);
// The cell with one half set to `colour`, or made transparent when it is
// empty; the other half is kept.
Cell setLayerPixel(const Cell& cell, bool bottom, std::optional<std::uint8_t> colour);
// A half pixel (`half`: a transparent cell holding one, as a copy of a pixel
// layer holds it) plotted over `under` as the half-block tools plot that
// pixel there. On a pixel layer (`pixelLayer`) it is setLayerPixel, the
// cells read with iCE on as the layer stores them. On a cells or text layer
// it is setHalfBlock with the document's iCE setting `ice`: `under` is read
// by the colours it shows (a blinking letter's bright background with iCE
// on), and with iCE off a bright colour that would share the cell with a
// different bright one is written dark, so no bright background is written
// (it would save as blink). Over a hole it stays the half pixel either way.
Cell halfPixelOver(const Cell& half, const Cell& under, bool pixelLayer, bool ice);
// A pixel layer's opaque cell (both halves set, as a copy of the layer
// holds it) written onto a cells or text layer: the whole cell, as the
// compositor shows the pixels with the document's iCE setting `ice`
// (halfBlockCell, which the half-block tools write with too). With iCE off
// two different bright colours can't share the cell, so the bottom one is
// written dark rather than as a bright background (it would save as
// blink). A copy turned a quarter (a ▌ or ▐ that holds no pixels) keeps its
// glyph, its background dark with iCE off: a pixel never blinks.
Cell pixelCellOnCells(const Cell& cell, bool ice);

// ---- edits -----------------------------------------------------------------

// A canvas-coordinate edit as the layer's own: each change moved by the
// layer's offset, those outside the layer dropped. Each change's before is
// the layer's own cell, whatever the canvas edit says (a tool builds its
// edit against the composite it shows), a cell the edit changes more than
// once is one change, to its last value, in the place of its first, and a
// change that leaves the layer's cell as it is is dropped, so undo restores
// the layer exactly. The tools work in canvas cells; the layer's undo
// command applies the result to Layer::cells.
CellEdit toLayerEdit(const Layer& layer, const CellEdit& canvasEdit);
// A tool's canvas-cell edit as the layer can hold it: toLayerEdit, with
// each cell written as the layer's kind stores it. In a cells or text layer
// every transparent cell is the plain transparentCell() (a transparent
// cell shows nothing, whatever glyph it carries), except one that holds a
// half pixel (pasted from a pixel layer): that is plotted over what the
// layers below show at its canvas cell, `below(x, y)` (the ground, a blank
// cell, when `below` is empty), with the document's iCE setting `ice`
// (halfPixelOver), an opaque cell. In a pixel layer a cell is stored as its
// pixels (pixelLayerCell of layerPixels): a pixel cell as halfblock.h reads
// it with iCE on, or a transparent cell; an edit that writes anything else
// there (a glyph, a shade, a half block turned on its side) can't be held,
// and comes back empty. Changes that then leave the layer's cell as it is
// are dropped, and so, on an alpha-locked layer, are changes that would
// clear an opaque cell or make a transparent one opaque (on a pixel layer:
// change which of its two pixels are set), whatever tool made them. Under
// the lock an edit that moves cells (CellEdit::moves: a selection moved,
// flipped or turned, its lift and stamp one edit) and would clear a cell
// comes back empty instead: the lock would keep what it lifts and drop
// what lands on clear cells, copying one glyph and losing another. A paste
// clears nothing, so it lands on the opaque cells and not on the clear ones.
// A wide pair the edit writes with one half off the layer or kept off by the
// lock is dropped whole when the edit is typed (CellEdit::typed); any other
// edit keeps the half that lands, as a space in its colours, as at the
// canvas edge (PLAN §16).
std::optional<CellEdit> fitLayerEdit(const Layer& layer, const CellEdit& canvasEdit,
                                     const std::function<Cell(int, int)>& below = {}, bool ice = true);
// The layer's cells as a canvas of width by height cells, each at its
// place on the canvas (its offset), and transparent cells where the layer
// has none: what the tools read when they edit the layer.
Canvas layerCanvas(const Layer& layer, int width, int height);
// A layer edit's changes in canvas cells (moved by the offset), for the
// region to recomposite.
CellEdit toCanvasEdit(const Layer& layer, const CellEdit& layerEdit);
// The canvas cells a layer edit changes, as one rectangle (not clipped to
// the canvas); empty for an empty edit. On a layer whose style the
// compositor draws (styledAtComposite) any change can restyle the whole
// layer, so a non-empty edit gives the layer's bounds.
CellRect dirtyRect(const Layer& layer, const CellEdit& layerEdit);

// ---- the stack -------------------------------------------------------------

// Bad stack operations: an id already in the stack, a layer the stack
// doesn't hold.
class LayerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The document's layers, bottom first, on a canvas of width by height cells.
class LayerStack {
public:
    LayerStack() : LayerStack(Canvas::kDefaultWidth, Canvas::kDefaultHeight) {}
    LayerStack(int width, int height);
    // One opaque cells layer holding the canvas, named "Background": how a
    // flat file (.ans) opens. Compositing it gives the canvas back.
    static LayerStack fromCanvas(const Canvas& canvas, std::string name = "Background");

    int width() const { return w_; }
    int height() const { return h_; }
    CellRect canvasRect() const { return {0, 0, w_, h_}; }
    // The canvas size; the layers keep their own size and offset.
    void resize(int width, int height);

    std::size_t size() const { return layers_.size(); }
    bool empty() const { return layers_.empty(); }
    // Bottom first.
    const std::vector<Layer>& layers() const { return layers_; }
    const Layer& operator[](std::size_t index) const { return layers_[index]; }
    Layer& operator[](std::size_t index) { return layers_[index]; }

    // Adds the layer at `index` (on top when empty, clamped to the top),
    // giving it a fresh id when its id is 0. Returns its id. Throws
    // LayerError for an id the stack already holds; an id from a file is
    // kept, and ids given later are above every id the stack has held.
    LayerId add(Layer layer, std::optional<std::size_t> index = {});
    // Removes the layer and returns it; empty if the stack doesn't hold it.
    // Its id is not given again.
    std::optional<Layer> remove(LayerId id);
    // Moves the layer to `index` (clamped to the top), the others keeping
    // their order. False if the stack doesn't hold it.
    bool move(LayerId id, std::size_t index);

    Layer* find(LayerId id);
    const Layer* find(LayerId id) const;
    std::optional<std::size_t> indexOf(LayerId id) const;
    // The id add() gives next; 0 once the last id (UINT32_MAX) has been
    // taken, and from then on add() refuses a layer without an id. It only
    // moves up: an id held or restored is always below it.
    LayerId nextId() const { return next_; }
    // Restores a saved nextId (the project file's), so ids removed before
    // the save aren't given again: moves nextId up to `next`, never down;
    // 0 marks the ids exhausted.
    void reserveIds(LayerId next);

    bool operator==(const LayerStack&) const = default;

private:
    int w_ = 0, h_ = 0;
    std::vector<Layer> layers_;
    LayerId next_ = 1;
};

} // namespace bw
