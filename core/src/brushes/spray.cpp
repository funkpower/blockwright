#include "blockwright/brushes/spray.h"

#include "blockwright/brushes/guard.h"
#include "blockwright/brushes/shading.h"

#include "blockwright/ansi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace bw {

namespace {

// splitmix64's finaliser: the same numbers on every platform
std::uint64_t mix(std::uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// salts: 0 haze, 1 smoke, 2 smoke's curl, 3 stars
std::uint64_t siteHash(int seed, long long i, long long j, int salt) {
    return mix(mix(mix(std::uint64_t(seed) * 4 + std::uint64_t(salt)) ^ std::uint64_t(i)) ^ std::uint64_t(j));
}

double unit(std::uint64_t h) { return double(h >> 11) * 0x1.0p-53; }

int seedOf(const SprayOptions& o) { return std::clamp(o.seed, 0, kMaxSpraySeed); }
int densityOf(const SprayOptions& o) { return std::clamp(o.density, kMinSprayDensity, kMaxSprayDensity); }

// x folded across the centre of `n` cells (none when n is 0 or `fold` off)
int fold(int x, int n, bool on) { return on && n > 0 ? std::min(x, n - 1 - x) : x; }

long long floorDiv(long long a, long long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// One octave of value noise: lattice values smoothly blended, in [0, 1).
// Only + - * /, so every platform gets the same numbers.
double valueNoise(int seed, int salt, double u, double v) {
    const double fi = std::floor(u), fj = std::floor(v);
    const auto i = static_cast<long long>(fi), j = static_cast<long long>(fj);
    const double s = u - fi, t = v - fj;
    const double ss = s * s * (3 - 2 * s), tt = t * t * (3 - 2 * t);
    const auto at = [&](long long a, long long b) { return unit(siteHash(seed, a, b, salt)); };
    const double top = at(i, j) + ss * (at(i + 1, j) - at(i, j));
    const double bottom = at(i, j + 1) + ss * (at(i + 1, j + 1) - at(i, j + 1));
    return top + tt * (bottom - top);
}

// The field's value at a cell, before the density cuts it.
double raw(SprayKind kind, int seed, int x, int y) {
    const double cx = x + 0.5, cy = y + 0.5;
    if (kind == SprayKind::Smoke) {
        // tall wisps, curled sideways along the rows
        const double curl = 6.0 * (valueNoise(seed, 2, cx / 16.0, cy / 6.0) - 0.5);
        return valueNoise(seed, 1, (cx + curl) / 4.0, cy / 10.0);
    }
    return valueNoise(seed, 0, cx / 20.0, cy / 3.0);  // one slow wave
}

// The value above which the field covers `density` percent of the cells,
// from the field's values over a fixed sample (the same for every seed).
double threshold(SprayKind kind, int density) {
    static const auto table = [] {
        std::array<std::array<double, kMaxSprayDensity + 1>, 2> out{};
        for (int k = 0; k < 2; ++k) {
            const SprayKind kind = k == 0 ? SprayKind::Haze : SprayKind::Smoke;
            std::vector<double> values;
            for (const int seed : {11, 23, 37})
                for (int y = 0; y < 150; ++y)
                    for (int x = 0; x < 300; ++x)
                        values.push_back(raw(kind, seed, x, y));
            std::sort(values.begin(), values.end());
            for (int d = 0; d <= kMaxSprayDensity; ++d) {
                const auto at = static_cast<size_t>(double(values.size()) * (1.0 - d / 100.0));
                out[size_t(k)][size_t(d)] = d == kMaxSprayDensity ? -1.0 : values[std::min(at, values.size() - 1)];
            }
        }
        return out;
    }();
    return table[kind == SprayKind::Haze ? 0 : 1][size_t(density)];
}

// The lift field over a box of cells: the cover cut from the field (folded,
// the canvas's edge cells repeated past it), cleaned of lone cells and pin
// holes, ▒ inside and ░ on the rim, ▒ specks dropped to ░.
class LiftWindow {
public:
    LiftWindow(const SprayOptions& o, int x0, int y0, int x1, int y1, int width, int height, bool foldX, bool foldY)
        : x0_(x0 - kMargin), y0_(y0 - kMargin), w_(x1 - x0 + 1 + 2 * kMargin), h_(y1 - y0 + 1 + 2 * kMargin) {
        const int seed = seedOf(o);
        const double t = threshold(o.kind, densityOf(o));
        std::vector<char> r(size_t(w_) * size_t(h_));
        for (int j = 0; j < h_; ++j)
            for (int i = 0; i < w_; ++i) {
                int x = x0_ + i, y = y0_ + j;
                if (width > 0)
                    x = fold(std::clamp(x, 0, width - 1), width, foldX);
                if (height > 0)
                    y = fold(std::clamp(y, 0, height - 1), height, foldY);
                r[idx(i, j)] = raw(o.kind, seed, x, y) > t;
            }
        constexpr int kFour[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
        // each pass reads the last one cell further out
        const auto pass = [&](const std::vector<char>& from, int margin, auto&& rule) {
            std::vector<char> out(from.size());
            for (int j = margin; j < h_ - margin; ++j)
                for (int i = margin; i < w_ - margin; ++i)
                    out[idx(i, j)] = rule(from, i, j);
            return out;
        };
        const auto c = pass(r, 1, [&](const std::vector<char>& f, int i, int j) {
            int n = 0;
            for (const auto& d : kFour)
                n += f[idx(i + d[0], j + d[1])] != 0;
            return char(f[idx(i, j)] ? n >= 2 : n >= 3);
        });
        const auto l1 = pass(c, 2, [&](const std::vector<char>& f, int i, int j) {
            if (!f[idx(i, j)])
                return char(0);
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di)
                    if (!f[idx(i + di, j + dj)])
                        return char(1);
            return char(2);
        });
        lift_ = pass(l1, 3, [&](const std::vector<char>& f, int i, int j) {
            const char v = f[idx(i, j)];
            if (v != 2)
                return v;
            for (const auto& d : kFour)
                if (f[idx(i + d[0], j + d[1])] == 2)
                    return v;
            return char(1);
        });
    }

    int at(int x, int y) const { return lift_[idx(x - x0_, y - y0_)]; }

private:
    static constexpr int kMargin = 3;
    size_t idx(int i, int j) const { return size_t(j) * size_t(w_) + size_t(i); }

    int x0_, y0_, w_, h_;
    std::vector<char> lift_;
};

constexpr char32_t kStars[] = {U'.', U'·', U'∙', U'+', U'*', U'☼'};
constexpr int kStarWeights[] = {35, 25, 20, 8, 8, 4};  // percent

// A star's block, and the clearance round it ("four pixels off every
// shape": columns, and rows of two pixels each).
constexpr int kStarBlockW = 8, kStarBlockH = 4;
constexpr int kClearX = 4, kClearY = 2;

// How far from a dab's footprint a cell the stroke held back is tried again.
constexpr int kSprayRetry = 4;

// The flat colour a cell reads as for a star: its own (solidColour), or a
// star's background; empty for a transparent or blinking cell, or anything
// else.
std::optional<std::uint8_t> groundOf(const Cell& cell, bool ice) {
    if (cell.transparent)
        return std::nullopt;
    const Cell shown = ice ? cell : iceCell(cell, false);
    if (shown.attrs & attr::Blink)
        return std::nullopt;
    if (isSprayStar(shown.ch))
        return shown.bg.isIndexed() ? std::optional<std::uint8_t>(shown.bg.index) : std::nullopt;
    return solidColour(shown);
}

} // namespace

bool isSprayStar(Glyph g) { return std::find(std::begin(kStars), std::end(kStars), g.codePointOr(kNoCodePoint)) != std::end(kStars); }

std::optional<char32_t> sprayStar(const SprayOptions& options, int x, int y, int width, int height, bool foldX,
                                  bool foldY) {
    if (options.kind != SprayKind::Stars)
        return std::nullopt;
    const int fx = fold(x, width, foldX), fy = fold(y, height, foldY);
    // a star and its mirror partner too close across the fold
    if (foldX && width > 0 && width - 1 - 2 * fx >= 1 && width - 1 - 2 * fx <= kClearX)
        return std::nullopt;
    if (foldY && height > 0 && height - 1 - 2 * fy >= 1 && height - 1 - 2 * fy <= kClearY)
        return std::nullopt;
    const long long i = floorDiv(fx, kStarBlockW), j = floorDiv(fy, kStarBlockH);
    const std::uint64_t h = siteHash(seedOf(options), i, j, 3);
    // inside the block, so neighbouring blocks' stars are 5 columns or 3
    // rows apart at the least
    const long long sx = i * kStarBlockW + 2 + static_cast<long long>(h & 3);
    const long long sy = j * kStarBlockH + 1 + static_cast<long long>((h >> 2) & 1);
    if (fx != sx || fy != sy)
        return std::nullopt;
    if (unit(mix(h)) * 100.0 >= densityOf(options))
        return std::nullopt;
    double pick = unit(mix(mix(h))) * 100.0;
    for (size_t k = 0; k < std::size(kStars); ++k) {
        if (pick < kStarWeights[k])
            return kStars[k];
        pick -= kStarWeights[k];
    }
    return kStars[0];
}

int sprayLift(const SprayOptions& options, int x, int y, int width, int height, bool foldX, bool foldY) {
    if (options.kind == SprayKind::Stars)
        return 0;
    return LiftWindow(options, x, y, x, y, width, height, foldX, foldY).at(x, y);
}

SprayBrush::SprayBrush(Ramp ramp, SprayOptions options, Color ink, ShadeDirection direction)
    : ramp_(std::move(ramp)), options_(options), ink_(ink), direction_(direction) {}

std::optional<Cell> SprayBrush::paintCell(const Stroke& stroke, int x, int y, double) const {
    // stars only: haze and smoke paint in dab()
    const Canvas& c = stroke.canvas();
    if (options_.kind != SprayKind::Stars || !c.contains(x, y))
        return std::nullopt;
    const Cell& start = c.at(x, y);
    if (direction_ == ShadeDirection::Darken) {
        if (start.transparent || !isSprayStar(start.ch))
            return std::nullopt;
        Cell out = start;
        out.ch = U' ';
        return out;
    }
    const SymmetryMode mode = stroke.options().symmetry.mode;
    const bool foldX = mode == SymmetryMode::MirrorX || mode == SymmetryMode::MirrorXY;
    const bool foldY = mode == SymmetryMode::MirrorY || mode == SymmetryMode::MirrorXY;
    const auto glyph = sprayStar(options_, x, y, c.width(), c.height(), foldX, foldY);
    if (!glyph)
        return std::nullopt;
    const bool ice = stroke.options().ice;
    const auto ground = groundOf(start, ice);
    if (!ground || (!ice && *ground >= 8) || ink_.paletteIndex() == *ground)
        return std::nullopt;
    // open ground all round, read as the stroke began
    for (int dy = -kClearY; dy <= kClearY; ++dy)
        for (int dx = -kClearX; dx <= kClearX; ++dx)
            if (c.contains(x + dx, y + dy) && groundOf(c.at(x + dx, y + dy), ice) != ground)
                return std::nullopt;
    Cell out;
    out.ch = *glyph;
    out.fg = ink_;
    out.bg = Color::indexed(*ground);
    return out;
}

void SprayBrush::dab(Stroke& stroke, const Dab& d, int size, double) const {
    const Canvas& c = stroke.canvas();
    const int W = c.width(), H = c.height();
    const Tip tip = stroke.settings().tip;
    const bool ice = stroke.options().ice;
    const bool lighten = direction_ == ShadeDirection::Lighten;
    using Pos = std::pair<int, int>;  // (y, x): row-major
    // What a reached cell wants: the field's cell, its rung, and the image
    // that reached it.
    struct Want {
        Cell cell;
        int rung;
        int image;
    };
    // The stroke's lint guard, read from its start at the first dab; the
    // canvas a try is judged on (the guard's, between tries); the cells the
    // stroke has reached, and of those the ones not yet at the field (haze,
    // smoke) or not yet starred or wiped (stars: the cell and its image).
    struct State {
        std::optional<StrokeLint> guard;
        std::optional<Canvas> next;
        std::set<Pos> reached;
        std::map<Pos, Want> pending;
        std::map<Pos, std::pair<Cell, int>> stars;
    };
    auto& slot = stroke.brushState();
    if (!slot)
        slot = std::make_shared<State>();
    State& st = *static_cast<State*>(slot.get());
    if (!st.guard) {
        st.guard.emplace(c, ramp_);
        st.next = c;
    }
    StrokeLint& guard = *st.guard;
    Canvas& next = *st.next;
    // Under a mirror the dab's images are painted together from the dab
    // itself (the stroke's own image), the field folded about the axes.
    const Symmetry& sym = stroke.options().symmetry;
    const bool foldX = sym.mode == SymmetryMode::MirrorX || sym.mode == SymmetryMode::MirrorXY;
    const bool foldY = sym.mode == SymmetryMode::MirrorY || sym.mode == SymmetryMode::MirrorXY;
    const bool mirror = foldX || foldY;
    std::vector<Dab> images{d};
    std::vector<int> keys{d.image};
    if (mirror) {
        if (d.image != 0)
            return;
        images = symmetryDabs(sym, d, W, H);
        keys.clear();
        for (size_t i = 0; i < images.size(); ++i)
            keys.push_back(int(i));
    }
    // A cell's mirror images (itself only, without a mirror).
    const auto imagesOf = [&](int x, int y) {
        std::vector<std::pair<int, int>> out{{x, y}};
        if (mirror)
            for (const Dab& o : symmetryDabs(sym, Dab{x, y}, W, H))
                if (std::find(out.begin(), out.end(), std::pair{o.x, o.y}) == out.end())
                    out.emplace_back(o.x, o.y);
        return out;
    };
    // Judges a change of the guard's canvas: kept, and written to the
    // stroke as image `image` of each cell, if it adds no finding.
    const auto tryKeep = [&](const std::vector<std::pair<std::pair<int, int>, Cell>>& change,
                             const std::function<int(int, int)>& image) {
        std::vector<std::pair<int, int>> at;
        for (const auto& [xy, cell] : change) {
            next.at(xy.first, xy.second) = cell;
            at.push_back(xy);
        }
        const bool clean = guard.judge(next, at).empty();
        if (clean)
            guard.keep();
        for (const auto& [x, y] : at)
            next.at(x, y) = guard.canvas().at(x, y);
        if (clean)
            for (const auto& [xy, cell] : change)
                stroke.set(xy.first, xy.second, cell, 1.0, image(xy.first, xy.second));
        return clean;
    };
    // Each image's footprint and the box round it (x0, y0, x1, y1).
    std::vector<std::vector<std::pair<int, int>>> cellsOf(images.size());
    std::vector<std::array<int, 4>> boxOf(images.size(), {W, H, -1, -1});
    for (size_t i = 0; i < images.size(); ++i) {
        cellsOf[i] = footprint(images[i].x, images[i].y, size, tip, W, H, images[i].flipX, images[i].flipY);
        auto& [x0, y0, x1, y1] = boxOf[i];
        for (const auto& [x, y] : cellsOf[i]) {
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
    }
    // The held cells within kSprayRetry of a footprint cell (columns and
    // rows apart) are tried again at this dab, and no others, so a dab's
    // work is its neighbourhood's however long the stroke. Each image's
    // reach is its footprint grown by kSprayRetry, over its box grown as
    // far, row by row and then column by column.
    struct Reach {
        int x0, y0, w, h;
        std::vector<char> in;
    };
    std::vector<Reach> near;
    for (size_t i = 0; i < images.size(); ++i) {
        if (cellsOf[i].empty())
            continue;
        const int R = kSprayRetry;
        Reach n{boxOf[i][0] - R, boxOf[i][1] - R, boxOf[i][2] - boxOf[i][0] + 1 + 2 * R,
                boxOf[i][3] - boxOf[i][1] + 1 + 2 * R, {}};
        std::vector<char> foot(size_t(n.w) * n.h), wide(foot.size());
        n.in.resize(foot.size());
        for (const auto& [x, y] : cellsOf[i])
            foot[size_t(y - n.y0) * n.w + (x - n.x0)] = 1;
        // a cell is in when a footprint cell is within R of it along the
        // row, then when such a cell is within R of it up the column
        const auto grow = [R](const std::vector<char>& from, std::vector<char>& to, int lines, int len,
                              size_t line, size_t step) {
            for (int l = 0; l < lines; ++l) {
                int last = -R - 1;  // the last set cell at or before the next R ahead
                for (int k = -R; k < len; ++k) {
                    if (k + R < len && from[l * line + size_t(k + R) * step])
                        last = k + R;
                    if (k >= 0)
                        to[l * line + size_t(k) * step] = last >= k - R;
                }
            }
        };
        grow(foot, wide, n.h, n.w, size_t(n.w), 1);
        grow(wide, n.in, n.w, n.h, 1, size_t(n.w));
        near.push_back(std::move(n));
    }
    // The keys of a map of held cells in one of the reaches.
    const auto inReach = [&](const auto& held) {
        std::set<Pos> out;
        for (const Reach& n : near)
            for (int y = n.y0; y < n.y0 + n.h; ++y)
                for (auto it = held.lower_bound({y, n.x0}); it != held.end() && it->first < Pos{y, n.x0 + n.w};
                     ++it)
                    if (n.in[size_t(y - n.y0) * n.w + (it->first.second - n.x0)])
                        out.insert(it->first);
        return out;
    };

    if (options_.kind == SprayKind::Stars) {
        // Each star the dab uncovers (or, with the right button, takes off)
        // is held until it goes through the guard, with its mirror images,
        // by itself. The held ones in reach are tried in row-major order,
        // and again while any lands, so one a neighbour's wipe frees in this
        // dab or a later one near it goes too.
        for (size_t i = 0; i < images.size(); ++i)
            for (const auto& [x, y] : cellsOf[i]) {
                if (!st.reached.insert({y, x}).second || !stroke.paintable(x, y))
                    continue;
                if (const auto cell = paintCell(stroke, x, y, 1.0); cell && !(*cell == c.at(x, y)))
                    st.stars[{y, x}] = {*cell, keys[i]};
            }
        const std::set<Pos> local = inReach(st.stars);
        for (bool went = true; went;) {
            went = false;
            for (const Pos& yx : local) {
                if (!st.stars.count(yx))
                    continue;
                std::vector<std::pair<std::pair<int, int>, Cell>> change;
                std::map<std::pair<int, int>, int> image;
                for (const auto& [x, y] : imagesOf(yx.second, yx.first))
                    if (const auto it = st.stars.find({y, x}); it != st.stars.end()) {
                        change.push_back({{x, y}, it->second.first});
                        image[{x, y}] = it->second.second;
                    }
                if (!tryKeep(change, [&](int x, int y) { return image.at({x, y}); }))
                    continue;
                for (const auto& [xy, cell] : change)
                    st.stars.erase({xy.second, xy.first});
                went = true;
            }
        }
        return;
    }

    const auto shownRung = [&](const Cell& cell) { return rungOf(ramp_, ice ? cell : iceCell(cell, false)); };
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& cells = cellsOf[i];
        if (cells.empty())
            continue;
        const auto& [x0, y0, x1, y1] = boxOf[i];
        const LiftWindow field(options_, x0, y0, x1, y1, W, H, foldX, foldY);
        for (const auto& [x, y] : cells) {
            if (!st.reached.insert({y, x}).second || !stroke.paintable(x, y))
                continue;
            const Cell& start = c.at(x, y);
            const auto r0 = shownRung(start);
            const int lift = field.at(x, y);
            if (start.transparent || !r0 || lift == 0)
                continue;
            // the solid rung the cell rounds to, plus or minus the lift
            const int base = lighten ? *r0 / kRungsPerStep * kRungsPerStep
                                     : (*r0 + kRungsPerStep - 1) / kRungsPerStep * kRungsPerStep;
            const int target = lighten ? base + lift : base - lift;
            if (target < 0 || target > ramp_.topRung() || (lighten ? target <= *r0 : target >= *r0))
                continue;
            const auto cell = stepRung(ramp_, start, target - *r0, ice);
            if (!cell || *cell == start)
                continue;
            st.pending[{y, x}] = Want{*cell, shownRung(*cell).value_or(*r0), keys[i]};
        }
    }
    // The cells this dab tries: those reached and not yet at the field, in
    // reach of one of its images. Those further off wait for a dab near
    // them.
    std::set<Pos> local = inReach(st.pending);
    if (local.empty())
        return;
    const auto imageOf = [&](int x, int y) { return st.pending.at({y, x}).image; };
    // A cell kept at the field is done: out of the round and the stroke's
    // held cells.
    const auto settle = [&](int x, int y) {
        if (const auto r = shownRung(guard.canvas().at(x, y)); r && *r == st.pending.at({y, x}).rung) {
            st.pending.erase({y, x});
            local.erase({y, x});
        }
    };
    // A cell at rung `rung`: the field's cell at its own rung, else a step
    // from where the guard's canvas has it; empty unless it comes nearer the
    // field and not past it, so every keep is a rung of the field.
    const auto cellAt = [&](const Pos& yx, int rung) -> std::optional<Cell> {
        const Want& w = st.pending.at(yx);
        const Cell& now = guard.canvas().at(yx.second, yx.first);
        const auto r = shownRung(now);
        if (!r)
            return std::nullopt;
        const auto cell = rung == w.rung ? std::optional<Cell>(w.cell) : stepRung(ramp_, now, rung - *r, ice);
        const auto to = cell ? shownRung(*cell) : std::nullopt;
        if (!to || (lighten ? *to <= *r || *to > w.rung : *to >= *r || *to < w.rung))
            return std::nullopt;
        return cell;
    };

    // Each cell short of the field and in reach is tried until none can go:
    // all of them together, then each with its mirror images by itself, a
    // rung further, in row-major order, and again together while any went,
    // so a cell a finding blamed with others, or one held back before its
    // neighbours came, goes as far as it can. The round is the cells in
    // reach of the dab and no more: what a kept cell frees further off waits
    // for a dab near it. Each keep is a rung of the field, so the round ends.
    // All of them at the field's rungs: the cells a finding blames go back
    // a rung toward where they stand, and the rest are judged again, until
    // the guard finds nothing new.
    const auto together = [&] {
        std::map<Pos, int> tries;
        for (const Pos& yx : local)
            tries[yx] = st.pending.at(yx).rung;
        while (true) {
            std::vector<std::pair<int, int>> at;  // (x, y)
            std::set<std::pair<int, int>> changed;
            for (const auto& [yx, rung] : tries) {
                const auto [y, x] = yx;
                const Cell& now = guard.canvas().at(x, y);
                const auto r = shownRung(now);
                if (!r || rung == *r)
                    continue;
                const auto cell = cellAt(yx, rung);
                if (!cell || *cell == now)
                    continue;
                next.at(x, y) = *cell;
                at.emplace_back(x, y);
                changed.insert({x, y});
            }
            if (at.empty())
                return false;
            const auto findings = guard.judge(next, at);
            if (findings.empty()) {
                guard.keep();
                for (const auto& [x, y] : at) {
                    stroke.set(x, y, next.at(x, y), 1.0, imageOf(x, y));
                    settle(x, y);
                }
                return true;
            }
            // the cells each finding blames: its own changed cells, else the
            // changed cells round it, else any
            std::set<std::pair<int, int>> blamed;
            const auto give = [&](int x, int y) {
                if (changed.count({x, y}))
                    blamed.insert({x, y});
            };
            for (const auto& f : findings) {
                const size_t before = blamed.size();
                int bx0 = std::min(f.x, f.nx), by0 = std::min(f.y, f.ny), bx1 = std::max(f.x, f.nx),
                    by1 = std::max(f.y, f.ny);
                switch (f.kind) {
                case lint::LadderFinding::Kind::Speckle:
                    give(f.x, f.y);
                    if (blamed.size() == before)
                        for (const auto& [dx, dy] : {Pos{0, -1}, Pos{0, 1}, Pos{-1, 0}, Pos{1, 0}})
                            give(f.x + dx, f.y + dy);
                    bx0 = bx1 = f.x;
                    by0 = by1 = f.y;
                    break;
                case lint::LadderFinding::Kind::RungSkip:
                    give(f.x, f.y);
                    give(f.nx, f.ny);
                    break;
                case lint::LadderFinding::Kind::WideRung:
                    for (int y = by0; y <= by1; ++y)
                        for (int x = bx0; x <= bx1; ++x)
                            give(x, y);
                    break;
                case lint::LadderFinding::Kind::OneGlyph:
                case lint::LadderFinding::Kind::Noise:
                    // read over the whole canvas: no cell is nearer them
                    blamed.insert(changed.begin(), changed.end());
                    break;
                default:
                    give(f.x, f.y);
                    bx0 = bx1 = f.x;
                    by0 = by1 = f.y;
                    break;
                }
                for (const int grow : {2, 4}) {
                    if (blamed.size() != before)
                        break;
                    for (int y = by0 - grow; y <= by1 + grow; ++y)
                        for (int x = bx0 - grow; x <= bx1 + grow; ++x)
                            give(x, y);
                }
                if (blamed.size() == before)
                    blamed.insert(changed.begin(), changed.end());
            }
            // under a mirror a cell's images give way with it
            if (mirror) {
                const auto mine = blamed;
                for (const auto& [x, y] : mine)
                    for (const auto& [mx, my] : imagesOf(x, y))
                        give(mx, my);
            }
            for (const auto& [x, y] : at)
                next.at(x, y) = guard.canvas().at(x, y);
            for (const auto& [x, y] : blamed) {
                int& rung = tries.at({y, x});
                rung += lighten ? -1 : 1;
            }
        }
    };

    // Each by itself a rung further, with its mirror images.
    const auto alone = [&] {
        bool moved = false;
        const std::vector<Pos> round(local.begin(), local.end());
        for (const Pos& yx : round) {
            if (!st.pending.count(yx))
                continue;
            std::vector<std::pair<std::pair<int, int>, Cell>> change;
            for (const auto& [x, y] : imagesOf(yx.second, yx.first)) {
                const auto it = st.pending.find({y, x});
                if (it == st.pending.end())
                    continue;
                const auto r = shownRung(guard.canvas().at(x, y));
                if (!r || *r == it->second.rung)
                    continue;
                if (const auto cell = cellAt({y, x}, *r + (lighten ? 1 : -1)); cell && !(*cell == next.at(x, y)))
                    change.push_back({{x, y}, *cell});
            }
            if (change.empty() || !tryKeep(change, imageOf))
                continue;
            moved = true;
            for (const auto& [xy, cell] : change)
                settle(xy.first, xy.second);
        }
        return moved;
    };
    for (bool went = true; went && !local.empty();) {
        went = together();
        while (!local.empty() && alone())
            went = true;
    }
}

} // namespace bw
