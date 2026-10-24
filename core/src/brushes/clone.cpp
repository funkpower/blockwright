#include "blockwright/brushes/clone.h"

#include <map>
#include <tuple>

namespace bw {

namespace {

// Image `image` of cell (x, y), or empty where symmetryDabs leaves it out.
std::optional<std::pair<int, int>> imageOf(const Symmetry& symmetry, int x, int y, int image, int width,
                                           int height) {
    Dab d;
    d.x = x;
    d.y = y;
    for (const Dab& im : symmetryDabs(symmetry, d, width, height))
        if (im.image == image)
            return std::pair{im.x, im.y};
    return std::nullopt;
}

} // namespace

std::optional<std::pair<long long, long long>> CloneBrush::offset(const Symmetry& symmetry, int image, int width,
                                                                  int height) const {
    const auto s = imageOf(symmetry, source_.first, source_.second, image, width, height);
    const auto a = imageOf(symmetry, anchor_.first, anchor_.second, image, width, height);
    if (!s || !a)
        return std::nullopt;
    return std::pair{static_cast<long long>(s->first) - a->first, static_cast<long long>(s->second) - a->second};
}

void CloneBrush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    const Symmetry& sym = stroke.options().symmetry;
    if (sym.mode != SymmetryMode::MirrorX && sym.mode != SymmetryMode::MirrorY &&
        sym.mode != SymmetryMode::MirrorXY) {
        Brush::dab(stroke, d, size, strength);
        return;
    }
    // The stroke's own image copies for all of them; the others' stamps are
    // done with it.
    if (d.image != 0)
        return;
    const Canvas& c = stroke.canvas();
    const int w = c.width(), h = c.height();
    const Tip tip = stroke.settings().tip;
    // Each cell goes to the image whose dab is nearest it. On a tie (the
    // cell is on an axis, or the images' dabs meet on the axis of an odd
    // canvas), the image whose frame has the cell in the canvas's first half
    // across, then down, wins, then the lower image, as the outline brush
    // settles its overlaps: a cell and its mirror cell go to two images that
    // mirror each other, so a symmetric canvas stays symmetric.
    using Rank = std::tuple<long long, bool, bool, int>;  // distance, second halves, image
    std::map<std::pair<int, int>, Rank> cells;            // (y, x): row-major
    for (const Dab& im : symmetryDabs(sym, d, w, h))
        for (const auto& [x, y] : footprint(im.x, im.y, size, tip, w, h, im.flipX, im.flipY)) {
            const long long dx = static_cast<long long>(x) - im.x, dy = 2LL * (static_cast<long long>(y) - im.y);
            const int fx = im.flipX ? w - 1 - x : x, fy = im.flipY ? h - 1 - y : y;
            const Rank r{dx * dx + dy * dy, 2 * fx > w - 1, 2 * fy > h - 1, im.image};
            const auto [it, added] = cells.try_emplace({y, x}, r);
            if (!added && r < it->second)
                it->second = r;
        }
    for (const auto& [at, r] : cells) {
        const auto [y, x] = at;
        const int image = std::get<3>(r);
        if (!stroke.paintable(x, y) || stroke.painted(x, y))
            continue;
        if (const auto cell = copy(stroke, x, y, image))
            stroke.set(x, y, *cell, strength, image);
    }
}

std::pair<long long, long long> CloneBrush::sampleAt(int x, int y) const {
    return {static_cast<long long>(x) + source_.first - anchor_.first,
            static_cast<long long>(y) + source_.second - anchor_.second};
}

std::optional<Cell> CloneBrush::paintCell(const Stroke& stroke, int x, int y, double) const {
    return copy(stroke, x, y, stroke.currentDab().image);
}

std::optional<Cell> CloneBrush::copy(const Stroke& stroke, int x, int y, int image) const {
    const Canvas& c = stroke.canvas();
    const Symmetry& sym = stroke.options().symmetry;
    if (!cached_ || cached_->symmetry != sym || cached_->width != c.width() || cached_->height != c.height())
        cached_ = Cached{sym, c.width(), c.height(), {}};
    auto it = cached_->offsets.find(image);
    if (it == cached_->offsets.end())
        it = cached_->offsets.emplace(image, offset(sym, image, c.width(), c.height())).first;
    const auto& o = it->second;
    if (!o)
        return std::nullopt;
    const long long sx = x + o->first, sy = y + o->second;
    if (sx < 0 || sy < 0 || sx >= c.width() || sy >= c.height())
        return std::nullopt;
    return stroke.originalAt(int(sx), int(sy));
}

std::optional<CloneBrush> CloneSource::beginStroke(int x, int y) {
    if (!source)
        return std::nullopt;
    if (!aligned || !anchor)
        anchor = {x, y};
    return CloneBrush(*source, *anchor);
}

std::optional<std::pair<long long, long long>> CloneSource::sampleFor(int x, int y) const {
    if (!source)
        return std::nullopt;
    if (!aligned || !anchor)
        return std::pair<long long, long long>{source->first, source->second};
    return std::pair{static_cast<long long>(x) + source->first - anchor->first,
                     static_cast<long long>(y) + source->second - anchor->second};
}

} // namespace bw
