// Property tests for the outline brush (PLAN §10, "Detail"): a seeded
// generator of lint-clean shaded shapes, and what an outline must do to
// every one of them.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace bw;

namespace {

using Point = std::pair<int, int>;

struct Rng {
    std::mt19937 g;
    explicit Rng(unsigned seed) : g(seed) {}
    int uni(int a, int b) { return std::uniform_int_distribution<int>(a, b)(g); }
    bool chance(double p) { return std::uniform_real_distribution<double>(0, 1)(g) < p; }
};

enum class Form { Rect, Ellipse, LShape, Blob, Stair45, StairSteep, Count };
enum class Shading { Flat, Linear, Radial, Count };

struct Case {
    Canvas canvas;
    Ramp ramp;
    bool ice = true;
    Form form = Form::Rect;
    Shading shading = Shading::Flat;
    int dir = 0;          // linear: 0-7, the eight directions
    bool thin = false;    // has thin parts
    bool fringe = false;  // has half-block fringe cells
    bool shaded = false;  // more than one rung
    bool atEdge = false;  // touches the canvas's edge
    bool endsMidRamp = false;
};

const int kDirs[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

// One shape on black: its form, a few thin parts, shading along one of eight
// directions or round a centre, and half-block fringes along its top or
// bottom, on one of five ramps, read as it shows with iCE on or off. One
// shape in four may run to the canvas's edge.
Case generate(Rng& rng, int n) {
    static const char* const ramps[] = {"steel", "fire", "ice", "sunset", "toxic"};
    Case k;
    k.ramp = *findRamp(ramps[n % 5]);
    k.ice = (n / 5) % 2 == 0;
    const int W = rng.uni(14, 30), H = rng.uni(10, 18);
    k.canvas = Canvas(W, H);
    const int m = rng.chance(0.25) ? 0 : 1;
    std::vector<char> in(static_cast<size_t>(W) * H, 0);
    auto set = [&](int x, int y) {
        if (x >= m && y >= m && x < W - m && y < H - m && !in[static_cast<size_t>(y) * W + x]) {
            in[static_cast<size_t>(y) * W + x] = 1;
            return true;
        }
        return false;
    };
    auto at = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && in[static_cast<size_t>(y) * W + x]; };
    k.form = Form(rng.uni(0, int(Form::Count) - 1));
    const int x0 = rng.uni(m, W / 3), y0 = rng.uni(m, H / 3);
    const int x1 = rng.uni(x0 + 3, W - 1 - m), y1 = rng.uni(y0 + 2, H - 1 - m);
    switch (k.form) {
    case Form::Rect:
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                set(x, y);
        break;
    case Form::Ellipse:
    case Form::Blob: {
        // round on screen, a row two columns tall; a blob is lumpier
        const double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0;
        const double rx = (x1 - x0) / 2.0 + 0.5, ry = (y1 - y0) / 2.0 + 0.5;
        const double lump = k.form == Form::Blob ? 0.25 : 0.0;
        const double phase = rng.uni(0, 628) / 100.0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double dx = (x - cx) / rx, dy = (y - cy) / ry;
                const double a = std::atan2(dy, dx);
                if (dx * dx + dy * dy <= 1.0 + lump * std::sin(3 * a + phase))
                    set(x, y);
            }
        break;
    }
    case Form::LShape: {
        const int xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                if (x <= xm || y >= ym)
                    set(x, y);
        break;
    }
    case Form::Stair45:
    case Form::StairSteep: {
        // a side at 45° (one column a row) or steep (one column every two
        // rows), facing one of four ways
        const int way = rng.uni(0, 3);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const int dy = (way & 1) ? y1 - y : y - y0;
                const int dx = (way & 2) ? x1 - x : x - x0;
                const int reach = k.form == Form::Stair45 ? dy + 2 : dy / 2 + 2;
                if (dx <= reach)
                    set(x, y);
            }
        break;
    }
    default: break;
    }
    // thin parts 1-3 cells across, growing out of the body
    if (rng.chance(0.4)) {
        for (int parts = rng.uni(1, 2); parts > 0; --parts) {
            std::vector<Point> body;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    if (at(x, y))
                        body.push_back({x, y});
            if (body.empty())
                break;
            auto [bx, by] = body[static_cast<size_t>(rng.uni(0, int(body.size()) - 1))];
            const int d = rng.uni(0, 3);
            const int ddx = d == 0 ? 1 : d == 1 ? -1 : 0, ddy = d == 2 ? 1 : d == 3 ? -1 : 0;
            while (at(bx + ddx, by + ddy))
                bx += ddx, by += ddy;
            const int width = rng.uni(1, 3), len = rng.uni(2, 6);
            for (int i = 1; i <= len; ++i)
                for (int j = 0; j < width; ++j)
                    k.thin = set(bx + ddx * i + (ddx ? 0 : j), by + ddy * i + (ddy ? 0 : j)) || k.thin;
        }
    }
    // shading
    const int top = k.ramp.topRung();
    k.shading = Shading(rng.uni(0, int(Shading::Count) - 1));
    const int lo = rng.uni(1, std::min(9, top - 1));
    const int step = rng.uni(1, 3);
    k.dir = rng.uni(0, 7);
    const int start = rng.uni(std::max(lo + 2, top - 6), top);
    const double cx = rng.uni(x0, x1), cy = rng.uni(y0, y1);
    const int flat = rng.uni(1, top);
    auto rungAt = [&](int x, int y) {
        switch (k.shading) {
        case Shading::Flat: return flat;
        case Shading::Linear: {
            // falling from `start` along the direction, a rung every
            // `step` cells, clamped at `lo` (13-x and the like: ends mid-ramp)
            const int t = kDirs[k.dir][0] * (x - (kDirs[k.dir][0] > 0 ? 0 : W)) +
                          kDirs[k.dir][1] * (y - (kDirs[k.dir][1] > 0 ? 0 : H));
            return std::max(lo, start - std::abs(t) / step);
        }
        case Shading::Radial: {
            const double d = std::hypot(x - cx, 2 * (y - cy));
            return std::max(lo, start - int(d) / step);
        }
        default: return flat;
        }
    };
    std::set<int> levels;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (at(x, y)) {
                const int g = rungAt(x, y);
                k.endsMidRamp = k.endsMidRamp || (g > 4 && g % 4 != 0);
                k.atEdge = k.atEdge || x == 0 || y == 0 || x == W - 1 || y == H - 1;
                levels.insert(g);
                k.canvas.at(x, y) = rungCell(k.ramp, g);
            }
    k.shaded = levels.size() > 1;
    // half-block fringes along the top or bottom: the shape's colour in one
    // half, black in the other
    if (rng.chance(0.3)) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!at(x, y) || !rng.chance(0.5))
                    continue;
                bool upOut = !at(x, y - 1), downOut = !at(x, y + 1);
                if (upOut == downOut) {
                    // now and then one inside the shape, its black half a
                    // notch in it
                    if (upOut || !rng.chance(0.1))
                        continue;
                    upOut = rng.chance(0.5);
                }
                const int g = rungAt(x, y);
                const auto colour = k.ramp.colours[static_cast<size_t>(g / kRungsPerStep)];
                if (colour == Black)
                    continue;
                k.canvas.at(x, y) = Cell{upOut ? kLowerHalf : kUpperHalf, Color::indexed(colour), Color::indexed(Black)};
                k.fringe = true;
            }
    }
    if (!k.ice)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                k.canvas.at(x, y) = iceCell(k.canvas.at(x, y), false);
    return k;
}

Canvas applied(Canvas c, const CellEdit& e) {
    e.apply(c);
    return c;
}

CellEdit outlineAll(const Canvas& c, const Ramp& r, const StrokeOptions& o) {
    BrushSettings s;
    s.size = kMaxBrushSize;
    return paintStroke(c, OutlineBrush(r), s, {{c.width() / 2, c.height() / 2}}, o);
}

bool halfBlockCell(const Cell& c) { return c.ch == kUpperHalf || c.ch == kLowerHalf || c.ch == U'▌' || c.ch == U'▐'; }

// A cell the outline may write: a shape cell with a cell outside the shape
// beside it along a row or a column, a staircase filler (outside only
// diagonally), or a half block (its black half is outside).
bool lineCell(const Canvas& c, const Ramp& r, int x, int y, bool ice) {
    const Cell shown = ice ? c.at(x, y) : iceCell(c.at(x, y), false);
    if (!inShape(r, c.at(x, y), ice))
        return false;
    if (halfBlockCell(shown))
        return true;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if ((dx || dy) && c.contains(x + dx, y + dy) && !inShape(r, c.at(x + dx, y + dy), ice))
                return true;
    return false;
}

// The line cells a full outline leaves on their own: runs of fewer than
// three rim cells it took to the line (or darker), all of them written,
// beside a rim cell left off the line. A cell's line is the outline colour
// of the darkest shape cell off the rim among its four neighbours, else of
// the darkest of the nearest ones by a walk through the shape (PLAN §10: a
// thin part takes the level of the body it grows from); a half block the
// line went into is a line cell, and other half blocks and cells of a part
// with no such cell aren't judged.
int fragments(const Canvas& before, const Canvas& after, const Ramp& r, bool ice) {
    const int W = before.width(), H = before.height();
    const auto shownRung = [&](const Canvas& c, int x, int y) -> std::optional<int> {
        const Cell s = ice ? c.at(x, y) : iceCell(c.at(x, y), false);
        if (halfBlockCell(s))
            return std::nullopt;
        return rungOf(r, s);
    };
    std::set<Point> edge;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (lineCell(before, r, x, y, ice))
                edge.insert({x, y});
    // the nearest interior cells' darkest rung, by a walk through the shape
    std::map<Point, int> dist, best;
    std::vector<Point> layer;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (inShape(r, before.at(x, y), ice) && !edge.count({x, y}))
                if (const auto g = shownRung(before, x, y)) {
                    dist[{x, y}] = 0;
                    best[{x, y}] = *g;
                    layer.push_back({x, y});
                }
    for (int d = 1; !layer.empty(); ++d) {
        std::vector<Point> next;
        for (const auto& [x, y] : layer)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const Point n{x + dx, y + dy};
                    if ((!dx && !dy) || !before.contains(n.first, n.second) ||
                        !inShape(r, before.at(n.first, n.second), ice))
                        continue;
                    if (!dist.count(n)) {
                        dist[n] = d;
                        best[n] = INT_MAX;
                        next.push_back(n);
                    }
                    if (dist[n] == d)
                        best[n] = std::min(best[n], best[{x, y}]);
                }
        layer = std::move(next);
    }
    std::set<Point> line, open;
    for (const auto& [x, y] : edge) {
        const auto own = shownRung(before, x, y);
        // a fringe the line went into is part of it
        if (!own) {
            if (!(after.at(x, y) == before.at(x, y)))
                line.insert({x, y});
            continue;
        }
        std::optional<int> l;
        int four = INT_MAX;
        for (const auto& n : {Point{x, y - 1}, {x, y + 1}, {x - 1, y}, {x + 1, y}})
            if (const auto it = dist.find(n); it != dist.end() && it->second == 0)
                four = std::min(four, best[n]);
        if (four == INT_MAX && best.count({x, y}))
            four = best[{x, y}];
        if (four != INT_MAX)
            l = lint::outlineRung(r, four).value_or(kRungsPerStep);
        if (!l)
            continue;
        const auto g = shownRung(after, x, y);
        (g && *g <= *l ? line : open).insert({x, y});
    }
    int out = 0;
    std::set<Point> seen;
    for (const Point& p : line) {
        if (seen.count(p))
            continue;
        std::vector<Point> work{p};
        seen.insert(p);
        size_t size = 0;
        bool written = true, broken = false;
        while (!work.empty()) {
            const auto [x, y] = work.back();
            work.pop_back();
            ++size;
            written = written && !(after.at(x, y) == before.at(x, y));
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const Point n{x + dx, y + dy};
                    broken = broken || open.count(n);
                    if (line.count(n) && !seen.count(n)) {
                        seen.insert(n);
                        work.push_back(n);
                    }
                }
        }
        out += size < 3 && written && broken;
    }
    return out;
}

struct Failures {
    int lint = 0, second = 0, notEdge = 0, fragment = 0;
    int partialOutside = 0, partialLock = 0, partialRepair = 0, interiorDab = 0;
    int partialAdded = 0, partialRepeat = 0, partialAgain = 0, partialReplay = 0;
    int shapes = 0;
};

// A random partial stroke over a shape: 1-8 dabs, joined or not, of one
// size from 1 to 7 and either tip, through a random selection most of the
// time, with a few cells locked; or, one time in three, the whole canvas
// through a selection.
struct Partial {
    Canvas canvas;  // the shape with its locked cells
    BrushSettings settings;
    StrokeOptions options;
    std::vector<std::pair<Dab, bool>> dabs;  // and whether each is joined
    std::set<Point> foot;                    // the cells the stroke may paint
};

Partial partial(Rng& rng, const Case& k) {
    Partial p;
    const int W = k.canvas.width(), H = k.canvas.height();
    p.canvas = k.canvas;
    for (int i = rng.uni(0, 6); i > 0; --i)
        p.canvas.at(rng.uni(0, W - 1), rng.uni(0, H - 1)).transparent = true;
    p.options.ice = k.ice;
    p.options.lockTransparent = true;
    std::shared_ptr<Mask> sel;
    const bool whole = rng.chance(1.0 / 3);
    if (whole || rng.chance(0.7)) {
        sel = std::make_shared<Mask>(
            Mask::rect(W, H, rng.uni(0, W - 1), rng.uni(0, H - 1), rng.uni(0, W - 1), rng.uni(0, H - 1)));
        for (int i = rng.uni(0, 10); i > 0; --i)
            sel->set(rng.uni(0, W - 1), rng.uni(0, H - 1), false);
        p.options.selection = sel;
    }
    p.settings.tip = rng.chance(0.5) ? Tip::Round : Tip::Square;
    std::vector<Point> centres;
    if (whole) {
        p.settings.size = kMaxBrushSize;
        p.dabs.push_back({Dab{W / 2, H / 2}, false});
        centres.push_back({W / 2, H / 2});
    } else {
        p.settings.size = rng.uni(1, 7);
        std::optional<Point> last;
        for (int i = rng.uni(1, 8); i > 0; --i) {
            const Dab d{rng.uni(0, W - 1), rng.uni(0, H - 1)};
            const bool join = last && rng.chance(0.5);
            p.dabs.push_back({d, join});
            // the stroke walks a joining line from its lesser end (by row,
            // then column)
            if (join)
                for (const auto& c : std::pair{last->second, last->first} < std::pair{d.y, d.x}
                                         ? line4(last->first, last->second, d.x, d.y)
                                         : line4(d.x, d.y, last->first, last->second))
                    centres.push_back(c);
            else
                centres.push_back({d.x, d.y});
            last = Point{d.x, d.y};
        }
    }
    for (const auto& [cx, cy] : centres)
        for (const auto& q : footprint(cx, cy, p.settings.size, p.settings.tip, W, H))
            if ((!sel || sel->contains(q.first, q.second)) && !p.canvas.at(q.first, q.second).transparent)
                p.foot.insert(q);
    return p;
}

CellEdit run(const Canvas& c, const Ramp& r, const Partial& p) {
    const OutlineBrush brush(r);
    Stroke stroke(c, brush, p.settings, p.options);
    for (const auto& [d, join] : p.dabs)
        stroke.addDab(d, join);
    return stroke.edit();
}

// The stroke's dabs, then the same dabs again in the same stroke: the edit
// after each.
std::pair<CellEdit, CellEdit> runTwice(const Canvas& c, const Ramp& r, const Partial& p) {
    const OutlineBrush brush(r);
    Stroke stroke(c, brush, p.settings, p.options);
    for (const auto& [d, join] : p.dabs)
        stroke.addDab(d, join);
    CellEdit once = stroke.edit();
    for (const auto& [d, join] : p.dabs)
        stroke.addDab(d, join);
    return {std::move(once), stroke.edit()};
}

// The lint's findings as (kind, x, y).
std::set<std::tuple<int, int, int>> findings(const Canvas& c, const Ramp& r) {
    std::set<std::tuple<int, int, int>> out;
    for (const auto& f : lint::checkLadder(c, r).findings)
        out.insert({int(f.kind), f.x, f.y});
    return out;
}

bool adds(const Canvas& before, const Canvas& after, const Ramp& r) {
    const auto was = findings(before, r);
    for (const auto& f : findings(after, r))
        if (!was.count(f))
            return true;
    return false;
}

} // namespace

TEST_CASE("the outline holds its properties on generated shaded shapes", "[detail][lint][property]") {
    // BW_PROPERTY_SEED runs another set of shapes
    const char* seed = std::getenv("BW_PROPERTY_SEED");
    Rng rng(seed ? unsigned(std::strtoul(seed, nullptr, 10)) : 20261022u);
    Failures f;
    std::map<std::string, int> cover;
    const int want = 1200, partials = 2;
    int kept = 0, strokes = 0;
    for (int n = 0; kept < want && n < 40 * want; ++n) {
        Case k = generate(rng, n);
        if (!lint::checkLadder(k.canvas, k.ramp).clean())
            continue;
        ++kept;
        cover[k.ramp.name]++;
        cover[k.ice ? "ice on" : "ice off"]++;
        cover["form " + std::to_string(int(k.form))]++;
        cover["shading " + std::to_string(int(k.shading))]++;
        if (k.shading == Shading::Linear && k.shaded)
            cover["dir " + std::to_string(k.dir)]++;
        cover["thin"] += k.thin;
        cover["fringe"] += k.fringe;
        cover["mid-ramp"] += k.endsMidRamp;
        cover["edge"] += k.atEdge;

        StrokeOptions o;
        o.ice = k.ice;
        const Canvas& c = k.canvas;
        const CellEdit e = outlineAll(c, k.ramp, o);
        const Canvas out = applied(c, e);
        bool bad = false;
        // lint clean
        if (!lint::checkLadder(out, k.ramp).clean()) {
            ++f.lint;
            bad = true;
        }
        // a second pass changes nothing
        if (!outlineAll(out, k.ramp, o).empty()) {
            ++f.second;
            bad = true;
        }
        // no lone line cells
        if (fragments(c, out, k.ramp, k.ice)) {
            ++f.fragment;
            bad = true;
        }
        // every change is a line cell, or a repair beside a changed cell
        std::set<Point> changed;
        for (const CellChange& ch : e.changes)
            changed.insert({ch.x, ch.y});
        for (const auto& [x, y] : changed) {
            if (lineCell(c, k.ramp, x, y, k.ice))
                continue;
            const bool beside = changed.count({x - 1, y}) || changed.count({x + 1, y}) || changed.count({x, y - 1}) ||
                                changed.count({x, y + 1});
            if (!beside) {
                ++f.notEdge;
                bad = true;
                break;
            }
        }
        // partial strokes: they paint only footprint, selection and unlocked
        // cells, add no finding, and the same stroke again changes nothing
        for (int i = 0; i < partials; ++i) {
            const Partial p = partial(rng, k);
            ++strokes;
            const CellEdit pe = run(p.canvas, k.ramp, p);
            const Canvas after = applied(p.canvas, pe);
            if (adds(p.canvas, after, k.ramp)) {
                ++f.partialAdded;
                bad = true;
            }
            // Its dabs again in the same stroke change nothing (exact). A
            // second stroke of the same dabs over the result adds no finding
            // but may change cells: a cell the first wrote a step lighter,
            // before the line beside it was drawn, stays for the rest of that
            // stroke (a cell changes once a stroke), and the second can take
            // it darker against the whole line. How often it does depends on
            // the strokes, so it is reported, not bounded (PLAN §10, limits).
            if (const auto [once, twice] = runTwice(p.canvas, k.ramp, p); once.changes != twice.changes) {
                ++f.partialReplay;
                bad = true;
            }
            const CellEdit again = run(after, k.ramp, p);
            if (adds(after, applied(after, again), k.ramp)) {
                ++f.partialRepeat;
                bad = true;
            }
            f.partialAgain += !again.empty();
            std::set<Point> pchanged;
            for (const CellChange& ch : pe.changes)
                pchanged.insert({ch.x, ch.y});
            bool anyLine = false;
            for (const auto& [x, y] : p.foot)
                anyLine = anyLine || lineCell(c, k.ramp, x, y, k.ice);
            if (!anyLine && !pe.empty()) {
                ++f.interiorDab;
                bad = true;
            }
            for (const auto& [x, y] : pchanged) {
                if (!p.foot.count({x, y})) {
                    ++f.partialOutside;
                    bad = true;
                    break;
                }
                if (p.canvas.at(x, y).transparent) {
                    ++f.partialLock;
                    bad = true;
                    break;
                }
                if (!lineCell(c, k.ramp, x, y, k.ice) &&
                    !(pchanged.count({x - 1, y}) || pchanged.count({x + 1, y}) || pchanged.count({x, y - 1}) ||
                      pchanged.count({x, y + 1}))) {
                    ++f.partialRepair;
                    bad = true;
                    break;
                }
            }
        }
        // a dab of size 1 on a cell the outline may not write changes
        // nothing: no repair without a line written beside it
        {
            BrushSettings s;
            const OutlineBrush brush(k.ramp);
            for (int i = 0; i < 24; ++i) {
                const int x = rng.uni(0, c.width() - 1), y = rng.uni(0, c.height() - 1);
                if (lineCell(c, k.ramp, x, y, k.ice))
                    continue;
                if (!paintStroke(c, brush, s, {{x, y}}, o).empty()) {
                    ++f.interiorDab;
                    bad = true;
                    break;
                }
            }
        }
        f.shapes += bad;
    }
    if (std::getenv("BW_PROPERTY_REPORT")) {
        std::printf("outline property: %d shapes, %d failing (lint %d, second pass %d, fragments %d, off the edge %d, "
                    "interior dab %d); %d partial strokes (added %d, repeat %d, replay %d, changed by a second stroke %d, "
                    "outside %d, lock %d, repair %d)\n",
                    kept, f.shapes, f.lint, f.second, f.fragment, f.notEdge, f.interiorDab, strokes, f.partialAdded,
                    f.partialRepeat, f.partialReplay, f.partialAgain, f.partialOutside, f.partialLock, f.partialRepair);
        for (const auto& [name, count] : cover)
            std::printf("  %s: %d\n", name.c_str(), count);
    }
    REQUIRE(kept == want);
    CHECK(strokes >= 2000);
    // every kind of input is there
    for (const char* name :
         {"steel", "fire", "ice", "sunset", "toxic", "ice on", "ice off", "thin", "fringe", "mid-ramp", "edge"})
        CHECK(cover[name] > 20);
    for (int i = 0; i < int(Form::Count); ++i)
        CHECK(cover["form " + std::to_string(i)] > 20);
    for (int i = 0; i < int(Shading::Count); ++i)
        CHECK(cover["shading " + std::to_string(i)] > 20);
    for (int i = 0; i < 8; ++i)
        CHECK(cover["dir " + std::to_string(i)] > 5);
    CHECK(f.lint == 0);
    CHECK(f.second == 0);
    CHECK(f.fragment == 0);
    CHECK(f.notEdge == 0);
    CHECK(f.partialAdded == 0);
    CHECK(f.partialRepeat == 0);
    CHECK(f.partialReplay == 0);
    CHECK(f.partialOutside == 0);
    CHECK(f.partialLock == 0);
    CHECK(f.partialRepair == 0);
    CHECK(f.interiorDab == 0);
    CHECK(f.shapes == 0);
}

TEST_CASE("the outline is a fixed point on any generated input", "[detail][lint][property]") {
    // The same shapes with no lint filter, half of them roughed up with
    // stray rungs and RGB cells: one outline adds no finding and a second
    // changes nothing
    const char* seed = std::getenv("BW_PROPERTY_SEED");
    Rng rng(seed ? unsigned(std::strtoul(seed, nullptr, 10)) + 1 : 20261023u);
    const int want = 1000;
    int second = 0, addedFindings = 0, unclean = 0;
    for (int n = 0; n < want; ++n) {
        Case k = generate(rng, n);
        Canvas& c = k.canvas;
        if (rng.chance(0.5))
            for (int i = rng.uni(1, 8); i > 0; --i) {
                Cell& cell = c.at(rng.uni(0, c.width() - 1), rng.uni(0, c.height() - 1));
                if (rng.chance(0.2)) {
                    cell = Cell{U'█', Color::fromRgb(Rgb{200, 30, 40}), Color::indexed(Black)};
                } else {
                    cell = rungCell(k.ramp, rng.uni(1, k.ramp.topRung()));
                    if (!k.ice)
                        cell = iceCell(cell, false);
                }
            }
        unclean += !lint::checkLadder(c, k.ramp).clean();
        StrokeOptions o;
        o.ice = k.ice;
        const Canvas out = applied(c, outlineAll(c, k.ramp, o));
        addedFindings += adds(c, out, k.ramp);
        second += !outlineAll(out, k.ramp, o).empty();
    }
    if (std::getenv("BW_PROPERTY_REPORT"))
        std::printf("outline fixed point: %d shapes, %d not lint clean; added findings %d, second pass %d\n", want,
                    unclean, addedFindings, second);
    CHECK(unclean > want / 2);
    CHECK(addedFindings == 0);
    CHECK(second == 0);
}
