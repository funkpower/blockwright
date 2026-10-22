#include "blockwright/canvas.h"

#include <algorithm>

namespace bw {

Canvas::Canvas(int width, int height)
    : w_(std::max(0, width)), h_(std::max(0, height)), cells_(static_cast<size_t>(w_) * h_) {}

void Canvas::resize(int width, int height) {
    Canvas next(width, height);
    for (int y = 0; y < std::min(h_, next.h_); ++y)
        for (int x = 0; x < std::min(w_, next.w_); ++x)
            next.at(x, y) = at(x, y);
    *this = std::move(next);
}

} // namespace bw
