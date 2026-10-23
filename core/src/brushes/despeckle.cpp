#include "blockwright/brushes/despeckle.h"

#include "blockwright/brushes/guard.h"

#include "blockwright/ansi.h"
#include "blockwright/palette.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace bw {

namespace {

// below, above, left, right: a speck takes the colour below first
constexpr int kNeighbours[4][2] = {{0, 1}, {0, -1}, {-1, 0}, {1, 0}};

} // namespace

std::vector<int> speckFixes(const Ramp& ramp, int x, int y,
                            const std::function<std::optional<int>(int, int)>& rungAt) {
    const auto own = rungAt(x, y);
    if (!own || *own == ramp.topRung())
        return {};
    const int g = *own;
    if (g % kRungsPerStep == 0 && ramp.colours[static_cast<size_t>(g / kRungsPerStep)] == Black)
        return {};
    int seen = 0, above = 0, below = 0;
    std::map<int, int> votes;
    for (const auto& d : kNeighbours) {
        const auto n = rungAt(x + d[0], y + d[1]);
        if (!n)
            continue;
        ++seen;
        above += *n > g;
        below += *n < g;
        ++votes[*n];
    }
    if (seen < 3 || (above != seen && below != seen))
        return {};
    std::vector<int> out;
    for (const auto& [rung, count] : votes)
        out.push_back(rung);
    // the most shared, then the nearest, then the darker (the map's order)
    std::stable_sort(out.begin(), out.end(), [&](int a, int b) {
        if (votes[a] != votes[b])
            return votes[a] > votes[b];
        return std::abs(a - g) < std::abs(b - g);
    });
    // After the first, not the far end of the neighbours' rungs (the
    // highest for a dark speck, the lowest for a light one) when one
    // neighbour alone holds it: a near-white speck taken to its one black
    // neighbour reads as a hole, so it is left rather than fixed that way.
    // The near end is the gentlest fix there is and stays.
    const int far = below == seen ? votes.begin()->first : votes.rbegin()->first;
    if (votes[far] == 1)
        out.erase(std::remove(out.begin() + 1, out.end(), far), out.end());
    return out;
}

std::optional<int> speckFix(const Ramp& ramp, int x, int y,
                            const std::function<std::optional<int>(int, int)>& rungAt) {
    const auto all = speckFixes(ramp, x, y, rungAt);
    if (all.empty())
        return std::nullopt;
    return all.front();
}

DespeckleBrush::DespeckleBrush(Ramp ramp) : ramp_(std::move(ramp)) {}

void DespeckleBrush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    const Canvas& c = stroke.canvas();
    const int W = c.width(), H = c.height();
    const Tip tip = stroke.settings().tip;
    const bool ice = stroke.options().ice;
    auto rungAt = [&](int x, int y) -> std::optional<int> {
        const auto cell = stroke.cellAt(x, y);
        if (!cell)
            return std::nullopt;
        return rungOf(ramp_, ice ? *cell : iceCell(*cell, false));
    };
    // The stroke's lint guard, read from its start at the first dab, and
    // the canvas a fix is judged on (the guard's, between fixes).
    struct State {
        std::set<std::tuple<int, int, bool, bool, int, double>> done;
        int lastImage = 0;
        std::optional<StrokeLint> guard;
        std::optional<Canvas> next;
    };
    auto& slot = stroke.brushState();
    if (!slot)
        slot = std::make_shared<State>();
    State& st = *static_cast<State*>(slot.get());
    if (!st.guard) {
        st.guard.emplace(c, ramp_);
        st.next = c;
    }
    // Under a mirror the dab's images are cleaned together, each cell with
    // its images, so a symmetric canvas stays symmetric. The stroke stamps
    // its own dab first, then each image's: an image cleaned since the
    // stroke's own last stamp is done.
    std::vector<Dab> images{d};
    const Symmetry& sym = stroke.options().symmetry;
    const bool mirror =
        sym.mode == SymmetryMode::MirrorX || sym.mode == SymmetryMode::MirrorY || sym.mode == SymmetryMode::MirrorXY;
    if (mirror) {
        if (d.image == 0 && st.lastImage != 0)
            st.done.clear();
        st.lastImage = d.image;
        if (st.done.count({d.x, d.y, d.flipX, d.flipY, size, strength}))
            return;
        images = symmetryDabs(sym, d, W, H);
        for (const Dab& o : images)
            st.done.insert({o.x, o.y, o.flipX, o.flipY, size, strength});
    }
    using Pos = std::pair<int, int>;
    std::set<Pos> own, all;
    for (const auto& p : footprint(d.x, d.y, size, tip, W, H, d.flipX, d.flipY))
        own.insert(p);
    for (const Dab& o : images)
        for (const auto& p : footprint(o.x, o.y, size, tip, W, H, o.flipX, o.flipY))
            all.insert(p);
    // The cells in the order the dab's own tip reads them: by rows from the
    // top and left to right, turned over with a flipped tip. A cell's images
    // go with it, so each image reads its cells as the mirror of the dab.
    auto order = [&](const Pos& p) { return Pos{d.flipY ? -p.second : p.second, d.flipX ? -p.first : p.first}; };
    struct Member {
        Pos at;
        bool flipX, flipY;  // turned over from the dab's own tip
    };
    std::map<Pos, std::vector<Member>> groups;
    for (const Pos& p : all) {
        std::vector<Pos> with;
        for (const Dab& o : mirror ? symmetryDabs(sym, Dab{p.first, p.second}, W, H) : std::vector<Dab>{Dab{p.first, p.second}})
            if (all.count({o.x, o.y}))
                with.push_back({o.x, o.y});
        // the image of the cell under the dab's own tip, else the first
        Pos first = with.front();
        bool mine = false;
        for (const Pos& q : with) {
            const bool in = own.count(q) != 0;
            if ((in && !mine) || (in == mine && order(q) < order(first)))
                first = q, mine = in;
        }
        groups[order(first)].push_back({p, p.first != first.first, p.second != first.second});
    }
    // A fix can make a neighbour a speck (a cluster's next cell): go over
    // the dab again until it is clean. Each fix moves a cell to a rung it
    // has a neighbour on, so this ends; the bound is a backstop. A fix (a
    // cell with its images) that would add a lint finding the stroke's
    // start didn't have, by the guard (one glyph or noise, a skip or a wide
    // rung the speck's new rung makes), gives way to the next rung, and is
    // left undone when every one would.
    Canvas& next = *st.next;
    for (size_t pass = 0; pass <= all.size(); ++pass) {
        bool changed = false;
        for (const auto& [key, members] : groups) {
            // The cell and its images judged from the same cells, then set:
            // each at its first rung (speckFixes), and where the guard
            // refuses that, each at its next.
            std::vector<std::vector<int>> rungs;
            for (const Member& m : members)
                rungs.push_back(stroke.paintable(m.at.first, m.at.second)
                                    ? speckFixes(ramp_, m.at.first, m.at.second, rungAt)
                                    : std::vector<int>{});
            for (size_t k = 0;; ++k) {
                std::vector<std::pair<Pos, Cell>> fixes;
                bool any = false;
                for (size_t i = 0; i < members.size(); ++i) {
                    if (k >= rungs[i].size())
                        continue;
                    any = true;
                    const Member& m = members[i];
                    const auto [x, y] = m.at;
                    for (const auto& n : kNeighbours) {
                        const int nx = x + ((m.flipX != d.flipX) ? -n[0] : n[0]);
                        const int ny = y + ((m.flipY != d.flipY) ? -n[1] : n[1]);
                        const auto cell = stroke.cellAt(nx, ny);
                        if (cell && rungAt(nx, ny) == rungs[i][k]) {
                            // the neighbour's look, the cell's own transparency
                            Cell out = *cell;
                            out.transparent = stroke.cellAt(x, y)->transparent;
                            fixes.emplace_back(m.at, out);
                            break;
                        }
                    }
                }
                if (!any)
                    break;
                std::vector<Pos> at;
                for (const auto& [p, out] : fixes)
                    if (!(next.at(p.first, p.second) == out)) {
                        next.at(p.first, p.second) = out;
                        at.push_back(p);
                    }
                if (at.empty())
                    break;
                if (!st.guard->judge(next, at).empty()) {
                    for (const Pos& p : at)
                        next.at(p.first, p.second) = st.guard->canvas().at(p.first, p.second);
                    continue;
                }
                st.guard->keep();
                for (const auto& [p, out] : fixes) {
                    stroke.set(p.first, p.second, out, strength);
                    changed = true;
                }
                break;
            }
        }
        if (!changed)
            return;
    }
}

} // namespace bw
