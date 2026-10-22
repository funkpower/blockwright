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
// writes.
struct CellBlock {
    int width = 0, height = 0;
    std::vector<Cell> cells;  // row-major

    CellBlock() = default;
    CellBlock(int w, int h) : width(w), height(h), cells(static_cast<size_t>(w) * h) {}

    bool empty() const { return width <= 0 || height <= 0; }
    Cell& at(int x, int y) { return cells[static_cast<size_t>(y) * width + x]; }
    const Cell& at(int x, int y) const { return cells[static_cast<size_t>(y) * width + x]; }

    bool operator==(const CellBlock&) const = default;
};

// A blank cell: a hole, or a space (also NUL or CP437 0xFF) on a black
// background without blink, which iCE would show as a bright background.
bool isBlank(const Cell& c);

// The selected cells, clipped to the canvas. Empty if nothing is selected.
CellBlock copyBlock(const Canvas& canvas, const Selection& sel);
// Clears the selected cells to blank (Delete, and the second half of Cut).
CellEdit clearSelection(const Canvas& canvas, const Selection& sel);

enum class PasteMode {
    Normal,       // every cell of the block is written
    Transparent,  // blank cells of the block leave the canvas as it is
    Under,        // only blank cells of the canvas are written
};

// What a paste leaves in a canvas cell that holds `under`: the block's
// `cell`, or nothing to keep `under`. Holes never write.
std::optional<Cell> pasteCell(const Cell& cell, const Cell& under, PasteMode mode);

// Pastes `block` with its top-left at (x, y); the part off the canvas is
// dropped.
CellEdit pasteBlock(const Canvas& canvas, const CellBlock& block, int x, int y, PasteMode mode);

// Stamps a floating block at (x, y). With `liftedFrom`, the block was
// lifted off those cells (a move): they are cleared first, and the clear and
// the paste are one edit.
CellEdit stampBlock(const Canvas& canvas, const CellBlock& block, int x, int y, PasteMode mode,
                    const Selection* liftedFrom = nullptr);

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

enum class Transform { FlipHorizontal, FlipVertical, RotateClockwise, RotateCounterClockwise };

// The block flipped or turned 90 degrees, each cell's glyph remapped.
// Rotation swaps width and height. Cells are twice as tall as they are wide,
// so a rotated block looks stretched; the glyph remap is about which way a
// glyph faces, not its shape.
CellBlock transformed(const CellBlock& block, Transform t);

// An edit and where the selection is after it.
struct SelectionEdit {
    CellEdit edit;
    Selection selection;
};

// Transforms the selected cells in place. A flip keeps the rectangle; a
// rotation keeps its top-left corner and swaps its sides, clearing the cells
// of the old rectangle the new one doesn't cover. Cells turned past the
// canvas edge are dropped, and the selection is clipped to match.
SelectionEdit transformSelection(const Canvas& canvas, const Selection& sel, Transform t);

// Moves the selected cells by (dx, dy), leaving blank cells behind, pasting
// them in `mode`. The selection moves with them, clipped to the canvas.
SelectionEdit moveSelection(const Canvas& canvas, const Selection& sel, int dx, int dy,
                            PasteMode mode = PasteMode::Normal);

// Mirror mode (PLAN §3): a transform of an edit, not a re-draw. Every change
// at column x is also made at width - 1 - x with its glyph flipped
// horizontally. Where the edit itself changes the mirrored cell (the centre
// column of an odd width, or an edit spanning both halves) the edit's own
// change wins. Integer columns only; no rounding.
int mirrorColumn(int width, int x);
CellEdit mirrorEdit(const Canvas& canvas, const CellEdit& edit);

} // namespace bw
