#include "blockwright/brushes/gradient.h"

#include "blockwright/brushes/guard.h"
#include "blockwright/palette.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace bw {

namespace {

// Whether a rung can be written with iCE off: a solid colour always can, a
// shade only when one way round keeps the background dark (rungCell).
bool writableWithoutIce(const Ramp& ramp, int rung) {
    return rungCell(ramp, rung).bg.paletteIndex() < 8;
}

// Black is ink: a gradient's run of levels ends there (lint.cpp).
bool blackRung(const Ramp& ramp, int rung) {
    return rung % kRungsPerStep == 0 && ramp.colours[static_cast<size_t>(rung / kRungsPerStep)] == Black;
}

constexpr int kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

thread_local long long visitCount = 0;

// A gradient's ladder and where along it each rung ends: rung i covers
// [ends[i - 1], ends[i]), the first rung everything before and the last
// everything after.
struct Bands {
    std::vector<int> ladder;
    std::vector<double> ends;

    int rungAt(double t) const {
        const auto it = std::upper_bound(ends.begin(), ends.end(), t);
        return ladder[std::min(size_t(it - ends.begin()), ladder.size() - 1)];
    }
    // The last rung keeps one band and `more` go on past it, a band each,
    // the last of them on past the ladder.
    void runOn(const std::vector<int>& more) {
        double at = (ladder.size() > 1 ? ends[ends.size() - 2] : ends[0] - kGradientBand) + kGradientBand;
        ends.back() = at;
        for (const int r : more) {
            ladder.push_back(r);
            ends.push_back(at += kGradientBand);
        }
    }
    // The first rung keeps one band and `more` go back before it, a band
    // each, the last of them on before the ladder.
    void runBack(const std::vector<int>& more) {
        double at = ends[0];
        for (const int r : more) {
            ladder.insert(ladder.begin(), r);
            ends.insert(ends.begin(), at -= kGradientBand);
        }
    }
};

// The gradient over a width × height canvas. `paints(x, y)` is whether the
// fill writes a cell and `outside(x, y)` the rung of one it doesn't (-1 off
// the ladder); off the canvas is -1. Only the cells of `window` are laid out
// and settled, so it must hold every cell the fill paints: the rest are
// read through `outside`. The clamp's walks along a line still go on past
// the window through a field of one rung, to another rung or the edge.
class Fill {
public:
    Fill(const Ramp& ramp, const Gradient& g, int width, int height, bool ice, const CellRect& window)
        : ramp_(ramp), g_(g), w_(width), h_(height), ice_(ice),
          win_(window.intersected(CellRect{0, 0, std::max(0, width), std::max(0, height)})) {
        // Screen units: a column is one, a row two. In double before any
        // subtraction, so ends at the limits of int don't overflow.
        dx_ = double(g.x1) - double(g.x0);
        dy_ = 2.0 * (double(g.y1) - double(g.y0));
        span_ = std::hypot(dx_, dy_);
        if (ramp.colours.size() < 2 || width <= 0 || height <= 0 || span_ == 0)
            return;
        from_ = std::clamp(g.fromRung, 0, ramp.topRung());
        const int to = std::clamp(g.toRung, 0, ramp.topRung());
        dir_ = to >= from_ ? 1 : -1;
        for (int r = from_;; r += dir_) {
            if (!writable(r))
                break;
            b_.ladder.push_back(r);
            if (r == to)
                break;
        }
        if (b_.ladder.empty())
            return;
        layOut();
    }

    bool empty() const { return b_.ladder.empty() || win_.empty(); }
    // The cells rungs() holds, row by row.
    const CellRect& window() const { return win_; }

    template <class Paints, class Outside>
    std::vector<int> rungs(Paints&& paints, Outside&& outside) {
        if (extend(paints, outside))
            rungs_.clear();
        if (rungs_.empty())
            lay();
        clamp(paints, outside);
        despeckle(paints, outside);
        return rungs_;
    }

private:
    bool writable(int r) const {
        return r >= 0 && r <= ramp_.topRung() && (ice_ || writableWithoutIce(ramp_, r));
    }
    bool on(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
    size_t at(int x, int y) const { return static_cast<size_t>(y - win_.y) * win_.width + (x - win_.x); }
    long long cells() const { return static_cast<long long>(win_.width) * win_.height; }

    // Each rung's width along the gradient: the shades kGradientBand, the
    // solids that and an equal share of what the span has left. With no solid
    // rung the first and last rungs share it, as planes at the ends: a shade
    // between a lower and a higher rung may be no wider than the lint's three
    // cells. A radial gradient's plateaus take whole bands, so every ring ends
    // on an even radius (a whole row): near the centre a ring ending between
    // two (1.5, 3.5 or 5.5 columns out) is a rung four cells wide where it cuts
    // a row. What is left over goes to the last rung, which runs on past the
    // end anyway.
    void layOut() {
        const std::vector<int>& ladder = b_.ladder;
        const int solids = int(std::count_if(ladder.begin(), ladder.end(), [](int r) { return r % kRungsPerStep == 0; }));
        double rest = std::max(0.0, span_ - kGradientBand * double(ladder.size()));
        if (g_.shape == GradientShape::Radial) {
            const int shares = solids == 0 ? std::min<int>(2, int(ladder.size())) : solids;
            rest = std::floor(rest / (kGradientBand * shares)) * kGradientBand * shares;
        }
        double end = 0;
        for (size_t i = 0; i < ladder.size(); ++i) {
            double w = kGradientBand;
            if (solids != 0) {
                if (ladder[i] % kRungsPerStep == 0)
                    w += rest / solids;
            } else if (i == 0 || i + 1 == ladder.size()) {
                w += rest / std::min<double>(2, double(ladder.size()));
            }
            b_.ends.push_back(end += w);
        }
        // Each shade is written the way round that keeps its background dark
        // where only one way does, so steel's 3 and 5 are both ▓ and its 7 and
        // 9 both ░: three rungs across one of those solids would show one
        // glyph (the lint's one-glyph finding). One more rung past the end, or
        // else before the start, shows another.
        const auto glyphs = [&](const std::vector<int>& rungs) {
            std::vector<char32_t> out;
            for (const int r : rungs)
                if (r % kRungsPerStep != 0 && std::find(out.begin(), out.end(), rungCell(ramp_, r).ch) == out.end())
                    out.push_back(rungCell(ramp_, r).ch);
            return out;
        };
        const std::vector<char32_t> shown = glyphs(ladder);
        const auto shades = std::count_if(ladder.begin(), ladder.end(), [](int r) { return r % kRungsPerStep != 0; });
        if (shades < 2 || shown.size() != 1)
            return;
        const auto another = [&](int r) { return writable(r) && glyphs({r}) != shown && r % kRungsPerStep != 0; };
        if (another(ladder.back() + dir_))
            b_.runOn({ladder.back() + dir_});
        else if (another(ladder.front() - dir_))
            b_.runBack({ladder.front() - dir_});
    }

    void lay() {
        rungs_.assign(static_cast<size_t>(cells()), 0);
        visitCount += cells();
        const double ux = dx_ / span_, uy = dy_ / span_;
        for (int y = win_.y; y < win_.y + win_.height; ++y)
            for (int x = win_.x; x < win_.x + win_.width; ++x) {
                const double px = double(x) - double(g_.x0), py = 2.0 * (double(y) - double(g_.y0));
                const double t = g_.shape == GradientShape::Linear ? px * ux + py * uy : std::hypot(px, py);
                rungs_[at(x, y)] = b_.rungAt(t);
            }
    }

    // A field it doesn't paint (outside the selection, or locked) of a
    // rung on past the ladder's end, beside the plane of its last rung: a
    // shade plane between the rung before it and the field is a wide rung,
    // and where it is pulled toward the field (clamp) it is one in front of
    // the climb. So the ladder goes on to the next solid rung or the
    // field's, whichever comes first, and that is the plane; likewise
    // before the first rung. Whether it went on.
    template <class Paints, class Outside>
    bool extend(Paints&& paints, Outside&& outside) {
        lay();
        const int first = b_.ladder.front(), last = b_.ladder.back();
        int onward = -1, back = -1;  // the nearest field past each end
        const auto nearer = [](int& best, int e, int end) {
            if (best < 0 || std::abs(e - end) < std::abs(best - end))
                best = e;
        };
        visitCount += cells();
        for (int y = win_.y; y < win_.y + win_.height; ++y)
            for (int x = win_.x; x < win_.x + win_.width; ++x) {
                const int r = rungs_[at(x, y)];
                if ((r != first && r != last) || !paints(x, y))
                    continue;
                for (const auto& s : kSteps) {
                    const int nx = x + s[0], ny = y + s[1];
                    if (!on(nx, ny) || paints(nx, ny))
                        continue;
                    const int e = outside(nx, ny);
                    if (e < 0 || blackRung(ramp_, e))
                        continue;
                    if (r == last && (e - last) * dir_ > 0)
                        nearer(onward, e, last);
                    if (r == first && (first - e) * dir_ > 0)
                        nearer(back, e, first);
                }
            }
        // The rungs from past `end` toward `e`, a step `d` at a time, to the
        // next solid or e.
        const auto toward = [&](int end, int e, int d) {
            std::vector<int> more;
            if (e < 0 || end % kRungsPerStep == 0)
                return more;
            for (int r = end + d; writable(r); r += d) {
                more.push_back(r);
                if (r == e || r % kRungsPerStep == 0)
                    break;
            }
            return more;
        };
        const std::vector<int> on = toward(last, onward, dir_), before = toward(first, back, -dir_);
        if (!on.empty())
            b_.runOn(on);
        if (!before.empty())
            b_.runBack(before);
        return !on.empty() || !before.empty();
    }

    // Where the fill meets a field it doesn't paint across the selection's
    // edge (or beside a locked cell), a jump of two rungs or more that goes
    // on the same way as the level before or after it is a skip (lint.cpp).
    // There the fill steps toward the field's rung, a rung a cell: the cell
    // beside the field (a source) comes within a rung of it, every cell d
    // cells from a source within d + 1. A cell between two fields too far
    // apart for that takes the darker pull. A fill that turns at the edge
    // (it goes away from the field there) is left as it is, so a gradient
    // along the edge keeps its shape. Sources are found again on the result
    // until no more are needed: a pull reaching along an edge makes the next
    // cell a source, a cell a round, so there are as many rounds as the
    // canvas is wide and high. A cell a pull has moved part of the way to a
    // field beside it (a corner, pulled from the other edge) is a source too,
    // or it is a dip two rungs short of the field that leaves the edge's run
    // a rung wide. With iCE off a rung that can't be written goes back toward
    // the fill's own rung until one can.
    template <class Paints, class Outside>
    void clamp(Paints&& paints, Outside&& outside) {
        const int top = ramp_.topRung();
        const size_t n = rungs_.size();
        const std::vector<int> laid = rungs_;
        std::vector<int> lo0(n, -1), hi0(n, top + 1);
        // The rung of the level next along a line, from (x, y) a step (sx,
        // sy) at a time past the run of `r`; -1 where the gradient ends (the
        // canvas's edge, black, off the ladder).
        const auto next = [&](int x, int y, int sx, int sy, int r) {
            for (;; x += sx, y += sy) {
                ++visitCount;
                if (!on(x, y))
                    return -1;
                const int o = paints(x, y) ? rungs_[at(x, y)] : outside(x, y);
                if (o < 0 || blackRung(ramp_, o))
                    return -1;
                if (o != r)
                    return o;
            }
        };
        // Makes (x, y) a source pulled to within a rung of `e`; whether it
        // wasn't one already.
        const auto pull = [&](int x, int y, int e) {
            const size_t i = at(x, y);
            if (lo0[i] >= e - 1 && hi0[i] <= e + 1)
                return false;
            lo0[i] = std::max(lo0[i], e - 1);
            hi0[i] = std::min(hi0[i], e + 1);
            return true;
        };
        // A climb from a field goes on along the edge, both ways, as far as
        // the fill there is two rungs or more from the same field: one that
        // stopped short would end its run of a rung along the edge on a
        // lower one, a step as wide as the run.
        const auto along = [&](int x, int y, const int* s, int e) {
            for (const int d : {-1, 1})
                for (int mx = x + s[1] * d, my = y + s[0] * d; on(mx, my) && paints(mx, my);
                     mx += s[1] * d, my += s[0] * d) {
                    ++visitCount;
                    const int qx = mx - s[0], qy = my - s[1];
                    if (!on(qx, qy) || paints(qx, qy) || outside(qx, qy) != e ||
                        std::abs(e - rungs_[at(mx, my)]) < 2)
                        break;
                    pull(mx, my, e);
                }
        };
        for (int round = 0; round <= w_ + h_; ++round) {
            bool more = false;
            visitCount += cells();
            for (int y = win_.y; y < win_.y + win_.height; ++y)
                for (int x = win_.x; x < win_.x + win_.width; ++x) {
                    if (!paints(x, y))
                        continue;
                    const int b = rungs_[at(x, y)];
                    for (const auto& s : kSteps) {
                        const int qx = x - s[0], qy = y - s[1];  // the field, a step back
                        if (!on(qx, qy) || paints(qx, qy))
                            continue;
                        const int e = outside(qx, qy);
                        if (e < 0 || blackRung(ramp_, e) || std::abs(e - b) < 2)
                            continue;
                        const int after = next(x, y, s[0], s[1], b), before = next(qx, qy, -s[0], -s[1], e);
                        const bool skip = (after >= 0 && (after - b) * (b - e) > 0) ||
                                          (before >= 0 && (e - before) * (b - e) > 0);
                        const bool partway = (b - laid[at(x, y)]) * (e - b) > 0;
                        if ((skip || partway) && pull(x, y, e)) {
                            along(x, y, s, e);
                            more = true;
                        }
                    }
                }
            if (!more)
                return;
            // The pulls spread a rung a cell through the painted cells.
            const auto spread = [&](std::vector<int> v, int sign) {
                for (int& p : v)
                    p = sign > 0 ? p : top - p;  // spread down from the highest
                std::vector<std::vector<size_t>> level(static_cast<size_t>(top) + 2);
                visitCount += static_cast<long long>(n);
                for (size_t i = 0; i < n; ++i)
                    if (v[i] > 0)
                        level[static_cast<size_t>(v[i])].push_back(i);
                for (int l = top + 1; l > 0; --l)
                    for (size_t k = 0; k < level[static_cast<size_t>(l)].size(); ++k) {
                        const size_t i = level[static_cast<size_t>(l)][k];
                        ++visitCount;
                        if (v[i] != l)
                            continue;
                        const int x = win_.x + int(i % size_t(win_.width)), y = win_.y + int(i / size_t(win_.width));
                        for (const auto& s : kSteps) {
                            const int nx = x + s[0], ny = y + s[1];
                            if (on(nx, ny) && paints(nx, ny) && v[at(nx, ny)] < l - 1) {
                                v[at(nx, ny)] = l - 1;
                                level[static_cast<size_t>(l - 1)].push_back(at(nx, ny));
                            }
                        }
                    }
                for (int& p : v)
                    p = sign > 0 ? p : top - p;
                return v;
            };
            const std::vector<int> lo = spread(lo0, 1), hi = spread(hi0, -1);
            visitCount += cells();
            for (int y = win_.y; y < win_.y + win_.height; ++y)
                for (int x = win_.x; x < win_.x + win_.width; ++x) {
                    if (!paints(x, y))
                        continue;
                    const size_t i = at(x, y);
                    int r = std::min(std::max(laid[i], lo[i]), hi[i]);
                    while (!writable(r))
                        r += laid[i] > r ? 1 : -1;
                    rungs_[i] = r;
                }
        }
    }

    // A cell whose neighbours are all on one side of it is a speck to the
    // lint (lint.cpp, speck): the first ring of a radial gradient whose
    // centre is a column off the canvas clips to the one cell beside it, its
    // three neighbours all a ring further out. Such a cell takes its nearest
    // neighbour's rung, until none is left.
    template <class Paints, class Outside>
    void despeckle(Paints&& paints, Outside&& outside) {
        for (int pass = 0; pass < 8; ++pass) {
            const std::vector<int> was = rungs_;
            bool changed = false;
            visitCount += cells();
            for (int y = win_.y; y < win_.y + win_.height; ++y)
                for (int x = win_.x; x < win_.x + win_.width; ++x) {
                    const int g = was[at(x, y)];
                    if (!paints(x, y) || blackRung(ramp_, g) || g == ramp_.topRung())
                        continue;
                    int seen = 0, above = 0, below = 0, up = INT_MAX, down = -1;
                    for (const auto& s : kSteps) {
                        const int nx = x + s[0], ny = y + s[1];
                        const int o = !on(nx, ny) ? -1 : paints(nx, ny) ? was[at(nx, ny)] : outside(nx, ny);
                        if (o < 0)
                            continue;
                        ++seen;
                        if (o > g)
                            ++above, up = std::min(up, o);
                        if (o < g)
                            ++below, down = std::max(down, o);
                    }
                    const int to = seen < 3 ? g : above == seen ? up : below == seen ? down : g;
                    if (to != g && writable(to)) {
                        rungs_[at(x, y)] = to;
                        changed = true;
                    }
                }
            if (!changed)
                return;
        }
    }

    const Ramp& ramp_;
    const Gradient& g_;
    int w_, h_;
    bool ice_;
    CellRect win_;
    double dx_ = 0, dy_ = 0, span_ = 0;
    int from_ = 0, dir_ = 1;
    Bands b_;
    std::vector<int> rungs_;
};

} // namespace

std::vector<int> gradientRungs(const Ramp& ramp, const Gradient& g, int width, int height, bool ice) {
    Fill fill(ramp, g, width, height, ice, CellRect{0, 0, width, height});
    if (fill.empty())
        return {};
    return fill.rungs([](int, int) { return true; }, [](int, int) { return -1; });
}

CellEdit rampGradient(const Canvas& canvas, const Ramp& ramp, const Gradient& g, const StrokeOptions& options,
                      std::vector<lint::LadderFinding>* added) {
    CellEdit edit;
    if (added)
        added->clear();
    // A selection's cells are all within its bounds.
    const CellRect window = options.selection ? options.selection->bounds()
                                              : CellRect{0, 0, canvas.width(), canvas.height()};
    Fill fill(ramp, g, canvas.width(), canvas.height(), options.ice, window);
    if (fill.empty())
        return edit;
    const CellRect& win = fill.window();
    const auto paints = [&](int x, int y) {
        return win.contains(x, y) && (!options.selection || options.selection->contains(x, y)) &&
               !(options.lockTransparent && canvas.at(x, y).transparent);
    };
    const auto outside = [&](int x, int y) {
        const auto r = rungOf(ramp, canvas.at(x, y));
        return r ? *r : -1;
    };
    const std::vector<int> rungs = fill.rungs(paints, outside);
    std::vector<std::pair<int, int>> changed;
    visitCount += static_cast<long long>(win.width) * win.height;
    for (int y = win.y; y < win.y + win.height; ++y) {
        for (int x = win.x; x < win.x + win.width; ++x) {
            if (!paints(x, y))
                continue;
            const Cell& before = canvas.at(x, y);
            const Cell after = rungCell(ramp, rungs[static_cast<size_t>(y - win.y) * win.width + (x - win.x)]);
            if (after != before) {
                edit.changes.push_back({x, y, before, after});
                changed.push_back({x, y});
            }
        }
    }
    if (added && !edit.empty()) {
        Canvas after = canvas;
        edit.apply(after);
        *added = StrokeLint(canvas, ramp).judge(after, changed);
    }
    return edit;
}

long long gradientCellsVisited() { return visitCount; }

} // namespace bw
