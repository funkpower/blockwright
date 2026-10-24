#include "blockwright/brushes/texture.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <tuple>

namespace bw {

namespace {

// How far out a lump's rim starts, over its radius.
constexpr double kRim = 0.75;

struct Shape {
    double r;       // radius in columns, before the jitter
    double squash;  // height over width, on screen
};

Shape shapeOf(TextureKind kind) {
    switch (kind) {
    case TextureKind::Foliage: return {4.0, 0.9};
    case TextureKind::Hoard: return {6.0, 0.6};
    case TextureKind::Scales: break;
    }
    return {5.0, 0.7};
}

// splitmix64's finaliser: the same numbers on every platform
std::uint64_t mix(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

std::uint64_t siteHash(int seed, long long i, long long j, int salt) {
    return mix(mix(mix(std::uint64_t(seed) * 4 + std::uint64_t(salt)) ^ std::uint64_t(i)) ^ std::uint64_t(j));
}

double unit(std::uint64_t h) { return double(h >> 11) * 0x1.0p-53; }

struct Lump {
    double cx = 0, cy = 0;  // centre, in cells (a cell's centre is at +0.5)
    double rx = 0, ry = 0;  // radii in columns and rows
    int peak = 0;           // the lit cells' rung
    int px = 0, py = 0;     // the left of the two lit cells
    long long i = 0, j = 0; // its grid site

    bool operator==(const Lump& o) const { return i == o.i && j == o.j; }

    bool covers(int x, int y) const {
        const double u = (x + 0.5 - cx) / rx, v = (y + 0.5 - cy) / ry;
        return u * u + v * v <= 1.0;
    }
    // drawn later: lower down, then further right
    bool above(const Lump& o) const {
        if (cy != o.cy)
            return cy > o.cy;
        if (cx != o.cx)
            return cx > o.cx;
        return j != o.j ? j > o.j : i > o.i;
    }
};

// The highest rung with every rung under it writable with iCE `ice`.
int topRung(const Ramp& ramp, bool ice) {
    if (ice)
        return ramp.topRung();
    for (int r = 1; r <= ramp.topRung(); ++r)
        if (rungCell(ramp, r).bg.paletteIndex() >= 8)
            return r - 1;
    return ramp.topRung();
}

class Field {
public:
    Field(const Ramp& ramp, const TextureOptions& o, Compass light, bool ice)
        : shape_(shapeOf(o.kind)), kind_(o.kind), seed_(std::clamp(o.seed, 0, kMaxTextureSeed)),
          top_(topRung(ramp, ice)), rampTop_(ramp.topRung()) {
        const double density = std::clamp(o.density, kMinTextureDensity, kMaxTextureDensity);
        // a grid cell's area is a lump's (pi r^2 squash on screen, half that
        // in cells) over the density, its sides in the lump's proportions
        const double s = std::sqrt(std::numbers::pi / density);
        gx_ = s * shape_.r;
        gy_ = s * shape_.r * shape_.squash / 2;
        std::tie(lx_, ly_) = compassVector(light);
    }

    // The rung at (x, y), before specks are cleared.
    int raw(int x, int y) const {
        const auto lump = owner(x, y);
        if (!lump)
            return 0;
        // the seam: where a lump drawn later covers a neighbour (so no two
        // lumps' lit cells meet along a row or a column), and the lump's
        // rim on the side away from the light
        for (const auto [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}})
            if (const auto n = owner(x + dx, y + dy); n && n->above(*lump))
                return 0;
        const double u = (x + 0.5 - lump->cx) / lump->rx, v = (y + 0.5 - lump->cy) / lump->ry;
        if (u * u + v * v > kRim * kRim && u * lx_ + v * ly_ < 0)
            return 0;
        const int far = std::max(std::abs(y - lump->py),
                                 std::min(std::abs(x - lump->px), std::abs(x - lump->px - 1)));
        const int floor = std::min(kRungsPerStep, lump->peak);
        return std::max(floor, lump->peak - far);
    }

    // The rung at (x, y) on a canvas `width` x `height` (0: no edge),
    // folded across its centre column (foldX) and row (foldY).
    int at(int x, int y, int width, int height, bool foldX, bool foldY) const {
        // a cell reads the field at its fold in the left (top) half
        const auto folded = [&](int fx, int fy) {
            if (foldX && width > 0)
                fx = std::min(fx, width - 1 - fx);
            if (foldY && height > 0)
                fy = std::min(fy, height - 1 - fy);
            return raw(fx, fy);
        };
        const int r = folded(x, y);
        if (r == 0 || r == rampTop_)
            return r;
        // lighter than all its neighbours on the canvas (three or more, as
        // the lint judges), folded as it is: a speck; it takes its lightest
        // neighbour's rung
        int most = -1, count = 0;
        for (const auto [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}}) {
            const int nx = x + dx, ny = y + dy;
            if (width > 0 && (nx < 0 || nx >= width))
                continue;
            if (height > 0 && (ny < 0 || ny >= height))
                continue;
            most = std::max(most, folded(nx, ny));
            ++count;
        }
        return count >= 3 && most < r ? most : r;
    }

private:
    Lump site(long long i, long long j) const {
        Lump l;
        l.i = i;
        l.j = j;
        l.cx = (double(i) + unit(siteHash(seed_, i, j, 0))) * gx_;
        l.cy = (double(j) + unit(siteHash(seed_, i, j, 1))) * gy_;
        const double rr = shape_.r * (0.75 + 0.45 * unit(siteHash(seed_, i, j, 2)));
        l.rx = rr;
        l.ry = rr * shape_.squash / 2;
        const std::uint64_t pick = siteHash(seed_, i, j, 3) % 3;
        int peak = top_;
        if (kind_ == TextureKind::Foliage)
            peak = top_ - int(pick);
        else if (kind_ == TextureKind::Hoard)
            peak = pick == 0 ? top_ : top_ - 2;
        l.peak = std::max(1, peak);
        // the lit point, 0.45 of the way from the centre to the rim
        const double lx = l.cx + lx_ * 0.45 * l.rx, ly = l.cy + ly_ * 0.45 * l.ry;
        l.px = int(std::floor(lx - 0.5));
        l.py = int(std::floor(ly));
        return l;
    }

    // The lump on top at (x, y), if any covers it.
    std::optional<Lump> owner(int x, int y) const {
        const double reachX = shape_.r * 1.2, reachY = shape_.r * 1.2 * shape_.squash / 2;
        const double ox = x + 0.5, oy = y + 0.5;
        const auto i0 = static_cast<long long>(std::floor((ox - reachX) / gx_));
        const auto i1 = static_cast<long long>(std::floor((ox + reachX) / gx_));
        const auto j0 = static_cast<long long>(std::floor((oy - reachY) / gy_));
        const auto j1 = static_cast<long long>(std::floor((oy + reachY) / gy_));
        std::optional<Lump> best;
        for (long long j = j0; j <= j1; ++j)
            for (long long i = i0; i <= i1; ++i) {
                const Lump l = site(i, j);
                if (l.covers(x, y) && (!best || l.above(*best)))
                    best = l;
            }
        return best;
    }

    Shape shape_;
    TextureKind kind_;
    int seed_;
    int top_, rampTop_;
    double gx_ = 1, gy_ = 1, lx_ = 0, ly_ = 0;
};

} // namespace

int textureRung(const Ramp& ramp, const TextureOptions& options, Compass light, int x, int y, bool ice, int width,
                int height, bool foldX, bool foldY) {
    return Field(ramp, options, light, ice).at(x, y, width, height, foldX, foldY);
}

TextureBrush::TextureBrush(Ramp ramp, TextureOptions options, Compass light)
    : ramp_(std::move(ramp)), options_(options), light_(light) {}

std::optional<Cell> TextureBrush::paintCell(const Stroke& stroke, int x, int y, double) const {
    // with a mirror on, every cell reads the field folded about the mirror
    // axes, whichever image paints it, so overlapping images agree; a
    // radial image reads the field at its own cell
    const Canvas& c = stroke.canvas();
    const SymmetryMode mode = stroke.options().symmetry.mode;
    const bool foldX = mode == SymmetryMode::MirrorX || mode == SymmetryMode::MirrorXY;
    const bool foldY = mode == SymmetryMode::MirrorY || mode == SymmetryMode::MirrorXY;
    const bool ice = stroke.options().ice;
    return rungCell(ramp_, textureRung(ramp_, options_, light_, x, y, ice, c.width(), c.height(), foldX, foldY));
}

} // namespace bw
