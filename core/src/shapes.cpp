#include "blockwright/shapes.h"

#include "blockwright/brush.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bw {

namespace {

// The box's corners in order and its part within the grid, or false when
// they don't meet.
struct Box {
    long long x0, y0, x1, y1;     // the shape's box
    long long cx0, cy0, cx1, cy1;  // its pixels in the grid
};

bool box(int ax, int ay, int bx, int by, const PixelRect& clip, Box& b) {
    b.x0 = std::min(ax, bx), b.x1 = std::max(ax, bx);
    b.y0 = std::min(ay, by), b.y1 = std::max(ay, by);
    b.cx0 = std::max(b.x0, clip.x0), b.cy0 = std::max(b.y0, clip.y0);
    b.cx1 = std::min(b.x1, clip.x1), b.cy1 = std::min(b.y1, clip.y1);
    return b.cx0 <= b.cx1 && b.cy0 <= b.cy1;
}

PixelRect grid(int width, int height) { return {0, 0, width - 1LL, height - 1LL}; }

} // namespace

PixelRect symmetryReach(const Symmetry& s, int width, int height) {
    PixelRect r{0, 0, width - 1LL, 2LL * height - 1};
    if (s.mode != SymmetryMode::Radial || width <= 0 || height <= 0)
        return r;
    // as symmetryPixels: the centre doubled, a set centre between its
    // cell's two pixels; the canvas's outer pixel edges doubled about it
    const double cx = s.centre ? 2.0 * s.centre->first : width - 1.0,
                 cy = s.centre ? 4.0 * s.centre->second + 1 : 2.0 * height - 1;
    const double xs[] = {-1 - cx, 2.0 * width - 1 - cx}, ys[] = {-1 - cy, 4.0 * height - 1 - cy};
    const int n = std::clamp(s.folds, kMinFolds, kMaxFolds);
    for (int k = 1; k < n; ++k) {
        // turned back: the pixels a fold of k carries onto the canvas
        const double a = -2.0 * 3.14159265358979323846 * k / n, cs = std::cos(a), sn = std::sin(a);
        for (const double u : xs)
            for (const double v : ys) {
                const double x = (cx + u * cs - v * sn) / 2, y = (cy + u * sn + v * cs) / 2;
                r.x0 = std::min(r.x0, static_cast<long long>(std::floor(x)) - 1);
                r.x1 = std::max(r.x1, static_cast<long long>(std::ceil(x)) + 1);
                r.y0 = std::min(r.y0, static_cast<long long>(std::floor(y)) - 1);
                r.y1 = std::max(r.y1, static_cast<long long>(std::ceil(y)) + 1);
            }
    }
    return r;
}

Pixels lineShape(int x0, int y0, int x1, int y1, int width, int height) {
    return lineShape(x0, y0, x1, y1, grid(width, height));
}

Pixels lineShape(int x0, int y0, int x1, int y1, const PixelRect& clip) {
    if (std::pair{y1, x1} < std::pair{y0, x0}) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    return line4(x0, y0, x1, y1, clip.x0, clip.y0, clip.x1, clip.y1);
}

Pixels rectShape(int x0, int y0, int x1, int y1, bool filled, int width, int height) {
    return rectShape(x0, y0, x1, y1, filled, grid(width, height));
}

Pixels rectShape(int x0, int y0, int x1, int y1, bool filled, const PixelRect& clip) {
    Pixels out;
    Box b;
    if (!box(x0, y0, x1, y1, clip, b))
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
    return ellipseShape(x0, y0, x1, y1, filled, grid(width, height));
}

Pixels ellipseShape(int x0, int y0, int x1, int y1, bool filled, const PixelRect& clip) {
    Pixels out;
    Box b;
    if (!box(x0, y0, x1, y1, clip, b))
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
