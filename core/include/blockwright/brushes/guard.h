#pragma once
// The ladder lint guard the shading brushes keep over a stroke (PLAN §10,
// "Shading"): what a dab would add to the canvas as the stroke began, read
// a change at a time through one lint::LadderIndex.

#include "blockwright/canvas.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"

#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace bw {

// One stroke's lint guard. It holds the canvas as the dabs it kept have
// left it, and judges a dab by the findings the canvas would then have that
// the stroke's start didn't, as the lint judges a change (LadderIndex, the
// maintainer's rule in PR #59): new by kind and place, a wide rung new
// unless the start had one the same way, on the same line, of the same
// rung, sharing a cell with it, a skip new unless the start had one it
// matches (LadderIndex::Skip::matches: the same way, on the same line,
// between the same two cells, or between the same two rungs with each
// level sharing a cell); one glyph when the start didn't use one
// glyph only; noise when it is over the limit and higher than the start's.
// So the canvas after any dab the guard keeps adds nothing to the stroke's
// start, the whole-canvas findings (one glyph and noise) included.
//
// How a brush uses it: take one at the stroke's first dab, from
// Stroke::canvas(), and keep it in Stroke::brushState(), so it lives and
// dies with the stroke and is never carried over an undo, a cancel or an
// edit made between strokes (each stroke reads the canvas afresh: a
// whole-canvas pass, once per stroke). Every change the brush makes to the
// stroke goes through it: judge() what the dab would leave, then keep() it
// or judge another try; what isn't kept is never written to the stroke, so
// the preview and the committed edit (both the stroke's cells) are what the
// guard holds. A judge costs the rows and columns through the cells it
// names (O(W + H) a cell), however long the stroke and large the canvas.
class StrokeLint {
public:
    StrokeLint(const Canvas& start, const Ramp& ramp);

    // The canvas as the kept dabs left it.
    const Canvas& canvas() const { return kept_; }
    // What `after`, which differs from canvas() only at `changed` cells,
    // adds to the stroke's start. The guard stays at canvas() until keep().
    std::vector<lint::LadderFinding> judge(const Canvas& after, const std::vector<std::pair<int, int>>& changed);
    // Keeps the canvas the last judge() read.
    void keep();

private:
    void revert();
    bool atStart(const lint::LadderFinding& f, const Canvas& now) const;

    Ramp ramp_;
    lint::LadderIndex index_;
    Canvas kept_;
    // the last judge's cells, as it read them, until kept or reverted
    std::vector<std::pair<int, int>> pending_;
    std::vector<Cell> pendingCells_;
    // the start's findings: by place, wide rungs by (across, line, rung) as
    // runs along the line, skips by line, and its whole-canvas readings
    std::set<lint::LadderFinding::Place> places_;
    std::map<std::tuple<bool, int, int>, std::vector<std::pair<int, int>>> wide_;
    std::vector<char> rowWide_, columnWide_;
    std::vector<std::vector<lint::LadderIndex::Skip>> rowSkips_, columnSkips_;
    bool oneGlyph_ = false;
    double noise_ = 0.0;
};

} // namespace bw
