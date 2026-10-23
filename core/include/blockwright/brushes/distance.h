#pragma once
// How deep each cell of a shape sits: the distance to the nearest cell off
// it, for the form brush's arc and the soft brush's falloff (PLAN §10,
// "Shading").

#include "blockwright/brush.h"

#include <vector>

namespace bw {

// Each cell's exact Euclidean distance, in columns with a row counted as
// two (a cell is 8×16), to the nearest cell not in the shape, cells beyond
// the mask's edge included; 0 off the shape. Row-major, width × height.
// Linear in the shape's bounding box (Felzenszwalb and Huttenlocher's two
// passes of lower envelopes of parabolas).
std::vector<double> edgeDistance(const Mask& shape);

} // namespace bw
