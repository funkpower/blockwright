#pragma once
// Shape rasterisers (PLAN §3, §4 tools): lines, rectangles and ellipses as
// the pixels they cover, in any grid. The GUI draws them in half-block
// pixels (blockwright/halfblock.h), a canvas `width` wide and twice its
// rows high. Each takes the two corners of a drag in either order and
// returns only the pixels inside 0..width-1, 0..height-1 (or a clip
// rectangle), row-major for the rectangle and ellipse, without visiting the
// rest of a large shape.

#include "blockwright/brush.h"

#include <utility>
#include <vector>

namespace bw {

using Pixels = std::vector<std::pair<int, int>>;

// A rectangle of pixels, corners inclusive; empty when x0 > x1 or y0 > y1.
struct PixelRect {
    long long x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool operator==(const PixelRect&) const = default;
};

// The pixels whose images under `symmetry` (symmetryPixels) can land on a
// canvas `width` cells wide and `height` rows high: the canvas's own
// pixels, and for a radial symmetry the box round the canvas turned back
// by each fold, a pixel wider each way for the rounding. A shape tool
// rasterises this much of a shape, so images of its part off the canvas
// still land (PixelPainter clips each image on its own). Mirrors map the
// canvas onto itself.
PixelRect symmetryReach(const Symmetry& symmetry, int width, int height);

// The 4-connected line between two points (line4), both ends included. The
// ends are put in order first (top first, then left), so a line covers the
// same pixels whichever end the drag started from, and its mirror image
// across a vertical axis is the mirrored line. Flipped top to bottom it can
// differ by a pixel at a corner: a 4-connected diagonal has to choose which
// side of each step to fill.
Pixels lineShape(int x0, int y0, int x1, int y1, int width, int height);
Pixels lineShape(int x0, int y0, int x1, int y1, const PixelRect& clip);

// The rectangle with these corners: its border, or every pixel when filled.
Pixels rectShape(int x0, int y0, int x1, int y1, bool filled, int width, int height);
Pixels rectShape(int x0, int y0, int x1, int y1, bool filled, const PixelRect& clip);

// The ellipse inscribed in the rectangle with these corners. A pixel is in
// it when its centre is within the ellipse through the box's outer pixel
// centres with each radius r tuned to sqrt(r(r + 1)) (in pixels), the
// radius small circles need to read round: a 3 × 3 box is a full square,
// 5 × 5 has rows 3, 5, 5, 5, 3 wide and 7 × 7 rows 3, 5, 7, 7, 7, 5, 3. The
// outline is the pixels of the filled shape with a neighbour (diagonals
// too) outside it, so the outline is 4-connected. A box one pixel wide or
// high is a line. Symmetric both ways.
Pixels ellipseShape(int x0, int y0, int x1, int y1, bool filled, int width, int height);
Pixels ellipseShape(int x0, int y0, int x1, int y1, bool filled, const PixelRect& clip);

} // namespace bw
