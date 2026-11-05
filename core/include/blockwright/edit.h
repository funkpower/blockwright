#pragma once
// Keyboard editing as cell deltas: every edit is a list of cells with their
// value before and after, so it can be applied (redo) and reverted (undo)
// without keeping a copy of the canvas. The GUI's undo stack wraps one
// CellEdit per command.

#include "blockwright/ansi.h"
#include "blockwright/canvas.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
    // The cells are typed (typeCluster, typeMarks, replaceText, backspace,
    // deleteCell), so a pair it writes is new and fitted whole or not at all
    // (fitLayerEdit), and a lead it writes is written though its glyph is
    // left, as when a narrow cluster is typed over its own base (settleWide;
    // PLAN §16, Cursor and typing).
    bool typed = false;

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
//   continuation in the new lead's values, stays whole; a lead a typed edit
//   (CellEdit::typed) writes is its typed cell, even with the glyph it had;
// - a continuation in column 0 or right of another continuation is a stray
//   and becomes a space in its own colours;
// - a colour, attribute or mark (Cell::lostBlink, Cell::dithered) changed on
//   one half is changed on both, the lead's values winning when both halves
//   are written, and a new pair taking its lead's.
// A lone wide cell, one without a continuation, is an ordinary glyph to it.
// An edit that touches no continuation, on the canvas or in what it writes,
// is returned as it is. Document::edit and cli shade settle every edit.
CellEdit settleWide(const Canvas& canvas, CellEdit edit);

// The cells `edit` leaves: its last change to each, and where it leaves a
// cell out, the cell under it (`under`, null where that isn't known, as off
// the canvas). Which pair a changed cell belongs to is asked of this, never
// of the edit alone, so a half the edit leaves out because it holds what it
// held already (the lead of a pair typed over a lone wide glyph, a
// continuation typed over in the same colours) is still that pair's half
// (PLAN §16).
class EditedCells {
public:
    using Under = std::function<const Cell*(int x, int y)>;
    EditedCells(const CellEdit& edit, Under under);
    // Over `canvas`, null off it.
    EditedCells(const Canvas& canvas, const CellEdit& edit);

    const Cell* at(int x, int y) const;
    // The lead's column of the pair (x, y) is either half of: a cell other
    // than a continuation with one right of it.
    std::optional<int> pairLead(int x, int y) const;

private:
    std::map<std::pair<int, int>, Cell> cells_;  // (y, x)
    Under under_;
};

// The cell the keyboard writes to. It is never a continuation: arrows,
// clicks and typing put it on the pair's lead (PLAN §16, Cursor and typing).
// `pastEnd` is set when a glyph was just typed at the end of the row, on the
// last column or as a pair whose continuation is the last column: the cursor
// stays on that cell (the lead) but stands past it, so Backspace clears that
// cell rather than the one to its left, a lone mark joins it, and an
// input-method replacement counts back from the column after it. It means
// nothing anywhere else (atRowEnd), and any move clears it.
struct Cursor {
    int x = 0, y = 0;
    bool pastEnd = false;
    bool operator==(const Cursor&) const = default;
};

// The colours typing writes with: grey on black until a palette picks others.
// `attrs` is the formatting (attr::Bold, Italic, Underline) the glyph brush
// writes under the IRC target (#350); typing doesn't write it yet.
struct Pen {
    Color fg = Color::indexed(Grey);
    Color bg = Color::indexed(Black);
    std::uint8_t attrs = 0;
};

// The cursor moved by (dx, dy), kept on the canvas and off a continuation:
// a step right from a lead goes past its continuation, any other move that
// lands on one goes to its lead. A move of (0, 0) only puts the cursor on
// the canvas and the lead.
Cursor moveCursor(const Canvas& canvas, Cursor c, int dx, int dy);

// Whether `c` is where past the end means something: on the last column,
// or on a lead whose continuation is the last column.
bool atRowEnd(const Canvas& canvas, Cursor c);

// Whether typing `ch` into a CP437 document can write it: printable (not a
// C0 control or DEL) and saved to .ans as itself, not as '?' (see
// writeAnsi).
bool isTypeable(char32_t ch);

// Whether typing `cluster`, a base and the zero-width code points after it,
// NFC-normalised, can write it into a document in `encoding` (PLAN §16): in
// CP437 a single code point isTypeable takes; in UTF-8 at most
// kMaxClusterCodePoints code points, none a control (C0, DEL, C1, U+2028,
// U+2029) or anything else utf8Writable refuses. A cluster whose base is
// zero-width is a lone mark, which typeMarks joins to a cluster instead.
bool isTypeable(std::u32string_view cluster, TextEncoding encoding);

// `text` split into clusters (PLAN §16, Width): each code point
// codePointWidth gives a width other than 0 starts one, and a zero-width one
// joins the one before; one with nothing before it, or after a control,
// starts a cluster of its own (a lone mark, which the zero-width code points
// after it join). A cluster may come out longer than
// kMaxClusterCodePoints, which isTypeable refuses.
std::vector<std::u32string> splitClusters(std::u32string_view text);

// What typing is allowed to write and how far it may fill the cluster pool.
struct TypingOptions {
    TextEncoding encoding = TextEncoding::Cp437;  // Document::textEncoding
    // The pool's cap for a cluster typed (ClusterPool::kLossyCap; lowered by
    // tests): past it a cluster is typed as its base, and the result's
    // notice counts it.
    std::size_t poolCap = ClusterPool::kLossyCap;
};

struct EditResult {
    CellEdit edit;   // empty when nothing changes
    Cursor cursor;   // where the cursor goes next
    // A cluster refused for the document's encoding (isTypeable), for the
    // status bar to name (rejectedGlyphsMessage); nothing changes then.
    std::u32string rejected;
    // Anything else typing refused or dropped, for the status bar ("中
    // doesn't fit on the last column; not typed").
    std::string notice;
};

// Writes `cluster` (NFC-normalised, as splitClusters gives one) in the
// pen's colours at the cursor and moves past it. A wide one (its base two
// columns, codePointWidth) is written as a pair, its lead at the cursor and
// its continuation in the same colours right of it, and moves two cells,
// staying on the lead with `pastEnd` when the continuation is the last
// column; on the last column it is refused with a notice. A narrow one
// moves one cell, staying put on the last column, past it. A cluster the
// encoding doesn't take changes nothing and is the result's `rejected`; a
// lone mark (a zero-width base) goes to typeMarks.
EditResult typeCluster(const Canvas& canvas, Cursor c, std::u32string_view cluster, const Pen& pen,
                       const TypingOptions& opts = {});
// typeCluster of one code point in a CP437 document (the F-key glyph sets).
EditResult typeGlyph(const Canvas& canvas, Cursor c, char32_t ch, const Pen& pen);

// Joins `marks`, zero-width code points typed with nothing to compose them,
// to the cluster that ends just left of the cursor on its row (the pair's
// lead when that cell is a continuation), or with `pastEnd` the one under
// the cursor, and NFC-normalises the result, keeping the cell's colours;
// the cursor stays. Refused with a notice at column 0 and when the cluster
// would hold more than kMaxClusterCodePoints code points; a result the
// encoding doesn't take is `rejected`.
EditResult typeMarks(const Canvas& canvas, Cursor c, std::u32string_view marks, const TypingOptions& opts = {});

// Replaces `length` clusters along the cursor's row, starting `start`
// clusters from the cursor (a pair counts as one), with `text` in the pen's
// colours, the way a text field takes an input-method commit. The range is
// clamped to the row. The text, split into clusters, is written from the
// range's start as if typed (anything past the end lands on the row's last
// cluster, a pair's lead when a pair ends the row; a wide cluster that would
// start on the last column is dropped and counted in the notice), replaced
// cells it doesn't reach are cleared, and the cursor ends after the text
// (past the end if the text reached it), or stays where nothing is written
// or cleared. All or nothing: if any cluster isn't typeable, or the text
// starts with a mark and has more after it, nothing changes and the cursor
// stays.
//
// A cleared cell becomes `blank`: a blank cell on the bottom layer, a
// transparent cell above it (Document::blankCell), so the layer beneath
// shows through.
EditResult replaceText(const Canvas& canvas, Cursor c, int start, int length, std::u32string_view text,
                       const Pen& pen, const Cell& blank = Cell{}, const TypingOptions& opts = {});

// A replacement range as an input method gives it (QInputMethodEvent's
// replacementStart and replacementLength: UTF-16 code units from the
// cursor) as replaceText counts it, in clusters (PLAN §16, Cursor and
// typing). Each cluster on the cursor's row counts the UTF-16 units of its
// code points, a pair being one cluster, and each column off the row one; a
// cursor past the end counts from after the cluster under it, as
// replaceText does. A range that starts or ends inside a cluster (in a
// surrogate pair, or among a cluster's marks) takes all of that cluster.
struct ClusterRange {
    int start = 0;
    int length = 0;
    bool operator==(const ClusterRange&) const = default;
};
ClusterRange clusterRange(const Canvas& canvas, Cursor c, int utf16Start, int utf16Length);

// Moves one cluster left and clears it to `blank`, both cells of a pair;
// nothing on the first column. A cursor past the end clears the cluster
// under it and stays there.
EditResult backspace(const Canvas& canvas, Cursor c, const Cell& blank = Cell{});

// Clears the cluster under the cursor to `blank`, both cells of a pair; the
// cursor stays.
EditResult deleteCell(const Canvas& canvas, Cursor c, const Cell& blank = Cell{});

} // namespace bw
