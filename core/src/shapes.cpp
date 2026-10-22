#include "blockwright/shapes.h"

#include "blockwright/brush.h"

#include <algorithm>
#include <cstdlib>

namespace bw {

namespace {

// The box's corners in order and its part within the grid, or false when
// they don't meet.
struct Box {
    long long x0, y0, x1, y1;     // the shape's box
    long long cx0, cy0, cx1, cy1;  // its pixels in the grid
};

bool box(int ax, int ay, int bx, int by, int width, int height, Box& b) {
    b.x0 = std::min(ax, bx), b.x1 = std::max(ax, bx);
    b.y0 = std::min(ay, by), b.y1 = std::max(ay, by);
    b.cx0 = std::max(b.x0, 0LL), b.cy0 = std::max(b.y0, 0LL);
    b.cx1 = std::min(b.x1, width - 1LL), b.cy1 = std::min(b.y1, height - 1LL);
    return b.cx0 <= b.cx1 && b.cy0 <= b.cy1;
}

} // namespace

Pixels lineShape(int x0, int y0, int x1, int y1, int width, int height) {
    if (std::pair{y1, x1} < std::pair{y0, x0}) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    return line4(x0, y0, x1, y1, 0, 0, width - 1LL, height - 1LL);
}

Pixels rectShape(int x0, int y0, int x1, int y1, bool filled, int width, int height) {
    Pixels out;
    Box b;
    if (!box(x0, y0, x1, y1, width, height, b))
        return out;
    for (long long y = b.cy0; y <= b.cy1; ++y) {
        if (filled || y == b.y0 || y == b.y1) {
            for (long long x = b.cx0; x <= b.cx1; ++x)
                out.emplace_back(int(x), int(y));
        } else {
            if (b.x0 == b.cx0)
                out.emplace_back(int(b.x0), int(y));
            if (b.x1 == b.cx1 && b.x1 != b.x0)
                out.emplace_back(int(b.x1), int(y));
        }
    }
    return out;
}

Pixels ellipseShape(int x0, int y0, int x1, int y1, bool filled, int width, int height) {
    Pixels out;
    Box b;
    if (!box(x0, y0, x1, y1, width, height, b))
        return out;
    // In doubled coordinates from the box's centre, so the centre of an
    // even-sized box is whole: X = 2x - (x0 + x1) runs -rx..rx in steps of
    // two, rx being the box width less one. The radius rx (in half-pixels)
    // tuned to sqrt(rx(rx + 2)) is the radius r in pixels tuned to
    // sqrt(r(r + 1)). Inside: X²/A + Y²/B <= 1, multiplied out.
    const long long rx = b.x1 - b.x0, ry = b.y1 - b.y0;
    const double a = double(rx) * double(rx + 2), bb = double(ry) * double(ry + 2);
    auto inside = [&](long long x, long long y) {
        const long long X = 2 * x - (b.x0 + b.x1), Y = 2 * y - (b.y0 + b.y1);
        if (std::llabs(X) > rx || std::llabs(Y) > ry)
            return false;
        return double(X) * double(X) * bb + double(Y) * double(Y) * a <= a * bb;
    };
    for (long long y = b.cy0; y <= b.cy1; ++y)
        for (long long x = b.cx0; x <= b.cx1; ++x) {
            if (!inside(x, y))
                continue;
            bool edge = filled;
            for (int dy = -1; dy <= 1 && !edge; ++dy)
                for (int dx = -1; dx <= 1 && !edge; ++dx)
                    edge = !inside(x + dx, y + dy);
            if (edge)
                out.emplace_back(int(x), int(y));
        }
    return out;
}

} // namespace bw
