#include "blockwright/brushes/smudge.h"

#include "blockwright/brushes/guard.h"
#include "blockwright/brushes/shading.h"

#include "blockwright/ansi.h"
#include "blockwright/palette.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace bw {

double smudgeMix(int rung, double carried, double strength) { return rung + strength * (carried - rung); }

int smudgeRung(double value, int from) {
    return int(value > from ? std::floor(value + 0.5) : std::ceil(value - 0.5));
}

SmudgeBrush::SmudgeBrush(Ramp ramp) : ramp_(std::move(ramp)) {}

void SmudgeBrush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    const Canvas& c = stroke.canvas();
    const int W = c.width(), H = c.height();
    const Tip tip = stroke.settings().tip;
    const bool ice = stroke.options().ice;
    using Pos = std::pair<int, int>;
    // The stroke's lint guard, read from its start at the first dab, the
    // canvas a stamp is judged on (the guard's, between stamps), and what
    // each tip cell carries: by image, then by the cell's place in the tip.
    struct State {
        std::optional<StrokeLint> guard;
        std::optional<Canvas> next;
        std::map<int, std::map<Pos, double>> carry;
    };
    auto& slot = stroke.brushState();
    if (!slot)
        slot = std::make_shared<State>();
    State& st = *static_cast<State*>(slot.get());
    if (!st.guard) {
        st.guard.emplace(c, ramp_);
        st.next = c;
    }
    // Under a mirror the dab's images are smudged together from the dab
    // itself (the stroke's own image), so they read the same cells; the
    // stroke's stamps of the other images are done with it.
    const Symmetry& sym = stroke.options().symmetry;
    const bool mirror =
        sym.mode == SymmetryMode::MirrorX || sym.mode == SymmetryMode::MirrorY || sym.mode == SymmetryMode::MirrorXY;
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
    // A cell's rung as the stroke has left it and as it shows, or empty
    // where the smear stops: off the ramp, black (ink), transparent (it
    // shows the layer beneath), or not the stroke's to paint.
    auto rungAt = [&](int x, int y) -> std::optional<int> {
        if (!stroke.paintable(x, y))
            return std::nullopt;
        const auto cell = stroke.cellAt(x, y);
        if (!cell || cell->transparent)
            return std::nullopt;
        const auto g = rungOf(ramp_, ice ? *cell : iceCell(*cell, false));
        if (!g || (*g % kRungsPerStep == 0 && ramp_.colours[size_t(*g / kRungsPerStep)] == Black))
            return std::nullopt;
        return g;
    };
    // What each image's tip lays on each cell, from the cells as the stamp
    // found them.
    struct Proposal {
        size_t image;
        Pos offset;
    };
    std::map<Pos, std::vector<std::pair<Proposal, double>>> laid;  // (y, x) order
    std::vector<std::map<Pos, double>> carried(images.size());
    std::map<Pos, int> rungs;
    for (size_t i = 0; i < images.size(); ++i) {
        const Dab& o = images[i];
        const auto& had = st.carry[keys[i]];
        for (const auto& [x, y] : footprint(o.x, o.y, size, tip, W, H, o.flipX, o.flipY)) {
            const auto g = rungAt(x, y);
            if (!g)
                continue;  // the tip cell drops what it carried
            const Pos off{x - o.x, y - o.y};
            rungs[{y, x}] = *g;
            const auto it = had.find(off);
            if (it == had.end()) {
                carried[i][off] = *g;  // picked up, nothing laid
                continue;
            }
            laid[{y, x}].push_back({{i, off}, smudgeMix(*g, it->second, strength)});
        }
    }
    // Each cell takes the mean of what its images lay, which each of them
    // carries on; the rung it is written at. The values are summed in
    // sorted order: a cell's mirror partners take the same values from
    // their images in another order, and floating-point addition of three
    // or more terms depends on it, so an unsorted sum can land a mean of
    // 1.5 on one side and 1.4999999999999998 on the other and round them
    // apart.
    struct Change {
        Pos at;  // (x, y)
        int from, to;
        int image;
    };
    std::vector<Change> changes;
    std::vector<double> values;
    for (const auto& [yx, ps] : laid) {
        values.clear();
        for (const auto& [p, v] : ps)
            values.push_back(v);
        std::sort(values.begin(), values.end());
        double sum = 0;
        for (const double v : values)
            sum += v;
        const double mean = sum / double(ps.size());
        int image = int(ps.front().first.image);
        for (const auto& [p, v] : ps) {
            carried[p.image][p.offset] = mean;
            image = std::min(image, int(p.image));
        }
        const int from = rungs[yx];
        const int to = std::clamp(smudgeRung(mean, from), 0, ramp_.topRung());
        if (to != from)
            changes.push_back({{yx.second, yx.first}, from, to, keys[size_t(image)]});
    }
    for (size_t i = 0; i < images.size(); ++i)
        st.carry[keys[i]] = std::move(carried[i]);
    if (changes.empty())
        return;

    // The cell a change writes `k` rungs short of its target, back toward
    // where it was; empty when that is where it was (or iCE off can't
    // write it).
    auto cellFor = [&](const Change& ch, int k) -> std::optional<Cell> {
        const int delta = ch.to - ch.from;
        const int steps = delta > 0 ? std::max(0, delta - k) : std::min(0, delta + k);
        if (steps == 0)
            return std::nullopt;
        return stepRung(ramp_, *stroke.cellAt(ch.at.first, ch.at.second), steps, ice);
    };
    Canvas& next = *st.next;
    // Lays `which` changes `k` rungs short on `next` and asks the guard;
    // keeps and writes them if it adds no finding, else puts `next` back.
    auto attempt = [&](const std::vector<size_t>& which, int k) {
        std::vector<Pos> at;
        std::vector<std::pair<size_t, Cell>> out;
        for (size_t i : which)
            if (const auto cell = cellFor(changes[i], k)) {
                const auto [x, y] = changes[i].at;
                if (next.at(x, y) == *cell)
                    continue;
                next.at(x, y) = *cell;
                at.push_back({x, y});
                out.push_back({i, *cell});
            }
        if (at.empty())
            return true;
        if (!st.guard->judge(next, at).empty()) {
            for (const Pos& p : at)
                next.at(p.first, p.second) = st.guard->canvas().at(p.first, p.second);
            return false;
        }
        st.guard->keep();
        for (const auto& [i, cell] : out)
            stroke.set(changes[i].at.first, changes[i].at.second, cell, strength, changes[i].image);
        return true;
    };
    std::vector<size_t> all(changes.size());
    for (size_t i = 0; i < all.size(); ++i)
        all[i] = i;
    if (attempt(all, 0))
        return;
    // The guard refused the stamp: it gives way a cell at a time, with its
    // mirror images, in rows from the top, each a rung at a time back toward
    // where it was, and a cell no rung of which is clean is left.
    std::map<Pos, size_t> index;
    for (size_t i = 0; i < changes.size(); ++i)
        index[changes[i].at] = i;
    std::set<size_t> done;
    for (size_t i = 0; i < changes.size(); ++i) {
        if (done.count(i))
            continue;
        std::vector<size_t> orbit{i};
        if (mirror)
            for (const Dab& o : symmetryDabs(sym, Dab{changes[i].at.first, changes[i].at.second}, W, H)) {
                const auto it = index.find({o.x, o.y});
                if (it != index.end() && it->second != i &&
                    std::find(orbit.begin(), orbit.end(), it->second) == orbit.end())
                    orbit.push_back(it->second);
            }
        int most = 0;
        for (size_t j : orbit) {
            done.insert(j);
            most = std::max(most, std::abs(changes[j].to - changes[j].from));
        }
        for (int k = 0; k < most; ++k)
            if (attempt(orbit, k))
                break;
    }
}

} // namespace bw
