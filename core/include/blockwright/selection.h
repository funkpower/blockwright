#pragma once
// Selections and block transforms (PLAN §4): a selected rectangle of cells;
// copy, cut and paste with three paste modes; move, flip and rotate, with
// the glyphs that face one way remapped so the block still reads; and mirror
// mode as a transform of any edit. Everything that changes the canvas is a
// pure function returning one CellEdit, which the GUI pushes as one undo step.

#include "blockwright/canvas.h"
#include "blockwright/edit.h"

#include <optional>
#include <vector>

namespace bw {

// A rectangle of cells: (x, y) is the top-left cell. Empty when either side
// is 0 or less.
struct CellRect {
    int x = 0, y = 0, width = 0, height = 0;

    // The rectangle with these two corner cells (inclusive, in any order).
    static CellRect fromCorners(int x0, int y0, int x1, int y1);

    bool empty() const { return width <= 0 || height <= 0; }
    bool contains(int cx, int cy) const {
        return !empty() && cx >= x && cy >= y && cx - x < width && cy - y < height;
    }
    // The part inside `o`; empty if they don't meet.
    CellRect intersected(const CellRect& o) const;

    bool operator==(const CellRect&) const = default;
};

// What is selected. Only a rectangle for now. A mask (lasso, magic wand)
// can come later behind contains(): the operations below ask only it and
// bounds(), and copyBlock turns cells outside a mask into holes.
class Selection {
public:
    Selection() = default;
    explicit Selection(const CellRect& rect) : rect_(rect) {}

    const CellRect& bounds() const { return rect_; }
    bool empty() const { return rect_.empty(); }
    bool contains(int x, int y) const { return rect_.contains(x, y); }
    // The selection cut down to the canvas.
    Selection clipped(const Canvas& canvas) const;

    bool operator==(const Selection&) const = default;

private:
    CellRect rect_;
};

// A block of cells lifted off a canvas: the clipboard and the floating
// selection. A cell with its transparent flag set is a hole, which no paste
// writes. A block lifted off a pixel layer (`pixelLayer`) holds its cells as
// the layer stores them: half pixels, and pixel cells read with iCE on.
struct CellBlock {
    int width = 0, height = 0;
    std::vector<Cell> cells;  // row-major
    bool pixelLayer = false;

    CellBlock() = default;
    CellBlock(int w, int h) : width(w), height(h), cells(static_cast<size_t>(w) * h) {}

    bool empty() const { return width <= 0 || height <= 0; }
    Cell& at(int x, int y) { return cells[static_cast<size_t>(y) * width + x]; }
    const Cell& at(int x, int y) const { return cells[static_cast<size_t>(y) * width + x]; }

    bool operator==(const CellBlock&) const = default;
};

// A blank cell: a hole, or a space (also NUL or CP437 0xFF) on a black
// background without blink, which iCE would show as a bright background,
// and without underline, whose line shows (attr::showsOnBlank).
bool isBlank(const Cell& c);

// The selected cells, clipped to the canvas. Empty if nothing is selected.
// Cells outside a mask are holes (transparentCell()).
CellBlock copyBlock(const Canvas& canvas, const Selection& sel);
// Clears the selected cells to `blank` (Delete, and the second half of
// Cut): a blank cell on the bottom layer, a transparent cell above it
// (Document::blankCell), so the layer beneath shows through.
CellEdit clearSelection(const Canvas& canvas, const Selection& sel, const Cell& blank = Cell{});

enum class PasteMode {
    Normal,       // every cell of the block is written
    Transparent,  // blank cells of the block leave the canvas as it is
    Under,        // only blank cells of the canvas are written
};

// How a paste writes a half pixel over the cell under it (halfPixelOver in
// layer.h): as the half-block tools plot a pixel on the layer it lands on,
// a pixel layer (`pixelLayer`) or a cells or text layer, read and written
// by the colours it shows with iCE colours `ice` (the document's setting).
struct PastePixels {
    bool pixelLayer = false;
    bool ice = true;
};

// What a paste leaves in a canvas cell that holds `under`: the block's
// `cell`, or nothing to keep `under`. Holes never write. A transparent cell
// that holds a pixel (a pixel layer's cell with one half set, layer.h) is
// not a hole: Normal and Under write it, so a move on a pixel layer keeps
// its half pixels. It is plotted into the cell under it as the half-block
// tools plot that pixel there (halfPixelOver, `pixels`): its clear half
// keeps a pixel cell's other half (a red top pixel over blue/blue is red
// over blue) or any other opaque cell's background as it shows (over a
// letter on black it is red over black), and over a hole it stays a half
// pixel (on a cells layer fitLayerEdit puts it over the layers below).
// Transparent mode takes it as blank and leaves the cell under it as it is.
// An opaque `cell` from a pixel layer's block (`pixelCell`, CellBlock::
// pixelLayer) is written whole onto a cells or text layer as the pixel
// layer shows it with the document's iCE setting (pixelCellOnCells), so
// with iCE off no bright background is written; onto a pixel layer it is
// written as it is.
std::optional<Cell> pasteCell(const Cell& cell, const Cell& under, PasteMode mode, PastePixels pixels = {},
                              bool pixelCell = false);

// Pastes `block` with its top-left at (x, y); the part off the canvas is
// dropped.
CellEdit pasteBlock(const Canvas& canvas, const CellBlock& block, int x, int y, PasteMode mode,
                    PastePixels pixels = {});

// Stamps a floating block at (x, y). With `liftedFrom`, the block was
// lifted off those cells (a move): they are cleared to `blank` first, and
// the clear and the paste are one edit.
CellEdit stampBlock(const Canvas& canvas, const CellBlock& block, int x, int y, PasteMode mode,
                    const Selection* liftedFrom = nullptr, const Cell& blank = Cell{}, PastePixels pixels = {});

// Glyph remaps. A glyph that faces one way is swapped for its mirror image
// or turned copy where CP437 has one: half blocks, box-drawing corners, tees
// and lines, slashes, brackets, arrows and triangles. Anything else (letters,
// shades, full block) is kept. For every glyph two flips each undo
// themselves, and a half turn (two quarter turns either way) is both flips;
// four quarter turns give the glyph back. A bracket has no turned glyph, so
// a quarter turn shows another pair (( → [ → ) → ]) and a half turn swaps it.
enum class Flip { Horizontal, Vertical };
char32_t flipGlyph(char32_t ch, Flip flip);
char32_t rotateGlyph(char32_t ch, bool clockwise);
// The same for a cell's glyph: a cluster and the continuation have no mirror
// or turned form and are kept.
Glyph flipGlyph(Glyph g, Flip flip);
Glyph rotateGlyph(Glyph g, bool clockwise);

enum class Transform { FlipHorizontal, FlipVertical, RotateClockwise, RotateCounterClockwise };

// The block flipped or turned 90 degrees, each cell's glyph remapped.
// Rotation swaps width and height. Cells are twice as tall as they are wide,
// so a rotated block looks stretched; the glyph remap is about which way a
// glyph faces, not its shape. Wide pairs (PLAN §16): a horizontal flip
// puts a pair back as lead then continuation, a vertical flip keeps it
// whole, and a rotation of a block holding either half of one returns the
// block as it is (blockHoldsPair; the editor refuses it with a message).
CellBlock transformed(const CellBlock& block, Transform t);

// An edit and where the selection is after it.
struct SelectionEdit {
    CellEdit edit;
    Selection selection;
};

// Transforms the selected cells in place. A flip keeps the rectangle; a
// rotation keeps its top-left corner and swaps its sides, clearing the cells
// of the old rectangle the new one doesn't cover. Cells turned past the
// canvas edge are dropped, and the selection is clipped to match. Cleared
// cells become `blank`. A rotation of a selection holding either half of a
// wide pair (selectionHoldsPair) does nothing: an empty edit and the same
// selection.
SelectionEdit transformSelection(const Canvas& canvas, const Selection& sel, Transform t,
                                 const Cell& blank = Cell{});

// Moves the selected cells by (dx, dy), leaving `blank` cells behind,
// pasting them in `mode`. The selection moves with them, clipped to the
// canvas.
SelectionEdit moveSelection(const Canvas& canvas, const Selection& sel, int dx, int dy,
                            PasteMode mode = PasteMode::Normal, const Cell& blank = Cell{});

// Mirror mode (PLAN §3): a transform of an edit, not a re-draw. Every change
// at column x is also made at width - 1 - x with its glyph flipped
// horizontally. Where the edit itself changes the mirrored cell (the centre
// column of an odd width, or an edit spanning both halves) the edit's own
// change wins. Integer columns only; no rounding.
//
// Each cell the edit changes is mirrored with the pair it is a half of once
// the edit lands (EditedCells over the settled edit and the canvas, edit.h,
// so a half the edit leaves out as unchanged still counts), or alone. What
// comes back is unsettled: the edit's own changes and the images of those
// pairs and cells, not of the cells the settle would add, and only they
// keep an image off a cell, so the caller settles once after fitting the
// edit to the layer (Document::edit), and a pair the fitting refuses takes
// no cleanup with it. A wide pair at
// (x, x + 1) is mirrored whole, its lead at mirrorColumn(x + 1) and its continuation
// at mirrorColumn(x), as a pair reads left to right (PLAN §16). On an even
// width a pair across the axis is its own image. On an odd width a pair
// over the centre column overlaps its own image and gets no copy;
// `pairsUnmirrored`, when given, is set to how many did.
int mirrorColumn(int width, int x);
CellEdit mirrorEdit(const Canvas& canvas, const CellEdit& edit, int* pairsUnmirrored = nullptr);
// Whether the selected cells hold either half of a wide pair (a lead with
// its continuation beside it, or that continuation): what a rotation
// refuses. A lone wide cell is no pair.
bool selectionHoldsPair(const Canvas& canvas, const Selection& sel);
// The same for a block's cells.
bool blockHoldsPair(const CellBlock& block);

} // namespace bw
