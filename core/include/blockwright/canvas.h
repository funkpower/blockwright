#pragma once
// A fixed-size grid of cells, row-major.

#include "blockwright/cell.h"

#include <vector>

namespace bw {

class Canvas {
public:
    static constexpr int kDefaultWidth = 80;
    static constexpr int kDefaultHeight = 40;

    Canvas() : Canvas(kDefaultWidth, kDefaultHeight) {}
    Canvas(int width, int height);

    int width() const { return w_; }
    int height() const { return h_; }
    bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }

    Cell& at(int x, int y) { return cells_[static_cast<size_t>(y) * w_ + x]; }
    const Cell& at(int x, int y) const { return cells_[static_cast<size_t>(y) * w_ + x]; }

    // Grows or shrinks, keeping the cells that still fit; new cells are blank.
    void resize(int width, int height);

    bool operator==(const Canvas&) const = default;

private:
    int w_ = 0, h_ = 0;
    std::vector<Cell> cells_;
};

} // namespace bw
