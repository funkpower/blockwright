#pragma once
// A fixed-size grid of cells, row-major.

#include "blockwright/cell.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace bw {

class Canvas {
public:
    static constexpr int kDefaultWidth = 80;
    static constexpr int kDefaultHeight = 40;

    Canvas() : Canvas(kDefaultWidth, kDefaultHeight) {}
    Canvas(int width, int height);
    // A copy adds its cells to canvasCellsCopied (below); a move adds none.
    // Both keep the revision (below).
    Canvas(const Canvas& other);
    Canvas& operator=(const Canvas& other);
    Canvas(Canvas&& other) noexcept;
    Canvas& operator=(Canvas&& other) noexcept;

    int width() const { return w_; }
    int height() const { return h_; }
    bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }

    Cell& at(int x, int y) {
        revision_.store(0, std::memory_order_relaxed);
        return cells_[static_cast<size_t>(y) * w_ + x];
    }
    const Cell& at(int x, int y) const { return cells_[static_cast<size_t>(y) * w_ + x]; }

    // Grows or shrinks, keeping the cells that still fit; new cells are blank.
    void resize(int width, int height);

    // A number naming the cells as they are, in constant time, for a cache
    // keyed on them: two canvases, or one canvas at two times, with the same
    // revision hold the same cells. A copy keeps its source's; the mutable
    // at() and resize give the canvas a new one at the next call. A Cell&
    // from at() kept and written through after a call goes unseen.
    std::uint64_t revision() const;

    // Adds the cells it compares to canvasCellsCompared (below).
    bool operator==(const Canvas& other) const;

private:
    int w_ = 0, h_ = 0;
    std::vector<Cell> cells_;
    // 0 until revision() names the cells; at() and resize put it back to 0.
    mutable std::atomic<std::uint64_t> revision_{0};
};

// How many cells canvases have copied on this thread so far (copy
// construction and copy assignment): a work counter, for tests of what an
// edit costs.
long long canvasCellsCopied();
// How many cells canvas comparisons have compared on this thread so far: a
// work counter like canvasCellsCopied.
long long canvasCellsCompared();

} // namespace bw
