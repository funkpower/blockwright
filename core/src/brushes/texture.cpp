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
    case TextureKind::Scales:
    case TextureKind::Brick:
    case TextureKind::Noise: break;
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

// brick and noise draw from their own stream, apart from the lumps' sites
std::uint64_t cellHash(int seed, int salt, long long i, long long j) {
    return mix(mix(mix(mix(std::uint64_t(seed)) + 0x51ed27 + std::uint64_t(salt)) ^ std::uint64_t(i)) ^
               std::uint64_t(j));
}

double unit(std::uint64_t h) { return double(h >> 11) * 0x1.0p-53; }

long long floorDiv(long long a, long long b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

// A rung less per cell toward an edge, over the `n` cells nearest it: the
// fall to shadow at `d` cells from the far edge.
int fall(int d, int n) { return d < n ? d - n : 0; }

// The noise kind's numbers. Its field is value noise (smoothstep between
// seeded lattice values in 0-1) at three scales, a drift, a mottle and a
// grain, plus the mottle's relief lit from the light, read once per two
// cells side by side (a block, square on screen). Two blocks side by side or
// one above the other are two columns apart on screen, and value noise of
// lattice spacing L climbs at most 1.5 per L columns, so a term of
// amplitude c moves at most 3c / L a block; the relief, the mottle at a
// block less the mottle a fifth of L toward the light, moves at most as
// much. The terms share 0.95 of a rung a block, so the rounded rungs of
// neighbouring blocks are at most one apart.
constexpr double kNoiseSpan = 24.0;  // the mottle's lattice spacing in columns at density 1
struct NoiseTerm {
    double spacing;  // over the mottle's
    double share;    // of the climb a block
};
constexpr NoiseTerm kDrift{3.0, 0.3}, kMottle{1.0, 0.35}, kGrain{0.4, 0.1}, kRelief{1.0, 0.25};
constexpr double kNoiseStep = 0.95;  // the most the field moves a block, in rungs

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
        // brick: a course is the brick's rows and a mortar row, and a brick
        // about 2.6 times as long as it is tall on screen
        brickRows_ = std::max(2, int(std::lround(3.6 / std::sqrt(density))));
        brickCols_ = int(std::lround(brickRows_ * 5.2));
        // noise: the mottle's lattice spacing, the low end of its rungs and
        // the terms' amplitudes, kept to the ramp's range
        mottle_ = kNoiseSpan / density;
        low_ = std::min(kRungsPerStep / 2, top_);
        mean_ = 0.5 * (low_ + top_);
        const auto amp = [&](NoiseTerm t) { return t.share * kNoiseStep / 3 * t.spacing * mottle_; };
        // a coarse field's swing is cut to about the ramp's (cutting every
        // term alike keeps the climb under a rung)
        const double swing = amp(kDrift) + amp(kMottle) + amp(kGrain) + 0.3 * amp(kRelief);
        const double cut = std::min(1.0, 1.4 * (top_ - low_) / swing);
        drift_ = cut * amp(kDrift);
        mottleAmp_ = cut * amp(kMottle);
        grain_ = cut * amp(kGrain);
        relief_ = cut * amp(kRelief);
    }

    // The rung at (x, y), before specks are cleared.
    int raw(int x, int y) const {
        if (kind_ == TextureKind::Brick)
            return brick(x, y);
        if (kind_ == TextureKind::Noise)
            return noise(x, y);
        const auto lump = owner(x, y);
        if (!lump)
            return 0;
        // the seam: where a lump drawn later covers a neighbour (so no two
        // lumps' lit cells meet along a row or a column), and the lump's
        // rim on the side away from the light
        for (const auto& [dx, dy] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}})
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
            // noise is read a block of two cells at a time; a block's left
            // cell whose right one isn't shown beside it (the last column,
            // or an odd canvas's centre column under a fold) joins the block
            // to its left, so every cell has a neighbour of its own rung
            if (kind_ == TextureKind::Noise && width > 0 && fx % 2 == 0 &&
                fx == (foldX ? width - 1 - fx : width - 1))
                --fx;
            return raw(fx, fy);
        };
        // the folded rungs within two steps of the cell as they are read,
        // -1 off the canvas (-2: not read yet)
        int near[5][5];
        for (auto& row : near)
            std::fill(std::begin(row), std::end(row), -2);
        const auto base = [&](int dx, int dy) {
            int& r = near[dy + 2][dx + 2];
            if (r == -2)
                r = (width <= 0 || (x + dx >= 0 && x + dx < width)) &&
                            (height <= 0 || (y + dy >= 0 && y + dy < height))
                        ? folded(x + dx, y + dy)
                        : -1;
            return r;
        };
        // A speck is lighter or darker than all its neighbours on the canvas
        // (three or more, as the lint judges), folded as it is; black and
        // the ramp's top aren't. First a lighter one takes its lightest
        // neighbour's rung, then, in what that leaves, a darker one takes
        // its darkest neighbour's. Each takes a neighbour's rung, so it
        // matches that neighbour and makes no new speck of it or of any
        // other cell (two neighbours can't both be lighter, or darker, than
        // each other), and the rungs a step apart stay so.
        constexpr std::pair<int, int> kSides[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        const auto clear = [&](int r, auto&& rungAt, bool lighter) {
            if (r <= 0 || r == rampTop_)
                return r;
            int most = -1, least = rampTop_ + 1, count = 0;
            for (const auto& [dx, dy] : kSides) {
                const int n = rungAt(dx, dy);
                if (n < 0)
                    continue;
                most = std::max(most, n);
                least = std::min(least, n);
                ++count;
            }
            if (count < 3)
                return r;
            if (lighter)
                return most < r ? most : r;
            return least > r ? least : r;
        };
        const auto unlit = [&](int cx, int cy) {
            return clear(base(cx, cy), [&](int dx, int dy) { return base(cx + dx, cy + dy); }, true);
        };
        const int r = unlit(0, 0);
        // the first pass only darkens, so a neighbour no lighter than the
        // cell before it is none after: no darker speck, and the cells two
        // steps away needn't be read
        for (const auto& [dx, dy] : kSides)
            if (const int n = base(dx, dy); n >= 0 && n <= r)
                return r;
        return clear(r, [&](int dx, int dy) { return base(dx, dy) < 0 ? -1 : unlit(dx, dy); }, false);
    }

private:
    // Running-bond courses: a mortar row (rung 0) over each course, a
    // mortar column (rung 0) between bricks, every other course shifted half
    // a brick, each joint moved a column either way or not by the seed.
    // A brick is a plane at its face rung (the seed moves it a rung either
    // way or not), a rung lighter along its two columns and its row on the
    // light's side, and falling a rung a cell to its far edges: three
    // columns (two on a short brick) and two rows (one on a brick three
    // rows tall, none on two). The two lit columns mean no cell is lighter
    // than both its neighbours along the row, so no brick cell is a speck;
    // the lit corner is two lit cells, a rung over the lit edges. A fold
    // (at()) reflects the profiles, so where it meets a brick's slope a cell
    // can be darker than its neighbours; at() clears it.
    int brick(int x, int y) const {
        const int course = brickRows_ + 1, period = brickCols_ + 1;
        const long long k = floorDiv(y, course);
        const int v = int(y - k * course) - 1;  // the brick's row, -1 the mortar
        if (v < 0)
            return 0;
        const long long shift = (k & 1) ? period / 2 : 0;
        const auto joint = [&](long long j) {
            return j * period + shift + int(cellHash(seed_, 0, j, k) % 3) - 1;
        };
        long long j = floorDiv(x - shift, period);
        while (joint(j) > x)
            --j;
        while (joint(j + 1) <= x)
            ++j;
        const long long left = joint(j);
        if (x == left)
            return 0;
        const int u = int(x - left) - 1, width = int(joint(j + 1) - left) - 1;
        static constexpr int kTone[4] = {-1, 0, 0, 1};
        const int face = int(std::lround(top_ * 0.62)) + kTone[cellHash(seed_, 1, j, k) % 4];
        // along the row, then down the column, with the light's side first
        const auto ramp = [](int i, int n, double toward, int lit, int falls) {
            if (toward > 0.3)
                i = n - 1 - i;
            else if (toward > -0.3)
                return 0;
            return (i < lit ? 1 : 0) + fall(n - 1 - i, falls);
        };
        const int across = ramp(u, width, lx_, 2, width >= 10 ? 3 : 2);
        const int down = ramp(v, brickRows_, ly_, 1, brickRows_ >= 4 ? 2 : brickRows_ - 2);
        return std::clamp(face + across + down, 1, std::max(1, top_));
    }

    // Value noise at (sx, sy) in columns on screen, lattice `spacing` apart.
    double valueNoise(double sx, double sy, double spacing, int salt) const {
        const double gx = sx / spacing, gy = sy / spacing;
        const double fi = std::floor(gx), fj = std::floor(gy);
        const auto i = static_cast<long long>(fi), j = static_cast<long long>(fj);
        const auto smooth = [](double t) { return t * t * (3 - 2 * t); };
        const double tx = smooth(gx - fi), ty = smooth(gy - fj);
        const auto at = [&](long long a, long long b) { return unit(cellHash(seed_, 2 + salt, a, b)); };
        const double upper = at(i, j) + (at(i + 1, j) - at(i, j)) * tx;
        const double lower = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * tx;
        return upper + (lower - upper) * ty;
    }

    // A mottled stone: the drift, mottle and grain and, lit from the light,
    // the mottle's relief, about the middle of the rungs from low_ to the
    // top, rounded to rungs. Read per block (two cells side by side),
    // so every cell has a neighbour of its own rung and none is a speck.
    int noise(int x, int y) const {
        const double sx = 2.0 * double(floorDiv(x, 2)) + 1.0, sy = 2.0 * y + 1.0;
        const double reach = mottle_ / 5;
        const double h = valueNoise(sx, sy, mottle_, 0);
        const double lit = h - valueNoise(sx + lx_ * reach, sy + ly_ * reach, mottle_, 0);
        const double q = mean_ + drift_ * (valueNoise(sx, sy, kDrift.spacing * mottle_, 2) - 0.5) +
                         mottleAmp_ * (h - 0.5) + grain_ * (valueNoise(sx, sy, kGrain.spacing * mottle_, 1) - 0.5) +
                         relief_ * lit;
        return std::clamp(int(std::lround(q)), low_, top_);
    }

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
    int brickRows_ = 3, brickCols_ = 16;
    double mottle_ = kNoiseSpan, mean_ = 0, drift_ = 0, mottleAmp_ = 0, grain_ = 0, relief_ = 0;
    int low_ = 0;
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
