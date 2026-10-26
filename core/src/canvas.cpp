#include "blockwright/canvas.h"

#include <algorithm>

namespace bw {

namespace {
thread_local long long copied = 0;
thread_local long long compared = 0;
std::atomic<std::uint64_t> nextRevision{1};
} // namespace

Canvas::Canvas(int width, int height)
    : w_(std::max(0, width)), h_(std::max(0, height)), cells_(static_cast<size_t>(w_) * h_) {}

Canvas::Canvas(const Canvas& other)
    : w_(other.w_), h_(other.h_), cells_(other.cells_), revision_(other.revision_.load(std::memory_order_relaxed)) {
    copied += static_cast<long long>(cells_.size());
}

Canvas& Canvas::operator=(const Canvas& other) {
    w_ = other.w_;
    h_ = other.h_;
    cells_ = other.cells_;
    revision_.store(other.revision_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    copied += static_cast<long long>(cells_.size());
    return *this;
}

Canvas::Canvas(Canvas&& other) noexcept
    : w_(other.w_), h_(other.h_), cells_(std::move(other.cells_)),
      revision_(other.revision_.exchange(0, std::memory_order_relaxed)) {}

Canvas& Canvas::operator=(Canvas&& other) noexcept {
    w_ = other.w_;
    h_ = other.h_;
    cells_ = std::move(other.cells_);
    revision_.store(other.revision_.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    return *this;
}

std::uint64_t Canvas::revision() const {
    std::uint64_t r = revision_.load(std::memory_order_acquire);
    if (r)
        return r;
    const std::uint64_t fresh = nextRevision.fetch_add(1, std::memory_order_relaxed);
    // another thread naming the same cells first wins; its name holds as well
    return revision_.compare_exchange_strong(r, fresh, std::memory_order_acq_rel) ? fresh : r;
}

long long canvasCellsCopied() { return copied; }

bool Canvas::operator==(const Canvas& other) const {
    if (w_ != other.w_ || h_ != other.h_)
        return false;
    compared += static_cast<long long>(cells_.size());
    return cells_ == other.cells_;
}

long long canvasCellsCompared() { return compared; }

void Canvas::resize(int width, int height) {
    Canvas next(width, height);
    for (int y = 0; y < std::min(h_, next.h_); ++y)
        for (int x = 0; x < std::min(w_, next.w_); ++x)
            next.at(x, y) = at(x, y);
    *this = std::move(next);
}

} // namespace bw
