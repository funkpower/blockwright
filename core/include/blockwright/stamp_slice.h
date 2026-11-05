#pragma once
// Resizable Library stamps (#345 slice 2, PLAN §12): a stamp whose sidecar
// carries slice insets is cut as a frame style is (frame.h) and drawn to any
// size with drawFrame's cut (frameSourceIndex). The corners stay as they are and the edges and the
// middle repeat, anchored at the top-left corner, so a frame stamp grows
// without stretching its border and a band stamp (a fence, a strip of
// grass) grows along its length. The middle is always drawn: a frame
// stamp's open middle is blank cells, which transparent paste skips.
//
// A slice resizes in width, in height or in both. A sidecar holds both
// ("insets": [l, r, t, b]) or width only ("insets": [l, r], a band); a
// rotation turns a band's width-only slice into a height-only one, which
// lives only as long as the float it belongs to.
#include "blockwright/selection.h"

#include <optional>
#include <string>
#include <vector>

namespace bw {

struct StampSlice {
    // The corners' size, as FrameStyle's insets. Top and bottom are 0 when
    // the slice doesn't resize in height, left and right when it doesn't in
    // width.
    int left = 0, right = 0, top = 0, bottom = 0;
    bool width = true;   // resizes in width
    bool height = true;  // resizes in height

    bool operator==(const StampSlice&) const = default;
};

// Why `slice` can't cut `block`, in English, or empty: a negative inset,
// an inset on an axis it doesn't resize in, neither axis, insets that leave
// no column (or row) between them to repeat, or a block holding a wide
// character, whose pair a repeat could cut in two. Checked without
// overflow whatever the insets hold.
std::string sliceProblem(const CellBlock& block, const StampSlice& slice);

// The smallest and largest size the block resizes to: the corners (at least
// one cell) up to kMaxColumns x kMaxRows on an axis it resizes in, its own
// size on one it doesn't.
int sliceMinimumWidth(const CellBlock& block, const StampSlice& slice);
int sliceMinimumHeight(const CellBlock& block, const StampSlice& slice);
int sliceMaximumWidth(const CellBlock& block, const StampSlice& slice);
int sliceMaximumHeight(const CellBlock& block, const StampSlice& slice);

// `block` cut width x height as drawFrame cuts: the corners as they are,
// the edges and the middle repeated from the top-left. Nothing when
// sliceProblem has one or the size is outside the limits above (checked
// before anything is allocated). The block may be as large as a stamp
// (kMaxColumns x kMaxRows), past a frame style's source limit. The result
// keeps the block's pixelLayer.
std::optional<CellBlock> resizeStamp(const CellBlock& block, const StampSlice& slice, int width, int height);

// `block` cut through `slice`, then turned by `turns` in order, to width x
// height as it shows after them: the limits above apply to that size (a
// band on its side is up to kMaxRows long), though the cut made before the
// turns may be past a canvas's on an axis. Neither side may be past a
// canvas's, the one that doesn't resize included: a band longer than
// kMaxColumns turned on its side turns, as any float does, but doesn't
// resize. Each turn is applied as the float
// applied it, so a glyph's turned form is the one the float showed.
std::optional<CellBlock> resizeTurnedStamp(const CellBlock& block, const StampSlice& slice,
                                           const std::vector<Transform>& turns, int width, int height);

// Adds `t` to a float's turns and replaces them with the shortest list
// that replays the same: the same orientation of the cells and the same
// turn of every glyph. There are sixteen such, so the list is never longer
// than kMaxTurns however many turns the float took. The reduction uses
// only the relations every CP437 glyph keeps (test_stamp_slice.cpp checks
// all 256, and every list followed by every turn), so replaying the short
// list gives the cells the long one would.
void addTurn(std::vector<Transform>& turns, Transform t);

// The most turns addTurn leaves in a list.
inline constexpr std::size_t kMaxTurns = 4;

// The slice of transformed(block, t): a flip swaps the insets across it,
// a rotation turns them and the axes with the block.
StampSlice transformedSlice(const StampSlice& slice, Transform t);

} // namespace bw
