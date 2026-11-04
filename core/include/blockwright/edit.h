#pragma once
// Keyboard editing as cell deltas: every edit is a list of cells with their
// value before and after, so it can be applied (redo) and reverted (undo)
// without keeping a copy of the canvas. The GUI's undo stack wraps one
// CellEdit per command.

#include "blockwright/canvas.h"

#include <string_view>
#include <vector>

namespace bw {

struct CellChange {
    int x = 0, y = 0;
    Cell before, after;

    bool operator==(const CellChange&) const = default;
};

struct CellEdit {
    std::vector<CellChange> changes;
    // The cells are moved or copied (a paste, a stamp, a move, a flip or a
    // turn), not drawn, and each `after` already has the lost-blink mark it
    // keeps (writtenOver); settleMarks leaves them as they are.
    bool moves = false;
    // The cells are the ramp gradient's (rampGradient), and each `after`
    // already has the dither mark it should (Cell::dithered): settleMarks
    // settles their lost-blink marks only.
    bool dithers = false;

    bool empty() const { return changes.empty(); }
    bool operator==(const CellEdit&) const = default;
    // Writes each change's `after` (apply) or `before` (revert, in reverse
    // order). Changes outside the canvas are skipped.
    void apply(Canvas& canvas) const;
    void revert(Canvas& canvas) const;
};

// The cell `c` written over `under`, with the marks it keeps: the
// lost-blink mark (Cell::lostBlink, PLAN §4) `c` carries, which only a moved
// or copied cell has, or else `under`'s, when the save writes the two alike
// (writesAlike), since the save still writes that cell without its blink;
// and the dither mark (Cell::dithered) `c` carries, or else `under`'s, when
// the two look the same. A cell the same edit lifts off (a move, flip or
// turn) takes its marks with it, so what it leaves under is a blank.
// `underRight` and `cRight` are the glyphs right of each, so that a lead is
// judged as written ('?'); none (the default) judges it a single cell.
Cell writtenOver(const Cell& under, Cell c, Glyph underRight = {}, Glyph cRight = {});

// The marks `edit` leaves on `canvas`, the canvas it applies to (a layer's
// own cells in the editor), once settleWide has settled it. A drawn cell
// carries no mark of its own, whatever a brush built it from (a copy of the
// cell, of a neighbour, of a mirror image), and takes the marks of the cell
// it is drawn over by writtenOver; a cell changed twice is judged against
// what it was before the edit, and its right neighbour as the edit leaves it
// before and after (the canvas's where the edit doesn't change it), so a
// lead is judged as written. A pair's continuation then takes its lead's
// marks, and a change the marks leave as it was is dropped. An edit that
// moves cells (CellEdit::moves) is left as its builder made it, and the ramp
// gradient's (CellEdit::dithers) keeps the dither marks it set. Document::edit
// and cli shade settle every edit: settleWide, then settleMarks.
CellEdit settleMarks(const Canvas& canvas, CellEdit edit);

// The pairs `edit` leaves whole (PLAN §16, Wide and combining characters).
// It reads `edit` against `canvas`, the canvas it applies to (a layer's own
// cells in the editor), and looks at the cells it changes and their
// neighbours only, going by continuations, never by width:
// - a pair one half of which has a glyph written on it (a glyph, a clear)
//   keeps the other half as a space in that half's colours, whatever the
//   edit does to its colours; a new pair written over the old one, its
//   continuation in the new lead's values, stays whole;
// - a continuation in column 0 or right of another continuation is a stray
//   and becomes a space in its own colours;
// - a colour, attribute or mark (Cell::lostBlink, Cell::dithered) changed on
//   one half is changed on both, the lead's values winning when both halves
//   are written, and a new pair taking its lead's.
// A lone wide cell, one without a continuation, is an ordinary glyph to it.
// An edit that touches no continuation, on the canvas or in what it writes,
// is returned as it is. Document::edit and cli shade settle every edit.
CellEdit settleWide(const Canvas& canvas, CellEdit edit);

// The cell the keyboard writes to. `pastEnd` is set when a glyph was just
// typed on the last column: the cursor stays on that cell but stands past
// it, so Backspace clears that cell rather than the one to its left, and an
// input-method replacement counts back from the column after it. It means
// nothing off the last column, and any move clears it.
struct Cursor {
    int x = 0, y = 0;
    bool pastEnd = false;
    bool operator==(const Cursor&) const = default;
};

// The colours typing writes with: grey on black until a palette picks others.
struct Pen {
    Color fg = Color::indexed(Grey);
    Color bg = Color::indexed(Black);
};

// The cursor moved by (dx, dy), kept on the canvas.
Cursor moveCursor(const Canvas& canvas, Cursor c, int dx, int dy);

// Whether typing `ch` can write it: printable (not a C0 control or DEL) and
// saved to .ans as itself, not as '?' (see writeAnsi).
bool isTypeable(char32_t ch);

struct EditResult {
    CellEdit edit;   // empty when nothing changes
    Cursor cursor;   // where the cursor goes next
};

// Writes `ch` in the pen's colours at the cursor and moves one cell right
// (staying put on the last column, past the glyph). A glyph that isn't
// typeable changes nothing.
EditResult typeGlyph(const Canvas& canvas, Cursor c, char32_t ch, const Pen& pen);

// Replaces `length` cells along the cursor's row, starting `start` cells from
// the cursor, with `text` in the pen's colours, the way a text field takes an
// input-method commit. The range is clamped to the row. The text is written
// from the range's start as if typed (anything past the last column lands on
// it), replaced cells it doesn't reach are cleared, and the cursor ends after
// the text (past the last column if the text reached it). All or nothing: if any character isn't typeable, nothing changes
// and the cursor stays.
//
// A cleared cell becomes `blank`: a blank cell on the bottom layer, a
// transparent cell above it (Document::blankCell), so the layer beneath
// shows through.
EditResult replaceText(const Canvas& canvas, Cursor c, int start, int length, std::u32string_view text,
                       const Pen& pen, const Cell& blank = Cell{});

// Moves one cell left and clears that cell to `blank`; nothing on the first
// column. A cursor past the end clears the last column and stays there.
EditResult backspace(const Canvas& canvas, Cursor c, const Cell& blank = Cell{});

// Clears the cell under the cursor to `blank`; the cursor stays.
EditResult deleteCell(const Canvas& canvas, Cursor c, const Cell& blank = Cell{});

} // namespace bw
