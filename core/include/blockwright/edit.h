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

    bool empty() const { return changes.empty(); }
    // Writes each change's `after` (apply) or `before` (revert, in reverse
    // order). Changes outside the canvas are skipped.
    void apply(Canvas& canvas) const;
    void revert(Canvas& canvas) const;
};

// The cell the keyboard writes to.
struct Cursor {
    int x = 0, y = 0;
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
// (staying put on the last column). A glyph that isn't typeable changes
// nothing.
EditResult typeGlyph(const Canvas& canvas, Cursor c, char32_t ch, const Pen& pen);

// Replaces `length` cells along the cursor's row, starting `start` cells from
// the cursor, with `text` in the pen's colours, the way a text field takes an
// input-method commit. The range is clamped to the row. The text is written
// from the range's start as if typed (anything past the last column lands on
// it), replaced cells it doesn't reach are cleared, and the cursor ends after
// the text. All or nothing: if any character isn't typeable, nothing changes
// and the cursor stays.
EditResult replaceText(const Canvas& canvas, Cursor c, int start, int length, std::u32string_view text,
                       const Pen& pen);

// Moves one cell left and clears that cell; nothing on the first column.
EditResult backspace(const Canvas& canvas, Cursor c);

// Clears the cell under the cursor; the cursor stays.
EditResult deleteCell(const Canvas& canvas, Cursor c);

} // namespace bw
