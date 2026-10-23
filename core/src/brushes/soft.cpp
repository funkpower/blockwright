#include "blockwright/brushes/soft.h"

#include "blockwright/brushes/guard.h"

#include "blockwright/ansi.h"
#include "blockwright/lint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

namespace bw {

namespace {

constexpr int kFar = 1 << 20;

enum : char { kOff = 0, kOn = 1, kOpen = 2 };

// How deep each cell of a shape sits in it, in bands: a column in from
// either end of its row, and no more than a row in from the top or bottom
// of its column, so each band is a cell wide wherever it runs. Where a band
// turns a corner its corner cell takes the band outside it, so no cell is
// both in the band's run across the top and in its run down the side: the
// ladder lint measures a step's width by the longest run the other way of
// any of its cells, and a stroke whose dabs drop a row puts a step at the
// end of every band across the top. A shape that is all corners (two by
// two) keeps them. Cells are off the shape, on it, or open: off it but no
// edge (the canvas's edge, a cell the stroke may not paint), so the shape
// runs on past them; so is everything outside the grid when `openOutside`.
// Cells off the shape are 0.
std::vector<int> bands(const std::vector<char>& state, int w, int h, bool openOutside) {
    auto kind = [&](int x, int y) {
        return x < 0 || y < 0 || x >= w || y >= h ? char(openOutside ? kOpen : kOff) : state[size_t(y) * w + x];
    };
    std::vector<int> n(size_t(w) * h, 0), across(n.size(), 0), down(n.size(), 0), wide(n.size(), 0),
        tall(n.size(), 0);
    // the band each cell is in by its row alone, and by its column alone,
    // and how long the shape's row and column through it run
    auto runs = [&](int lines, int len, auto cell, std::vector<int>& out, std::vector<int>& length) {
        for (int l = 0; l < lines; ++l)
            for (int i = 0; i < len;) {
                if (kind(cell(l, i).first, cell(l, i).second) == kOff) {
                    ++i;
                    continue;
                }
                int end = i;
                while (end < len && kind(cell(l, end).first, cell(l, end).second) != kOff)
                    ++end;
                const bool openStart = kind(cell(l, i - 1).first, cell(l, i - 1).second) == kOpen;
                const bool openEnd = kind(cell(l, end).first, cell(l, end).second) == kOpen;
                // how far the stroke's own cells run: a selection's or the
                // canvas's edge cuts the tip, and what's left is the shape
                int on = 0;
                for (int k = i; k < end; ++k)
                    on += kind(cell(l, k).first, cell(l, k).second) == kOn;
                for (int k = i; k < end; ++k) {
                    const auto [x, y] = cell(l, k);
                    const int in = std::min(openStart ? kFar : k - i, openEnd ? kFar : end - 1 - k);
                    out[size_t(y) * w + x] = kind(x, y) == kOpen ? kFar : 1 + in;
                    length[size_t(y) * w + x] = on;
                }
                i = end;
            }
    };
    runs(h, w, [](int l, int i) { return std::pair{i, l}; }, across, wide);
    runs(w, h, [](int l, int i) { return std::pair{l, i}; }, down, tall);
    n = across;
    for (int x = 0; x < w; ++x) {
        int above = kind(x, -1) == kOpen ? kFar : 0;
        for (int y = 0; y < h; ++y)
            above = n[size_t(y) * w + x] = std::min(n[size_t(y) * w + x], above + 1);
        int below = kind(x, h) == kOpen ? kFar : 0;
        for (int y = h - 1; y >= 0; --y)
            below = n[size_t(y) * w + x] = std::min(n[size_t(y) * w + x], below + 1);
    }
    for (int k = 0; k < w * h; ++k)
        if (state[size_t(k)] == kOff)
            n[size_t(k)] = 0;
    // The corners. The outermost band keeps a corner where the shape runs
    // three cells or fewer one way or the other: no step there is wide
    // enough to matter, and a thin stroke (a size 1 tip's line) or a small
    // tip still moves every cell.
    const std::vector<int> uncut = n;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t k = size_t(y) * w + x;
            if (int& c = n[k]; state[k] == kOn && c > 0 && c == across[k] && c == down[k] &&
                               (c > 1 || (wide[k] > 3 && tall[k] > 3)))
                --c;
        }
    bool any = false;
    for (int k = 0; k < w * h; ++k)
        any = any || (state[size_t(k)] == kOn && n[size_t(k)] > 0);
    if (!any)
        n = uncut;
    // and its neighbours stay within a band of it (cells off the shape are
    // 0, a band's own edge): passes down and back up until none moves, so
    // a shape that turns back on itself settles as its mirror image does
    auto at = [&](int x, int y) {
        const char s = kind(x, y);
        return s == kOff ? 0 : x < 0 || y < 0 || x >= w || y >= h ? kFar : n[size_t(y) * w + x];
    };
    for (bool moved = true; moved;) {
        moved = false;
        auto settle = [&](int x, int y, int a, int b) {
            if (int& c = n[size_t(y) * w + x]; state[size_t(y) * w + x] != kOff && std::min(a, b) + 1 < c) {
                c = std::min(a, b) + 1;
                moved = true;
            }
        };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                settle(x, y, at(x - 1, y), at(x, y - 1));
        for (int y = h - 1; y >= 0; --y)
            for (int x = w - 1; x >= 0; --x)
                settle(x, y, at(x + 1, y), at(x, y + 1));
    }
    return n;
}

int depthOf(int deepest, double strength, double hardness) {
    // The middle moves `wanted` rungs, as far as the bands reach; hardness
    // is the share of the bands it doesn't use, and a hard tip (1) moves
    // every cell the whole way.
    const int wanted = std::max(1, static_cast<int>(std::lround(kRungsPerStep * std::clamp(strength, 0.0, 1.0))));
    hardness = std::clamp(hardness, 0.0, 1.0);
    return hardness >= 1.0 ? wanted
                           : std::min(wanted, std::max(1, static_cast<int>(std::lround((1.0 - hardness) * deepest))));
}

// The whole tip (unclipped) as a shape on a grid of its own, and its corner.
std::vector<char> tipShape(const Dab& d, int size, Tip tip, int& x0, int& y0, int& w, int& h) {
    const auto whole = footprint(d.x, d.y, size, tip);
    w = h = 0;
    if (whole.empty())
        return {};
    x0 = whole.front().first, y0 = whole.front().second;
    int x1 = x0, y1 = y0;
    for (const auto& [x, y] : whole) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<char> state(size_t(w) * h, kOff);
    for (const auto& [x, y] : whole)
        state[size_t(y - y0) * w + (x - x0)] = kOn;
    return state;
}

} // namespace

std::vector<std::pair<std::pair<int, int>, int>> softFalloff(const Dab& d, int size, Tip tip, int width,
                                                             int height, double strength, double hardness) {
    std::vector<std::pair<std::pair<int, int>, int>> out;
    int x0 = 0, y0 = 0, w = 0, h = 0;
    const auto state = tipShape(d, size, tip, x0, y0, w, h);
    if (state.empty())
        return out;
    std::vector<int> n = bands(state, w, h, false);
    int deepest = 0;
    for (const int k : n)
        deepest = std::max(deepest, k);
    const int depth = depthOf(deepest, strength, hardness);
    for (int& k : n)
        k = std::min(depth, k);
    // No cell deeper than all four of its neighbours (cells off the tip
    // move none): a lone deepest cell is a speck.
    auto at = [&](int x, int y) { return x < 0 || y < 0 || x >= w || y >= h ? 0 : n[size_t(y) * w + x]; };
    for (bool changed = true; changed;) {
        changed = false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int most = std::max({at(x - 1, y), at(x + 1, y), at(x, y - 1), at(x, y + 1)});
                if (int& c = n[size_t(y) * w + x]; c > most && c > 1) {
                    c = std::max(1, most);
                    changed = true;
                }
            }
    }
    const bool hard = std::clamp(hardness, 0.0, 1.0) >= 1.0;
    for (const auto& [x, y] : footprint(d.x, d.y, size, tip, width, height, d.flipX, d.flipY)) {
        // a flipped tip is the whole tip flipped about the dab's cell
        const int tx = d.flipX ? 2 * d.x - x : x;
        const int ty = d.flipY ? 2 * d.y - y : y;
        // a corner the falloff takes off the tip moves none
        if (const int k = hard ? depth : n[size_t(ty - y0) * w + (tx - x0)]; k > 0)
            out.push_back({{x, y}, k});
    }
    return out;
}

SoftShadeBrush::SoftShadeBrush(Ramp ramp, ShadeDirection direction, double hardness)
    : ramp_(std::move(ramp)), direction_(direction), hardness_(std::clamp(hardness, 0.0, 1.0)) {}

namespace {

// How far round a dab's footprint a cell may move back from where the last
// dab left it, how far its band's cap is lifted (the bands and the falloff
// through them reach this far), how much further a cell may rise with them
// (a rung a cell, four at most), and the context round that the bands read.
constexpr int kFresh = 4;
constexpr int kReset = 9;
constexpr int kSettle = 4;
constexpr int kContext = 13;
constexpr signed char kNoCap = 127;

thread_local long long visited = 0;

struct Rect {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    Rect grown(int d, int w, int h) const {
        return {std::max(0, x0 - d), std::max(0, y0 - d), std::min(w - 1, x1 + d), std::min(h - 1, y1 + d)};
    }
    bool has(int x, int y) const { return x >= x0 && y >= y0 && x <= x1 && y <= y1; }
    // overlapping or side by side
    bool touches(const Rect& r) const { return r.x0 <= x1 + 1 && x0 <= r.x1 + 1 && r.y0 <= y1 + 1 && y0 <= r.y1 + 1; }
    Rect hull(const Rect& r) const {
        return {std::min(x0, r.x0), std::min(y0, r.y0), std::max(x1, r.x1), std::max(y1, r.y1)};
    }
};

bool inAny(const std::vector<Rect>& rects, int x, int y) {
    for (const Rect& r : rects)
        if (r.has(x, y))
            return true;
    return false;
}

} // namespace

// What the soft brush keeps over a stroke: for the cells it has covered,
// the depth the strongest dab wanted, the rungs the cell has moved from
// its colour at the stroke's start, and the cap the lint left it; the
// mirror images solved since the stroke's own dab; the stroke's lint guard,
// which holds the canvas as the last dab left it; and a copy of that a
// solve works on, so a dab copies no canvas.
struct SoftShadeBrush::State {
    struct Entry {
        signed char want = -1, moved = 0, cap = kNoCap;
    };
    std::unordered_map<size_t, Entry> cells;
    Entry get(size_t k) const {
        const auto it = cells.find(k);
        return it == cells.end() ? Entry{} : it->second;
    }
    std::set<std::tuple<int, int, bool, bool, int, double>> solved;
    int lastImage = 0;
    std::optional<StrokeLint> lint;
    std::optional<Canvas> out;
};

long long SoftShadeBrush::cellsVisited() { return visited; }

void SoftShadeBrush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    const Canvas& c = stroke.canvas();
    const int W = c.width(), H = c.height();
    const Tip tip = stroke.settings().tip;
    // The tip's deepest band, the same wherever it is: worked out on the
    // whole tip at the origin, never one a canvas's edge or int's ends cut.
    static thread_local std::map<std::pair<int, int>, int> deepestOf;
    auto [it, fresh] = deepestOf.try_emplace({size, int(tip)}, 0);
    if (fresh) {
        int x0 = 0, y0 = 0, w = 0, h = 0;
        const auto state = tipShape(Dab{0, 0}, size, tip, x0, y0, w, h);
        for (const int k : bands(state, w, h, false))
            it->second = std::max(it->second, k);
    }
    const auto depth = static_cast<signed char>(depthOf(it->second, strength, hardness_));
    auto& slot = stroke.brushState();
    if (!slot)
        slot = std::make_shared<State>();
    State& st = *static_cast<State*>(slot.get());
    // Under a mirror the dab's images are shaded together, in one solve that
    // treats each as the mirror of the others, so a symmetric canvas stays
    // symmetric. The stroke stamps its own dab first, then each image's: an
    // image a solve has shaded since the stroke's own last stamp is done.
    std::vector<Dab> images{d};
    const SymmetryMode mode = stroke.options().symmetry.mode;
    if (mode == SymmetryMode::MirrorX || mode == SymmetryMode::MirrorY || mode == SymmetryMode::MirrorXY) {
        if (d.image == 0 && st.lastImage != 0)
            st.solved.clear();
        st.lastImage = d.image;
        if (st.solved.count({d.x, d.y, d.flipX, d.flipY, size, strength}))
            return;
        images = symmetryDabs(stroke.options().symmetry, d, W, H);
        for (const Dab& o : images)
            st.solved.insert({o.x, o.y, o.flipX, o.flipY, size, strength});
    }
    // Each image's footprint, as a box, where it may paint any of it; its
    // cells want the depth of the strongest dab over them.
    std::vector<Rect> boxes;
    bool deeper = false;
    for (const Dab& o : images) {
        Rect u{W, H, -1, -1};
        bool any = false;
        for (const auto& [x, y] : footprint(o.x, o.y, size, tip, W, H, o.flipX, o.flipY)) {
            u = {std::min(u.x0, x), std::min(u.y0, y), std::max(u.x1, x), std::max(u.y1, y)};
            if (!stroke.paintable(x, y))
                continue;
            any = true;
            signed char& want = st.cells[static_cast<size_t>(y) * W + x].want;
            if (want >= depth)
                continue;
            want = depth;
            deeper = true;
            // kept as it is for now; shaded below
            stroke.set(x, y, *stroke.cellAt(x, y), depth);
        }
        if (any)
            boxes.push_back(u);
    }
    // A dab that wants nothing deeper than the stroke already does (going
    // over it again, at the same or a lower strength) changes nothing.
    if (boxes.empty() || !deeper)
        return;
    std::vector<std::array<int, 4>> around;
    for (const Rect& u : boxes)
        around.push_back({u.x0, u.y0, u.x1, u.y1});
    if (shade(stroke, st, around, whole_))
        return;
    // A dab that lays nothing is tried again with the wants of the stroke's
    // cells it hasn't moved set aside, outside this dab's footprints: an
    // earlier dab that laid nothing holds no later one back. They are put
    // back after, so dabs that are specks alone still make a line together.
    // This solve is round the dab even when the whole stroke is re-shaded:
    // it's for this dab's cells, and settling the rest with wants set aside
    // could move cells far off that the dab's own solve leaves.
    std::vector<std::pair<size_t, signed char>> aside;
    std::set<size_t> mine;
    for (const Dab& o : images)
        for (const auto& [x, y] : footprint(o.x, o.y, size, tip, W, H, o.flipX, o.flipY))
            mine.insert(static_cast<size_t>(y) * W + x);
    for (const Rect& u : boxes) {
        const Rect g = u.grown(kReset + kSettle + kContext, W, H);
        for (int y = g.y0; y <= g.y1; ++y)
            for (int x = g.x0; x <= g.x1; ++x) {
                const size_t i = static_cast<size_t>(y) * W + x;
                const auto it = st.cells.find(i);
                ++visited;
                if (it == st.cells.end() || it->second.want < 0 || it->second.moved != 0 || mine.count(i))
                    continue;
                aside.emplace_back(i, it->second.want);
                it->second.want = -1;
            }
    }
    if (aside.empty())
        return;
    shade(stroke, st, around, false);
    for (const auto& [i, want] : aside)
        st.cells[i].want = want;
}

int SoftShadeBrush::reach(const Cell& cell, int want, bool ice) const {
    // With iCE off a cell climbs only as far as the rungs it can write run
    // unbroken from its own, so it never jumps a rung it can't.
    const Cell shown = ice ? cell : iceCell(cell, false);
    const auto rung = rungOf(ramp_, shown);
    if (!rung)
        return 0;
    const ShadeForm form = shadeForm(ramp_, shown).value_or(ShadeForm::LightOnTop);
    const int dir = direction_ == ShadeDirection::Lighten ? 1 : -1;
    int k = 0;
    while (k < want && *rung + dir * (k + 1) >= 0 && *rung + dir * (k + 1) <= ramp_.topRung() &&
           (ice || rungCell(ramp_, *rung + dir * (k + 1), form).bg.paletteIndex() < 8))
        ++k;
    return k;
}

bool SoftShadeBrush::shade(Stroke& stroke, State& st, const std::vector<std::array<int, 4>>& around,
                           bool whole) const {
    const Canvas& canvas = stroke.canvas();
    const int W = canvas.width(), H = canvas.height();
    const Rect all{0, 0, W - 1, H - 1};
    // Round each box, the cells near it are shaded again from the stroke's
    // start and the rest no further back than the last dab left them, and
    // four beyond those settle round them; the bands read the context round
    // those. Shading the whole stroke (`reshadeWhole`, used only by tests)
    // settles every cell of it, with bands over the whole canvas, and gives
    // the same but for rare cells at the edge of those the near solve
    // settles; a test pins the known ones.
    std::vector<Rect> fresh, reset, settle, regions;
    for (const auto& b : around) {
        const Rect u{b[0], b[1], b[2], b[3]};
        fresh.push_back(u.grown(kFresh, W, H));
        reset.push_back(u.grown(kReset, W, H));
        settle.push_back(whole ? all : u.grown(kReset + kSettle, W, H));
        regions.push_back(settle.back().grown(kContext, W, H));
    }
    if (whole)
        regions = {all};
    // Mirror images whose regions overlap or touch are solved on one grid
    // round them all, the rest each on its own, so a dab's work doesn't
    // grow with the gap between its images. A stroke cell between them
    // settles in no region and holds where it is either way.
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < regions.size() && !merged; ++i)
            for (size_t j = i + 1; j < regions.size() && !merged; ++j)
                if (regions[i].touches(regions[j])) {
                    regions[i] = regions[i].hull(regions[j]);
                    regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                }
    }
    struct Grid {
        Rect r;
        int w, h;
        size_t off;
        size_t at(int x, int y) const { return off + static_cast<size_t>(y - r.y0) * w + (x - r.x0); }
    };
    std::vector<Grid> grids;
    size_t n = 0;
    for (const Rect& r : regions) {
        grids.push_back({r, r.x1 - r.x0 + 1, r.y1 - r.y0 + 1, n});
        n += static_cast<size_t>(grids.back().w) * grids.back().h;
    }
    // a canvas cell's index in the grids, n off them
    auto index = [&](int x, int y) {
        for (const Grid& gr : grids)
            if (gr.r.has(x, y))
                return gr.at(x, y);
        return n;
    };
    auto each = [&](auto&& f) {
        for (const Grid& gr : grids)
            for (int y = gr.r.y0; y <= gr.r.y1; ++y)
                for (int x = gr.r.x0; x <= gr.r.x1; ++x)
                    f(gr.at(x, y), x, y);
    };
    const bool ice = stroke.options().ice;
    const int dir = direction_ == ShadeDirection::Lighten ? 1 : -1;

    // The canvas as the last dab left it and as this one would, kept over
    // the stroke (copied once, at its first solve), and the rungs each
    // stroke cell may move: no more than it wants, can, or its band in the
    // stroke allows (-1 off the stroke). The stroke's cells past the
    // settling ones stay where they are, and those settle round them.
    if (!st.lint) {
        // The stroke's guard, read from its start (the canvas as the last
        // dab left it, as no dab has changed a cell yet): the lint index,
        // the guard's copy and the solve's.
        st.lint.emplace(canvas, ramp_);
        st.out = canvas;
        visited += 3 * static_cast<long long>(W) * H;
    }
    const Canvas& before = st.lint->canvas();
    Canvas& out = *st.out;
    std::vector<signed char> most(n, -1), prev(n, 0), kept(n, kNoCap);
    std::vector<char> settling(n, 0), back(n, 0);
    each([&](size_t k, int x, int y) {
        const auto e = st.get(static_cast<size_t>(y) * W + x);
        if (e.want < 0)
            return;
        prev[k] = e.moved;
        back[k] = inAny(fresh, x, y);
        kept[k] = inAny(reset, x, y) ? kNoCap : e.cap;
        settling[k] = inAny(settle, x, y);
        most[k] = static_cast<signed char>(reach(canvas.at(x, y), e.want, ice));
        ++visited;  // the rung reach reads
    });
    visited += static_cast<long long>(n);
    // The stroke's bands, from its edge with the cells it could paint and
    // didn't: the canvas's edge and cells it may not paint are no edge, the
    // tip runs on past them. A hard tip has none.
    if (hardness_ < 1.0)
        for (const Grid& gr : grids) {
            const size_t m = static_cast<size_t>(gr.w) * gr.h;
            std::vector<char> state(m, kOff);
            for (int y = gr.r.y0; y <= gr.r.y1; ++y)
                for (int x = gr.r.x0; x <= gr.r.x1; ++x)
                    state[gr.at(x, y) - gr.off] = most[gr.at(x, y)] >= 0 ? kOn : stroke.paintable(x, y) ? kOff : kOpen;
            const auto band = bands(state, gr.w, gr.h, true);
            for (size_t k = 0; k < m; ++k)
                if (most[gr.off + k] >= 0)
                    most[gr.off + k] = static_cast<signed char>(std::min<int>(most[gr.off + k], band[k]));
            visited += 6 * static_cast<long long>(m);
        }
    for (size_t k = 0; k < n; ++k)
        if (most[k] >= 0 && !settling[k])
            most[k] = prev[k];
    visited += static_cast<long long>(n);
    // Neighbours on the stroke at most a rung apart, each settling cell as
    // far as that lets it: a pressure change ramps, at any hardness. No
    // stroke cell of one grid neighbours a settling cell of another.
    auto ramp = [&](std::vector<signed char>& v) {
        for (const Grid& gr : grids) {
            const int w = gr.w, h = gr.h;
            auto at = [&](int x, int y) { return gr.off + static_cast<size_t>(y) * w + x; };
            for (bool changed = true; changed;) {
                changed = false;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        if (signed char& c = v[at(x, y)]; c >= 0 && settling[at(x, y)]) {
                            if (x > 0 && v[at(x - 1, y)] >= 0 && c > v[at(x - 1, y)] + 1)
                                c = v[at(x - 1, y)] + 1, changed = true;
                            if (y > 0 && v[at(x, y - 1)] >= 0 && c > v[at(x, y - 1)] + 1)
                                c = v[at(x, y - 1)] + 1, changed = true;
                        }
                for (int y = h - 1; y >= 0; --y)
                    for (int x = w - 1; x >= 0; --x)
                        if (signed char& c = v[at(x, y)]; c >= 0 && settling[at(x, y)]) {
                            if (x + 1 < w && v[at(x + 1, y)] >= 0 && c > v[at(x + 1, y)] + 1)
                                c = v[at(x + 1, y)] + 1, changed = true;
                            if (y + 1 < h && v[at(x, y + 1)] >= 0 && c > v[at(x, y + 1)] + 1)
                                c = v[at(x, y + 1)] + 1, changed = true;
                        }
                visited += 2 * static_cast<long long>(w) * h;
            }
        }
    };
    ramp(most);

    std::vector<signed char> moved(n, 0), cap = kept;
    // Where no tones that way are clean round the cells that stay (the
    // last dab left them where they are), every cell settles no further
    // back than the last dab left it.
    bool floor = false;
    auto rungAt = [&](int x, int y) -> std::optional<int> {
        if (x < 0 || y < 0 || x >= W || y >= H)
            return std::nullopt;
        ++visited;
        return rungOf(ramp_, out.at(x, y));
    };
    // how far along the brush's way a cell sits: lighter for lighten
    auto tone = [&](int x, int y) { return dir * rungAt(x, y).value_or(0); };
    auto changed = [&](int x, int y) {
        const size_t k = index(x, y);
        return k < n && most[k] >= 0 && settling[k] && moved[k] > (floor || !back[k] ? prev[k] : 0);
    };
    const SymmetryMode mode = stroke.options().symmetry.mode;
    const bool mirror =
        mode == SymmetryMode::MirrorX || mode == SymmetryMode::MirrorY || mode == SymmetryMode::MirrorXY;
    bool clean = false;
    for (;;) {
        // Each cell as far as its limit and its neighbours let it.
        std::vector<signed char> v(n, -1);
        for (size_t k = 0; k < n; ++k)
            if (most[k] >= 0)
                v[k] = std::min(most[k], cap[k]);
        visited += static_cast<long long>(n);
        ramp(v);
        std::vector<std::pair<int, int>> differ;
        each([&](size_t k, int x, int y) {
            if (most[k] < 0)
                return;
            moved[k] = !settling[k] ? prev[k] : floor || !back[k] ? std::max(prev[k], v[k]) : v[k];
            const Cell& from = canvas.at(x, y);
            const auto to = moved[k] > 0 ? stepRung(ramp_, from, dir * moved[k], ice) : std::nullopt;
            out.at(x, y) = to ? *to : from;
            visited += 2;  // the rung stepped, and the cell against the last dab's
            if (!(out.at(x, y) == before.at(x, y)))
                differ.emplace_back(x, y);
        });
        visited += static_cast<long long>(n);
        // Whatever the guard finds that the start didn't have gives way, by
        // the tones it left: the cell that stands out of a speck, or the ones
        // that rose round a speck that sinks; the higher side of a skip; a
        // wide step's own cells; for one glyph and noise, any of the dab's.
        // The guard reads every row and column through the cells that differ
        // from the last dab's. Only settling cells give way, a rung at a
        // time.
        std::set<size_t> lower;
        bool found = false;
        for (const auto& f : st.lint->judge(out, differ)) {
            found = true;
            // each finding's own cells, whatever the others give way
            std::set<size_t> mine;
            auto give = [&](int x, int y) {
                if (changed(x, y))
                    mine.insert(index(x, y));
            };
            const int fx = f.x, fy = f.y;
            Rect where{fx, fy, fx, fy};
            constexpr int kNeighbours[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
            switch (f.kind) {
            case lint::LadderFinding::Kind::Speckle: {
                bool above = true;
                for (const auto& d : kNeighbours)
                    if (rungAt(fx + d[0], fy + d[1]))
                        above = above && tone(fx + d[0], fy + d[1]) < tone(fx, fy);
                if (above)
                    give(fx, fy);
                else
                    for (const auto& d : kNeighbours)
                        if (tone(fx + d[0], fy + d[1]) > tone(fx, fy))
                            give(fx + d[0], fy + d[1]);
                break;
            }
            case lint::LadderFinding::Kind::RungSkip: {
                const bool far = tone(f.nx, f.ny) > tone(fx, fy);
                give(far ? f.nx : fx, far ? f.ny : fy);
                where = {std::min(fx, f.nx), std::min(fy, f.ny), std::max(fx, f.nx), std::max(fy, f.ny)};
                break;
            }
            case lint::LadderFinding::Kind::WideRung:
                // the run, from its first cell to its last, or where none of
                // those moved, the dab's cells along its line: a dab giving
                // way next to a run a cell at a time can join the run a cell
                // at a time
                where = {fx, fy, f.nx, f.ny};
                for (int y = where.y0; y <= where.y1; ++y)
                    for (int x = where.x0; x <= where.x1; ++x)
                        give(x, y);
                if (mine.empty())
                    each([&](size_t, int x, int y) {
                        if (f.ny == fy ? y == fy : x == fx)
                            give(x, y);
                    });
                break;
            case lint::LadderFinding::Kind::OneGlyph:
            case lint::LadderFinding::Kind::Noise:
                // read over the whole canvas, so no cell of the dab is
                // nearer them than another
                each([&](size_t, int x, int y) { give(x, y); });
                break;
            default:
                break;
            }
            // where the finding's own cells can't, those nearest it do, and
            // then any of the dab's
            for (const int d : {2, 4}) {
                if (!mine.empty())
                    break;
                const Rect near = where.grown(d, W, H);
                for (int y = near.y0; y <= near.y1; ++y)
                    for (int x = near.x0; x <= near.x1; ++x)
                        give(x, y);
            }
            if (mine.empty())
                each([&](size_t, int x, int y) { give(x, y); });
            lower.insert(mine.begin(), mine.end());
        }
        // Under a mirror a cell's images give way with it, so a solve that
        // meets the findings in one order on one side and another on the
        // other still leaves a symmetric canvas symmetric.
        if (mirror && !lower.empty()) {
            const std::set<size_t> mine = lower;
            each([&](size_t k, int x, int y) {
                if (!mine.count(k))
                    return;
                for (const Dab& o : symmetryDabs(stroke.options().symmetry, Dab{x, y}, W, H))
                    if (changed(o.x, o.y))
                        lower.insert(index(o.x, o.y));
            });
        }
        if (!found) {
            // A clean solve that takes a cell back from where the last dab
            // left it is tried again no further back anywhere: a stroke never
            // takes paint off.
            bool behind = false;
            for (size_t k = 0; k < n && !floor; ++k)
                behind = behind || (most[k] >= 0 && moved[k] < prev[k]);
            if (behind) {
                floor = true;
                cap = kept;
                continue;
            }
            clean = true;
            break;
        }
        if (lower.empty()) {
            if (floor)
                break;
            floor = true;
            cap = kept;
            continue;
        }
        for (const size_t k : lower)
            cap[k] = static_cast<signed char>(moved[k] - 1);
    }
    // A dab whose cells can't settle without a finding the start didn't
    // have gives way entirely: every cell stays where the last dab left it.
    // The guard keeps the clean solve it judged last, so it holds what the
    // stroke does; it keeps it after the cells are compared with the last
    // dab's, which keep() overwrites.
    bool laid = false;
    each([&](size_t k, int x, int y) {
        if (most[k] < 0)
            return;
        if (!clean || !settling[k]) {
            out.at(x, y) = before.at(x, y);
            return;
        }
        auto& e = st.cells[static_cast<size_t>(y) * W + x];
        laid = laid || !(out.at(x, y) == before.at(x, y));
        e.moved = moved[k];
        e.cap = cap[k];
        if (!(*stroke.cellAt(x, y) == out.at(x, y)))
            stroke.set(x, y, out.at(x, y), e.want);
    });
    if (clean)
        st.lint->keep();
    visited += static_cast<long long>(n);
    return laid;
}

} // namespace bw
