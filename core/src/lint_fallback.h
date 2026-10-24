// Fallback specks (#71), for checkLadder and LadderIndex: the rule reads
// half blocks against the shading round them, so it lives apart from the
// ladder's rungs.
#pragma once

#include "blockwright/lint.h"

#include <vector>

namespace bw::lint::fallback {

// Whether the cell at x, y (inside `box`) is a fallback speck, reading the
// cells of its run along the row and the run's ends; `reads` counts them.
bool speckAt(const Canvas& canvas, const Ramp& ramp, const Box& box, int x, int y, bool ice, long long& reads);
// The same for every cell of row y in the box at once, out[x - box.x0]
// (1 for a speck), each run read once: O(W).
void row(const Canvas& canvas, const Ramp& ramp, const Box& box, int y, bool ice, std::vector<char>& out,
                 long long& reads);
LadderFinding finding(int x, int y);

} // namespace bw::lint::fallback
