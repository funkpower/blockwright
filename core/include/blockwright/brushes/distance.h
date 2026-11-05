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
// passes of lower envelopes of parabolas). With `steps`, adds its work to
// it: each cell of the mask and of the padded box it reads, and each step
// of an envelope (a site looked at, a parabola dropped, an output cell, a
// move to the next parabola), at most 4 a cell of the box per pass.
// `steps` only in builds with the tests (BW_TEST_HOOKS).
#ifdef BW_TEST_HOOKS
std::vector<double> edgeDistance(const Mask& shape, long long* steps = nullptr);
#else
std::vector<double> edgeDistance(const Mask& shape);
#endif

} // namespace bw
