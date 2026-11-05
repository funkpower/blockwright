#include "blockwright/brushes/form.h"

#include "blockwright/brushes/guard.h"

#include "blockwright/ansi.h"
#include "blockwright/brushes/distance.h"
#include "blockwright/canvas.h"
#include "blockwright/lint.h"
#include "blockwright/palette.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace bw {

namespace {

// A cell is twice as tall as it is wide: a row is two columns on screen.
constexpr double kAspect = 2.0;
// The light: raised a little off the canvas.
constexpr double kLightZ = 0.6;
// The glint stays this far (in columns) inside the edge.
constexpr double kGlintInset = 2.0;

struct Grid {
    int w = 0, h = 0;
    bool in(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    size_t at(int x, int y) const { return static_cast<size_t>(y) * w + x; }
};

constexpr int kNeighbours[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};

// Each shape cell's surface normal, (x, y, z) with z out of the canvas, in
// screen units. A dome is a ball about the shape's centre: a cell's
// distance from the centre over that plus its distance to the shape's edge
// is how far round the ball it is. A cylinder is the same across each run
// of its row (a pillar) or its column (a log) about the run's middle.
struct Normal {
    double x = 0, y = 0, z = 1;
};

std::vector<Normal> normals(const Mask& shape, const Grid& g, const std::vector<double>& dist, int across, int ox,
                            int oy) {
    std::vector<Normal> n(static_cast<size_t>(g.w) * g.h);
    // r over the edge's distance, out along unit vector ux, uy
    auto round = [](double rho, double ux, double uy) {
        rho = std::min(rho, 0.999);
        return Normal{rho * ux, rho * uy, std::sqrt(1.0 - rho * rho)};
    };
    if (across == 0) {
        double cx = 0, cy = 0;
        int cells = 0;
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x)
                if (shape.contains(x, y)) {
                    cx += x + ox;
                    cy += y + oy;
                    ++cells;
                }
        cx /= cells;
        cy /= cells;
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                if (!shape.contains(x, y))
                    continue;
                const double vx = x + ox - cx, vy = (y + oy - cy) * kAspect, r = std::sqrt(vx * vx + vy * vy);
                if (r < 1e-9)
                    continue;
                // the edge is half a cell short of the nearest cell off the
                // shape
                n[g.at(x, y)] = round(r / (r + std::max(0.5, dist[g.at(x, y)] - 0.5)), vx / r, vy / r);
            }
        return n;
    }
    const int lines = across == 1 ? g.h : g.w, len = across == 1 ? g.w : g.h;
    const double unit = across == 1 ? 1.0 : kAspect;
    for (int l = 0; l < lines; ++l)
        for (int i = 0; i < len;) {
            auto cell = [&](int k) { return across == 1 ? std::pair{k, l} : std::pair{l, k}; };
            if (!shape.contains(cell(i).first, cell(i).second)) {
                ++i;
                continue;
            }
            int end = i;
            while (end < len && shape.contains(cell(end).first, cell(end).second))
                ++end;
            // the run's middle and half its width, to its cells' outer edges
            const double mid = (i + end - 1) / 2.0, half = (end - i) * unit / 2.0;
            for (int k = i; k < end; ++k) {
                const double v = (k - mid) * unit;
                const double s = v < 0 ? -1.0 : 1.0;
                const auto [x, y] = cell(k);
                n[g.at(x, y)] = across == 1 ? round(std::abs(v) / half, s, 0) : round(std::abs(v) / half, 0, s);
            }
            i = end;
        }
    return n;
}

bool blackRung(const Ramp& ramp, int rung) {
    return rung % kRungsPerStep == 0 && ramp.colours[static_cast<size_t>(rung / kRungsPerStep)] == Black;
}

// Neighbours at most a rung apart: the dark side comes up to no less than a
// rung below any neighbour. Each cell ends at the most any cell gives it, a
// rung less for every step between them, worked from the lightest rung down
// so each cell is raised once.
void ladder(std::vector<int>& v, const Mask& shape, const Grid& g, int top) {
    std::vector<std::vector<std::pair<int, int>>> at(static_cast<size_t>(top) + 1);
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            if (shape.contains(x, y))
                at[static_cast<size_t>(std::clamp(v[g.at(x, y)], 0, top))].emplace_back(x, y);
    for (int r = top; r > 0; --r)
        for (size_t i = 0; i < at[r].size(); ++i) {
            const auto [x, y] = at[r][i];
            if (v[g.at(x, y)] != r)
                continue;
            for (const auto& d : kNeighbours) {
                const int nx = x + d[0], ny = y + d[1];
                if (shape.contains(nx, ny) && v[g.at(nx, ny)] < r - 1) {
                    v[g.at(nx, ny)] = r - 1;
                    at[r - 1].emplace_back(nx, ny);
                }
            }
        }
}

// The lint's speckle rule over the tones (lint::checkLadder): a cell with
// three or more neighbours lighter than all of them or darker than all,
// other than black and the top rung. Cells off the shape on the canvas are
// counted as darker (the ground, or ink). A speck takes the rung most of its
// neighbours on the shape share, the nearest one on a tie, the darker of
// two as near. Repeats until none is left.
void despeckle(std::vector<int>& v, const Ramp& ramp, const Mask& shape, const Grid& g, int top) {
    for (int pass = 0; pass < 64; ++pass) {
        bool changed = false;
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                if (!shape.contains(x, y))
                    continue;
                int& c = v[g.at(x, y)];
                if (blackRung(ramp, c) || c == top)
                    continue;
                int seen = 0, above = 0, below = 0, on = 0;
                int near[4];
                for (const auto& d : kNeighbours) {
                    const int nx = x + d[0], ny = y + d[1];
                    if (!g.in(nx, ny))
                        continue;
                    ++seen;
                    if (!shape.contains(nx, ny)) {
                        ++below;
                        continue;
                    }
                    const int n = v[g.at(nx, ny)];
                    above += n > c;
                    below += n < c;
                    // kept in order as they come (four at most)
                    int k = on++;
                    for (; k > 0 && near[k - 1] > n; --k)
                        near[k] = near[k - 1];
                    near[k] = n;
                }
                if (seen < 3 || (above != seen && below != seen) || on == 0)
                    continue;
                int best = near[0], bestCount = 0;
                for (int i = 0; i < on;) {
                    int j = i;
                    while (j < on && near[j] == near[i])
                        ++j;
                    if (j - i > bestCount || (j - i == bestCount && std::abs(near[i] - c) < std::abs(best - c)))
                        best = near[i], bestCount = j - i;
                    i = j;
                }
                c = best;
                changed = true;
            }
        if (!changed)
            return;
    }
}

// The widest a rung of the turn runs along the gradient, as the lint
// measures a step (the shorter of its run and its longest run the other
// way), over the rungs between black (or the ramp's darkest colour) and the
// lit plane, solid ones too: "no rung wider than about 4 cells
// on a form" judges every rung, where the lint judges only shades.
int widestTurn(const std::vector<int>& v, const Mask& shape, const Grid& g, int plane) {
    auto at = [&](int x, int y) { return shape.contains(x, y) ? v[g.at(x, y)] : -1; };
    auto runLen = [&](int x, int y, int dx, int dy) {
        const int t = at(x, y);
        int n = 1;
        for (int k = 1; at(x - k * dx, y - k * dy) == t; ++k)
            ++n;
        for (int k = 1; at(x + k * dx, y + k * dy) == t; ++k)
            ++n;
        return n;
    };
    int widest = 0;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{0, 1}}) {
                const int t = at(x, y);
                if (t <= 0 || t >= plane || at(x - dx, y - dy) == t)
                    continue;
                int len = 1;
                while (at(x + len * dx, y + len * dy) == t)
                    ++len;
                const int lo = at(x - dx, y - dy), hi = at(x + len * dx, y + len * dy);
                if (lo < 0 || hi < 0 || (lo < t) == (hi < t))
                    continue;
                int other = 0;
                for (int i = 0; i < len; ++i)
                    other = std::max(other, runLen(x + i * dx, y + i * dy, dy, dx));
                widest = std::max(widest, std::min(len, other));
            }
    return widest;
}

// How a try at a form reads: whether the ladder lint passes it, the widest
// rung of its turn, how many shade glyphs it has, its detail over
// its box (cells off the shape black, as `--box` reads a form on
// the ground) at the box's corner and averaged over the 8 alignments of its
// 4x2 windows, and its shares of black and of the lit plane.
struct Reading {
    bool clean = false;
    int widest = 0;
    int glyphs = 0;
    // whether the lit plane is there: a share of the form at its rung
    bool plane = false;
    lint::Detail corner;
    // whether any alignment has windows, and the mean over those that do
    bool windows = false;
    double mean = 0, flat = 0;
    double black = 0, lit = 0;
    // one-cell ridges three or more cells down a column, each cell lighter
    // than both its neighbours along the row: a light line drawn down the
    // form
    int seams = 0;

    // The bar: clean, no rung wider than about four cells on the
    // turn, more than one shade glyph, no seam, and where the form has
    // windows to measure, detail 3.6 or more and under 15% flat over the
    // alignments.
    bool passes() const {
        return clean && plane && widest <= 4 && glyphs >= 2 && seams == 0 &&
               (!windows || (mean >= 3.6 && flat < 15));
    }
    // How near a clean try that misses the bar comes to it: the narrow
    // rungs and the shade glyphs first, then no seam, then a lit plane,
    // then the glint the options ask for, then under 15% flat, then the
    // detail.
    double score(bool glint) const {
        return (widest <= 4 ? 1000.0 : 0.0) + (glyphs >= 2 ? 500.0 : 0.0) + (seams == 0 ? 400.0 : 0.0) +
               (plane ? 200.0 : 0.0) + (glint ? 150.0 : 0.0) + (flat < 15 ? 100.0 : 0.0) + mean - 0.02 * flat;
    }
};

// The form's geometry under a light: the shape's box, each cell's distance
// to the edge, how far it is from the brightest point (its arc, in columns
// over the surface) and how much of the light it takes (its dot), and
// where the form turns from the light.
struct Geometry {
    Grid g;
    int minX = 0, minY = 0, maxX = -1, maxY = -1, cells = 0;
    // 0 for a dome or a plane; a cylinder's 1 is a pillar (round across
    // its rows), 2 a log (down its columns)
    int across = 0;
    // the light's direction across the canvas, tilted off a cylinder's axis
    double lx = 0, ly = 0;
    std::vector<double> dist, arc, dot;
    double nearest = 0, farthest = 0, span = 0, terminator = 0;
};

// The shape is a crop of the canvas at (ox, oy): the dome's centre and the
// plane's distances are read in canvas cells, so a crop shades as the canvas
// does.
std::optional<Geometry> geometry(const Mask& shape, const FormOptions& options, Compass light, int ox = 0,
                                 int oy = 0) {
    Geometry geo;
    geo.g = Grid{shape.width(), shape.height()};
    const Grid& g = geo.g;
    int minX = g.w, minY = g.h, maxX = -1, maxY = -1;
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            if (shape.contains(x, y)) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    if (maxX < 0)
        return std::nullopt;
    geo.minX = minX, geo.minY = minY, geo.maxX = maxX, geo.maxY = maxY;

    // A cylinder lies along the shape's longer side on screen: a tall shape
    // is a pillar (shaded across its rows), a wide one a log (down its
    // columns).
    const bool tall = (maxY - minY + 1) * kAspect > (maxX - minX + 1);
    const int across = options.shape == FormShape::Cylinder ? (tall ? 1 : 2) : 0;
    geo.across = across;
    auto [lx, ly] = compassVector(light);
    // A cylinder lit along its axis would take the light alike all the way
    // round: when the light's part across the axis is under 0.3 of it, the
    // light is tilted to the diagonal on the side it leans to across the
    // axis (west of a pillar, north of a log, when it leans to neither).
    if (across) {
        double& acrossPart = across == 1 ? lx : ly;
        double& along = across == 1 ? ly : lx;
        const double len = std::hypot(lx, ly);
        if (std::abs(acrossPart) < 0.3 * len) {
            const double side = acrossPart > 0 ? 1.0 : -1.0, way = along > 0 ? 1.0 : along < 0 ? -1.0 : 0.0;
            const double k = way != 0 ? len / std::sqrt(2.0) : len;
            acrossPart = side * k;
            along = way * k;
        }
    }
    geo.lx = lx, geo.ly = ly;
    const double ll = std::sqrt(lx * lx + ly * ly + kLightZ * kLightZ);
    const double Lx = lx / ll, Ly = ly / ll, Lz = kLightZ / ll;
    geo.dist = edgeDistance(shape);
    const std::vector<Normal> normal =
        options.shape == FormShape::Plane ? std::vector<Normal>{} : normals(shape, g, geo.dist, across, ox, oy);

    // How far each cell is from the brightest point (or line): on a dome
    // or a cylinder the angle between its normal and the light (the
    // light's whole direction, across the canvas and out of it) times the
    // form's radius; on a plane the distance along the light. And where the
    // form turns from the light: on a dome or a cylinder the terminator, a
    // right angle from the light; on a plane the middle of its span.
    int cells = 0, runs = 0;
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x)
            if (shape.contains(x, y)) {
                ++cells;
                runs += across == 1 ? !shape.contains(x - 1, y) : across == 2 ? !shape.contains(x, y - 1) : 0;
            }
    geo.cells = cells;
    const double radius = across == 0 ? std::sqrt(cells * kAspect / 3.141592653589793)
                                      : cells * (across == 1 ? 1.0 : kAspect) / (2.0 * std::max(1, runs));
    geo.arc.assign(static_cast<size_t>(g.w) * g.h, 0.0);
    geo.dot.assign(geo.arc.size(), 0.0);
    double nearest = std::numeric_limits<double>::max(), farthest = std::numeric_limits<double>::lowest();
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x) {
            if (!shape.contains(x, y))
                continue;
            double a;
            if (options.shape == FormShape::Plane) {
                a = -((x + ox) * lx + (y + oy) * kAspect * ly);
            } else {
                const Normal& n = normal[g.at(x, y)];
                geo.dot[g.at(x, y)] = std::clamp(n.x * Lx + n.y * Ly + n.z * Lz, -1.0, 1.0);
                a = std::acos(geo.dot[g.at(x, y)]) * radius;
            }
            geo.arc[g.at(x, y)] = a;
            nearest = std::min(nearest, a);
            farthest = std::max(farthest, a);
        }
    geo.nearest = nearest, geo.farthest = farthest, geo.span = farthest - nearest;
    geo.terminator = options.shape == FormShape::Plane ? (nearest + farthest) / 2
                                                       : std::clamp(1.5707963267948966 * radius, nearest, farthest);
    // a plane's light falls off as a round form's would over its span, a
    // right angle at the middle
    if (options.shape == FormShape::Plane)
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x)
                if (shape.contains(x, y))
                    geo.dot[g.at(x, y)] =
                        geo.terminator > nearest
                            ? std::cos(1.5707963267948966 * (geo.arc[g.at(x, y)] - nearest) / (geo.terminator - nearest))
                            : 1.0;
    return geo;
}

// The light's shares of the form: the shadow (the cells turned from it)
// and the cells it lights as brightly as the lit plane's rung or more, the
// rungs from black to `top` taken as the light from none to full.
FormLight lightOf(const Geometry& geo, const Mask& shape, int plane, int top) {
    int dark = 0, lit = 0;
    for (int y = geo.minY; y <= geo.maxY; ++y)
        for (int x = geo.minX; x <= geo.maxX; ++x)
            if (shape.contains(x, y)) {
                const double d = geo.dot[geo.g.at(x, y)];
                dark += d < 0;
                lit += top > 0 && d >= (plane - 0.5) / top;
            }
    return FormLight{double(dark) / geo.cells, double(lit) / geo.cells};
}

} // namespace

FormLight formLight(const Ramp& ramp, const Mask& shape, const FormOptions& options, Compass light, int plane,
                    int highest) {
    const auto geo = geometry(shape, options, light);
    if (!geo || ramp.colours.size() < 2)
        return {};
    return lightOf(*geo, shape, plane, std::clamp(highest < 0 ? ramp.topRung() : highest, 0, ramp.topRung()));
}

namespace {

// The form's work so far: the cells of every try's box and of every lay
// (FormShadeBrush::cellsVisited).
long long visited = 0;

// formShade over a crop of the canvas at (ox, oy) that holds the shape
// with a ring of cells off it (or the canvas's edge), with tries until they
// have covered `budget` cells.
FormResult shadeCrop(const Ramp& ramp, const Mask& shape, int ox, int oy, const FormOptions& options, Compass light,
                     int highest, long budget) {
    FormResult out;
    out.tones.assign(static_cast<size_t>(shape.width()) * shape.height(), -1);
    if (ramp.colours.size() < 2)
        return out;
    const auto found = geometry(shape, options, light, ox, oy);
    if (!found)
        return out;
    const Geometry& geo = *found;
    const Grid& g = geo.g;
    const int minX = geo.minX, minY = geo.minY, maxX = geo.maxX, maxY = geo.maxY, cells = geo.cells;
    const std::vector<double>& arc = geo.arc;
    const std::vector<double>& dist = geo.dist;

    // The lit plane is the top rung, or with a glint the highest solid rung
    // below it (a glint on a shade plane would make the plane a step of the
    // gradient wider than three cells); the glint comes up to the top from
    // it.
    const int top = std::clamp(highest < 0 ? ramp.topRung() : highest, 0, ramp.topRung());
    auto planeOf = [&](bool lit) {
        int p = lit ? top - 1 : top;
        if (lit)
            while (p > 0 && p % kRungsPerStep)
                --p;
        return p;
    };
    const bool glinted = options.specular && options.shape != FormShape::Plane && planeOf(true) >= 1;

    // Where the glint may go, most lit first: the cells clear of the edge
    // (on a cylinder the ones nearer the middle of its length first, of
    // those as lit). A plane has none: its most lit cells are its lit edge.
    std::vector<std::pair<int, int>> spots;
    if (glinted) {
        const double midX = (minX + maxX) / 2.0, midY = (minY + maxY) / 2.0;
        auto offMiddle = [&](int x, int y) {
            return geo.across == 2 ? std::abs(x - midX) : geo.across == 1 ? std::abs(y - midY) : 0.0;
        };
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x)
                if (shape.contains(x, y) && dist[g.at(x, y)] > kGlintInset)
                    spots.emplace_back(x, y);
        auto key = [&](const std::pair<int, int>& c) {
            return std::tuple{std::llround(arc[g.at(c.first, c.second)] * 1e6), offMiddle(c.first, c.second),
                              c.second, c.first};
        };
        std::sort(spots.begin(), spots.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
        if (spots.size() > 256)
            spots.resize(256);
    }
    auto clear = [&](int x, int y) { return shape.contains(x, y) && dist[g.at(x, y)] > kGlintInset; };
    const int side = geo.lx > 0 ? 1 : -1;

    // The glint on a try: the top rung on the first spot with room for it
    // and the more lit of the cells either side of it (towards the light on
    // a tie), falling to the lit plane a rung every `falloff` columns on
    // screen from it (0: a rung a cell, the ladder's) and a rung a cell
    // past that. It has room where every cell it lifts above the plane, and
    // every neighbour of one, is on the shape at the plane's rung, so the
    // glint is a spot on the lit side and never reaches the fall, the
    // shadow or the edge. False where no spot has room.
    auto glintOn = [&](std::vector<int>& v, int plane, double falloff) {
        const int steps = top - plane;
        const int rx = static_cast<int>(std::ceil(falloff * steps)) + steps + 2, ry = rx / 2 + steps + 2;
        for (const auto& [bx, by] : spots) {
            if (v[g.at(bx, by)] < plane)
                continue;
            int nx = clear(bx + side, by) ? bx + side : bx - side;
            if (clear(bx - side, by) && arc[g.at(bx - side, by)] < arc[g.at(nx, by)] - 1e-9)
                nx = bx - side;
            if (!clear(nx, by) || v[g.at(nx, by)] < plane)
                continue;
            const int x0 = std::min(bx, nx) - rx, x1 = std::max(bx, nx) + rx, y0 = by - ry, y1 = by + ry;
            const int ww = x1 - x0 + 1;
            std::vector<int> u(static_cast<size_t>(ww) * (y1 - y0 + 1), plane);
            auto at = [&](int x, int y) -> int& { return u[static_cast<size_t>(y - y0) * ww + (x - x0)]; };
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double d = std::min(std::hypot(x - bx, (y - by) * kAspect), std::hypot(x - nx, (y - by) * kAspect));
                    const int r = d < 0.5 ? top : falloff > 0 ? top - static_cast<int>(std::ceil(d / falloff - 1e-9)) : plane;
                    at(x, y) = std::max(plane, r);
                }
            // the ladder in the window, a rung less a cell out
            for (bool moved = true; moved;) {
                moved = false;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                        for (const auto& d : kNeighbours) {
                            const int ex = x + d[0], ey = y + d[1];
                            if (ex < x0 || ex > x1 || ey < y0 || ey > y1 || at(ex, ey) >= at(x, y) - 1)
                                continue;
                            at(ex, ey) = at(x, y) - 1;
                            moved = true;
                        }
            }
            auto onPlane = [&](int x, int y) { return shape.contains(x, y) && v[g.at(x, y)] >= plane; };
            bool room = true;
            for (int y = y0; y <= y1 && room; ++y)
                for (int x = x0; x <= x1 && room; ++x)
                    if (at(x, y) > plane) {
                        room = onPlane(x, y);
                        for (const auto& d : kNeighbours)
                            room = room && onPlane(x + d[0], y + d[1]);
                    }
            if (!room)
                continue;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (at(x, y) > plane)
                        v[g.at(x, y)] = at(x, y);
            return true;
        }
        return false;
    };

    // A try: the cells nearer the brightest point than the fall are the lit
    // plane, solid; the fall takes every rung from the one below the plane
    // down to the first above the darkest colour, each `band` columns of the
    // arc wide from `start`; past it the shadow is the darkest colour, black
    // on a ramp that starts with it. Then the ladder (neighbours at most a
    // rung apart, the dark side coming up where the form turns faster than
    // the bands), the glint, the rim and the despeckle. Empty where the
    // glint has no room, or the despeckle took it off the plane.
    auto shadeAt = [&](double band, double start, bool lit, double falloff) -> std::optional<std::vector<int>> {
        std::vector<int> v(static_cast<size_t>(g.w) * g.h, -1);
        const int plane = planeOf(lit);
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x)
                if (shape.contains(x, y)) {
                    const double a = arc[g.at(x, y)];
                    v[g.at(x, y)] = a < start ? plane : std::max(0, plane - 1 - static_cast<int>((a - start) / band));
                }
        ladder(v, shape, g, top);
        // A cylinder too thin for the ladder to come down to black still
        // has it on its shadow edge: the cells on the silhouette across the
        // axis that face away from the light, black against the fall as ink
        // (the lint ends a gradient at black). Not with a rim, which lights
        // that edge, and not on a ramp that doesn't start with black.
        if (geo.across && !options.rim && blackRung(ramp, 0)) {
            const int ex = geo.across == 1 ? (geo.lx > 0 ? -1 : 1) : 0, ey = geo.across == 2 ? (geo.ly > 0 ? -1 : 1) : 0;
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x)
                    if (shape.contains(x, y) && !shape.contains(x + ex, y + ey))
                        v[g.at(x, y)] = 0;
        }
        if (lit && !glintOn(v, plane, falloff))
            return std::nullopt;
        if (options.rim) {
            // the edge cells facing away from the light, a rung lighter
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x) {
                    if (!shape.contains(x, y))
                        continue;
                    double ox = 0, oy = 0;
                    for (const auto& d : kNeighbours)
                        if (!shape.contains(x + d[0], y + d[1]))
                            ox += d[0], oy += d[1];
                    const double ol = std::sqrt(ox * ox + oy * oy);
                    if (ol > 0 && (ox * geo.lx + oy * geo.ly) / ol < -0.3)
                        v[g.at(x, y)] = std::min(plane, v[g.at(x, y)] + 1);
                }
            ladder(v, shape, g, top);
        }
        despeckle(v, ramp, shape, g, ramp.topRung());
        if (lit)
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x)
                    if (shape.contains(x, y) && v[g.at(x, y)] > plane)
                        for (const auto& d : kNeighbours)
                            if (!shape.contains(x + d[0], y + d[1]) || v[g.at(x + d[0], y + d[1])] < plane)
                                return std::nullopt;
        return v;
    };
    // The cheap half of a reading first (detail, widest rung, glyphs, the
    // plane, the shares); the lint only for a try that could be kept.
    auto read = [&](const std::vector<int>& v, int plane) {
        Reading r;
        Canvas box(maxX - minX + 1, maxY - minY + 1);
        std::set<char32_t> glyphs;
        int atPlane = 0, lit = 0, dark = 0;
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x)
                if (shape.contains(x, y)) {
                    const int t = v[g.at(x, y)];
                    const Cell c = rungCell(ramp, t);
                    box.at(x - minX, y - minY) = c;
                    if (t % kRungsPerStep)
                        glyphs.insert(c.ch.codePoint().value());  // a shade: a code point
                    atPlane += t == plane;
                    lit += t >= plane;
                    dark += t == 0;
                }
        r.widest = widestTurn(v, shape, g, plane);
        auto peak = [&](int x, int y) {
            if (!shape.contains(x - 1, y) || !shape.contains(x, y) || !shape.contains(x + 1, y))
                return false;
            const int t = v[g.at(x, y)];
            return t > v[g.at(x - 1, y)] && t > v[g.at(x + 1, y)];
        };
        for (int x = minX; x <= maxX; ++x)
            for (int y = minY, run = 0; y <= maxY; ++y) {
                run = peak(x, y) ? run + 1 : 0;
                r.seams += run == 3;
            }
        r.glyphs = static_cast<int>(glyphs.size());
        r.plane = atPlane >= std::max(2, cells / 20);
        r.black = double(dark) / cells;
        r.lit = double(lit) / cells;
        r.corner = lint::detail(box);
        int n = 0;
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 4; ++dx) {
                const auto d = lint::detail(box, lint::Box{dx, dy, box.width() - 1, box.height() - 1});
                if (d.windows == 0)
                    continue;
                r.mean += d.mean;
                r.flat += d.flatPercent;
                ++n;
            }
        r.windows = n > 0;
        if (n)
            r.mean /= n, r.flat /= n;
        return std::pair{r, box};
    };
    auto clean = [&](const Canvas& box) {
        Canvas grid(g.w, g.h);
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x)
                if (shape.contains(x, y))
                    grid.at(x, y) = box.at(x - minX, y - minY);
        lint::LadderOptions atCells;
        atCells.whole = false;
        return lint::checkLadder(grid, ramp, lint::Box{minX, minY, maxX, maxY}, atCells).findings.empty();
    };

    // Where a try's fall starts: centred on the turn or a little either
    // side of it, or just past the share of the form the light lights as
    // brightly as the plane (and a little more): the lit plane keeps at
    // least that share.
    std::vector<double> sortedArc;
    sortedArc.reserve(static_cast<size_t>(cells));
    for (int y = minY; y <= maxY; ++y)
        for (int x = minX; x <= maxX; ++x)
            if (shape.contains(x, y))
                sortedArc.push_back(arc[g.at(x, y)]);
    std::sort(sortedArc.begin(), sortedArc.end());
    struct Try {
        double band, start, falloff;
    };
    auto tries = [&](int plane, bool lit, double floor) {
        std::vector<Try> out;
        constexpr double kBands[] = {1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0};
        const std::vector<double> falloffs = lit ? std::vector<double>{0.0, 1.0} : std::vector<double>{0.0};
        for (const double band : kBands) {
            std::vector<double> starts;
            for (const double shift : {0.0, -0.15, 0.15})
                starts.push_back(geo.terminator + shift * geo.span - (plane - 1) * band / 2);
            for (const double more : {0.0, 0.05, 0.1, 0.2}) {
                const double n = std::ceil(std::min(1.0, floor + more) * cells - 1e-9);
                starts.push_back(n < 1 ? sortedArc.front()
                                       : sortedArc[std::min(sortedArc.size() - 1, static_cast<size_t>(n) - 1)] + 1e-9);
            }
            for (const double start : starts)
                for (const double falloff : falloffs)
                    out.push_back({band, start, falloff});
        }
        return out;
    };

    // The tries with a glint come first, from the most crowded fall (the
    // narrowest bands) out, each at the turn, either side of it and past
    // the lit share, with the glint's falloff none or a column a rung (a
    // wider one rings the two top cells into a bull's-eye that reads as a
    // disc, not a 2-5 cell spot); then the same without a glint. A try is
    // in bounds where its black is no more than the light's shadow and ten
    // points, and its lit plane (its rung and above) no less than the
    // light's lit share. The
    // first try in bounds that clears the bar is the form; when
    // none does, the clean try in bounds that comes nearest, and when no
    // try in bounds is clean, the clean try that comes nearest; when none
    // is clean, nothing. A big form gets as many tries as cover a couple of
    // million cells, so its first dab stays quick: the narrow bands.
    const FormLight lit = lightOf(geo, shape, planeOf(true), top), unlit = lightOf(geo, shape, planeOf(false), top);
    auto finish = [&](std::vector<int> tones, const Reading& r, int plane, bool bounded) {
        out.tones = std::move(tones);
        const FormLight& l = plane == planeOf(false) ? unlit : lit;
        out.reading = FormReading{true, r.passes(), bounded, plane, r.mean, r.flat, r.corner, r.black, r.lit,
                                  l.shadow, l.lit};
        return out;
    };
    const long area = long(maxX - minX + 1) * (maxY - minY + 1);
    long work = 0;
    struct Kept {
        double score;
        std::vector<int> tones;
        Reading reading;
        int plane;
    };
    std::optional<Kept> nearIn, nearAny;
    for (const bool glint : {true, false}) {
        if (glint && !glinted)
            continue;
        const int plane = planeOf(glint);
        const FormLight& l = glint ? lit : unlit;
        for (const Try& at : tries(plane, glint, l.lit)) {
            if (work > budget)
                break;
            work += cells;
            visited += area;
            auto t = shadeAt(at.band, at.start, glint, at.falloff);
            if (!t)
                continue;
            auto [r, box] = read(*t, plane);
            r.clean = true;
            const bool in = r.black <= l.shadow + 0.10 + 1e-9 && r.lit >= l.lit - 1e-9;
            const double score = r.score(glint);
            const bool nearerIn = in && (!nearIn || score > nearIn->score);
            const bool nearerAny = !nearIn && (!nearAny || score > nearAny->score);
            if (!(in && r.passes()) && !nearerIn && !nearerAny)
                continue;
            if (!clean(box))
                continue;
            if (in && r.passes())
                return finish(std::move(*t), r, plane, true);
            if (nearerIn)
                nearIn = Kept{score, *t, r, plane};
            else
                nearAny = Kept{score, std::move(*t), r, plane};
        }
    }
    if (nearIn)
        return finish(std::move(nearIn->tones), nearIn->reading, nearIn->plane, true);
    if (nearAny)
        return finish(std::move(nearAny->tones), nearAny->reading, nearAny->plane, false);
    return out;
}

// A form's tries cover about two million cells, so a select-all of 160×400
// lays its first dab in under 0.2 s.
constexpr long kWork = 2'000'000;

// The shape's cells, cropped from a w × h canvas to their box with a ring
// round it (clipped to the canvas): the form reads nothing further off.
struct Crop {
    int ox = 0, oy = 0;
    Mask shape;
};

template <class Cells>
std::optional<Crop> crop(int w, int h, const Cells& cells) {
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (const auto& [x, y] : cells) {
        x0 = std::min(x0, x), x1 = std::max(x1, x);
        y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    if (x1 < 0)
        return std::nullopt;
    x0 = std::max(0, x0 - 1), y0 = std::max(0, y0 - 1);
    x1 = std::min(w - 1, x1 + 1), y1 = std::min(h - 1, y1 + 1);
    Crop c{x0, y0, Mask(x1 - x0 + 1, y1 - y0 + 1)};
    for (const auto& [x, y] : cells)
        c.shape.set(x - x0, y - y0);
    return c;
}

} // namespace

FormResult formShade(const Ramp& ramp, const Mask& shape, const FormOptions& options, Compass light,
                     int highest) {
    const int w = shape.width(), h = shape.height();
    std::vector<std::pair<int, int>> cells;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (shape.contains(x, y))
                cells.emplace_back(x, y);
    FormResult out;
    out.tones.assign(static_cast<size_t>(w) * h, -1);
    const auto c = crop(w, h, cells);
    if (!c)
        return out;
    FormResult in = shadeCrop(ramp, c->shape, c->ox, c->oy, options, light, highest, kWork);
    out.reading = in.reading;
    const int cw = c->shape.width();
    for (int y = 0; y < c->shape.height(); ++y)
        for (int x = 0; x < cw; ++x)
            out.tones[static_cast<size_t>(y + c->oy) * w + (x + c->ox)] = in.tones[static_cast<size_t>(y) * cw + x];
    return out;
}

std::vector<int> formRungs(const Ramp& ramp, const Mask& shape, const FormOptions& options, Compass light,
                          int highest) {
    return formShade(ramp, shape, options, light, highest).tones;
}

FormShadeBrush::FormShadeBrush(Ramp ramp, FormOptions options) : ramp_(std::move(ramp)), options_(options) {}

long long FormShadeBrush::cellsVisited() { return visited; }

int FormShadeBrush::highest(const Stroke& stroke) const {
    if (stroke.options().ice)
        return ramp_.topRung();
    // With iCE off a shade of two bright neighbours can't be written: the
    // form stops below the first such rung up from the dark end, so every
    // tone is one it can write.
    const int darkest = ramp_.colours[0] == Black ? 1 : 0;
    int r = darkest;
    while (r < ramp_.topRung() && rungCell(ramp_, r + 1).bg.paletteIndex() < 8)
        ++r;
    return r;
}

std::optional<Cell> FormShadeBrush::toneCell(const Stroke& stroke, const std::vector<int>& tones, int x, int y,
                                             double strength) const {
    const Canvas& c = stroke.canvas();
    const int tone = tones[static_cast<size_t>(y) * c.width() + x];
    if (tone < 0)
        return std::nullopt;
    const bool ice = stroke.options().ice;
    const Cell original = c.at(x, y);
    const Cell shown = ice ? original : iceCell(original, false);
    const auto rung = rungOf(ramp_, shown);
    int target = tone;
    if (rung && strength < 1.0) {
        const int n = std::max(1, static_cast<int>(std::lround(strength * ramp_.topRung())));
        target = *rung + std::clamp(tone - *rung, -n, n);
    }
    const ShadeForm form = shadeForm(ramp_, shown).value_or(ShadeForm::LightOnTop);
    // A weaker press from a rung above the form's can still land on one
    // that needs a bright background: the nearest that doesn't, the darker
    // one first.
    auto writable = [&](int r) { return ice || rungCell(ramp_, r, form).bg.paletteIndex() < 8; };
    for (int k = 0; k <= ramp_.topRung(); ++k) {
        if (target - k >= 0 && writable(target - k)) {
            target -= k;
            break;
        }
        if (target + k <= ramp_.topRung() && writable(target + k)) {
            target += k;
            break;
        }
    }
    if (rung && *rung == target)
        return original;
    Cell out = rungCell(ramp_, target, form);
    out.attrs |= original.attrs & attr::Formatting;  // shading keeps the formatting (#360)
    return out;
}

void FormShadeBrush::lay(Stroke& stroke, StrokeLint& guard, Canvas& next, std::vector<Paint> paints) const {
    // The paints are judged by the findings they add to the canvas as the
    // stroke began (the stroke's guard, which reads every row and column
    // through the cells that differ from what the last dab left): a finding
    // that canvas didn't have takes back the changed cells at it and next to
    // it, and where none is that near (a wide rung reported at the far end
    // of its run, a skip further along, one glyph or noise) every changed
    // cell. A cell taken back keeps what the stroke had left it before this
    // dab, so taking cells back ends, at worst, where the last dab left the
    // stroke, which added no finding. `next` is the guard's canvas between
    // dabs.
    std::vector<Cell> before;
    before.reserve(paints.size());
    for (const Paint& p : paints)
        before.push_back(guard.canvas().at(p.x, p.y));
    auto changes = [&](size_t i) { return !(before[i] == paints[i].cell); };
    std::vector<char> kept(paints.size(), 1);
    for (;;) {
        std::vector<std::pair<int, int>> changed;
        for (size_t i = 0; i < paints.size(); ++i) {
            const Paint& p = paints[i];
            next.at(p.x, p.y) = kept[i] && changes(i) ? p.cell : before[i];
            if (!(next.at(p.x, p.y) == before[i]))
                changed.emplace_back(p.x, p.y);
        }
        const auto found = guard.judge(next, changed);
        if (found.empty()) {
            guard.keep();
            break;
        }
        bool dropped = false;
        for (size_t i = 0; i < paints.size(); ++i) {
            if (!kept[i] || !changes(i))
                continue;
            for (const auto& f : found)
                if (std::abs(paints[i].x - f.x) <= 1 && std::abs(paints[i].y - f.y) <= 1) {
                    kept[i] = 0;
                    dropped = true;
                    break;
                }
        }
        if (!dropped)
            for (size_t i = 0; i < paints.size(); ++i)
                kept[i] = 0;
    }
    // each laid cell at its holder's strength, taken back or not
    for (size_t i = 0; i < paints.size(); ++i) {
        const Paint& p = paints[i];
        stroke.set(p.x, p.y, kept[i] ? p.cell : before[i], p.strength, p.image);
    }
}

namespace {

struct Box {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

    bool contains(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    // grown by mx columns and my rows either side, inside a w x h canvas
    Box grown(int mx, int my, int w, int h) const {
        return {std::max(0, x0 - mx), std::max(0, y0 - my), std::min(w - 1, x1 + mx), std::min(h - 1, y1 + my)};
    }
};

using Covered = std::map<std::pair<int, int>, double>;  // (x, y) -> strength

// Calls f(x, y, strength) for each covered cell in the box.
template <class F>
void inBox(const Covered& covered, const Box& b, F f) {
    for (int x = b.x0; x <= b.x1; ++x)
        for (auto it = covered.lower_bound({x, b.y0}); it != covered.end() && it->first.first == x &&
                                                      it->first.second <= b.y1;
             ++it)
            f(x, it->first.second, it->second);
}

// What the form brush keeps over a stroke: for each symmetry image, the
// cells its dabs have covered and the strongest strength each was covered
// at, and with nothing selected the image's form over them.
struct FormState {
    std::map<int, Covered> covered;          // image -> cells
    std::map<int, std::vector<int>> tones;   // image -> its form, a tone a cell of the canvas
    // with a selection, the cells the stroke may paint, worked out once
    std::optional<Mask> selection;
    // the stroke's lint guard, and the canvas a lay works on
    std::optional<StrokeLint> guard;
    std::optional<Canvas> next;
};

} // namespace

void FormShadeBrush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    const Canvas& c = stroke.canvas();
    const int w = c.width(), h = c.height();
    const auto cells = footprint(d.x, d.y, size, stroke.settings().tip, w, h, d.flipX, d.flipY);
    auto& slot = stroke.brushState();
    if (!slot)
        slot = std::make_shared<FormState>();
    auto& state = *static_cast<FormState*>(slot.get());
    // The dab's image covers its cells, each at the strongest dab that has
    // covered it; a dab that covers nothing new or stronger paints nothing.
    auto& covered = state.covered[d.image];
    bool grew = false, stronger = false;
    Box foot{w, h, -1, -1};
    for (const auto& [x, y] : cells) {
        if (!stroke.paintable(x, y))
            continue;
        foot = {std::min(foot.x0, x), std::min(foot.y0, y), std::max(foot.x1, x), std::max(foot.y1, y)};
        const auto [it, added] = covered.try_emplace({x, y}, strength);
        grew = grew || added;
        if (!added && it->second < strength) {
            it->second = strength;
            stronger = true;
        }
    }
    if (!grew && !stronger) {
        if (covered.empty())
            state.covered.erase(d.image);
        return;
    }
    if (!state.guard) {
        state.guard.emplace(c, ramp_);
        state.next = c;
    }
    const int high = highest(stroke);
    const Compass light = stroke.settings().lightDirection;
    const bool selected = static_cast<bool>(stroke.options().selection);
    // The dab lays the covered cells near it: its footprint and a dab's
    // width either side, half that above and below. A cell further off
    // keeps what the stroke has left it, so a dab's work is bounded by its
    // size, however long the stroke.
    Box laid = foot.grown(size + 2, size / 2 + 1, w, h);
    if (selected) {
        // the shape is what the stroke may paint, which holds for the
        // stroke: keyed on the cells themselves, so a new selection or
        // canvas at an old one's address, or a cell gone transparent under
        // the lock, is a new form
        if (!state.selection) {
            Mask shape(w, h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (stroke.paintable(x, y))
                        shape.set(x, y);
            visited += static_cast<long long>(w) * h;
            if (!cached_ || cached_->highest != high || cached_->light != light || !(cached_->shape == shape))
                cached_ = Cached{shape, high, light, formRungs(ramp_, shape, options_, light, high)};
            state.selection = std::move(shape);
        }
    } else if (grew) {
        // The image's form is the cells it has covered, worked out over the
        // ones near the dab: two dab widths and more either side, a dab's
        // height above and below. Where that holds every cell the image
        // has covered it is the whole form, and every one is laid;
        // otherwise only the cells near the dab take their tones from it,
        // clear of the cut, and the rest keep theirs.
        const Box solve = foot.grown(2 * size + 6, size + 3, w, h);
        std::vector<std::pair<int, int>> near;
        inBox(covered, solve, [&](int x, int y, double) { near.emplace_back(x, y); });
        visited += static_cast<long long>(solve.x1 - solve.x0 + 1) * (solve.y1 - solve.y0 + 1);
        const bool whole = near.size() == covered.size();
        if (whole)
            laid = solve;
        auto& tones = state.tones[d.image];
        if (tones.empty())
            tones.assign(static_cast<size_t>(w) * h, -1);
        if (const auto cut = crop(w, h, near)) {
            const FormResult in = shadeCrop(ramp_, cut->shape, cut->ox, cut->oy, options_, light, high, kWork);
            if (in.reading.laid)
                for (const auto& [x, y] : near)
                    if (laid.contains(x, y))
                        tones[static_cast<size_t>(y) * w + x] =
                            in.tones[static_cast<size_t>(y - cut->oy) * cut->shape.width() + (x - cut->ox)];
        }
    }
    // The covered cells near the dab are laid again: each by the image whose
    // dab covered it strongest, the lower image on a tie, from the canvas as
    // the stroke began.
    std::map<std::pair<int, int>, std::pair<double, int>> holder;
    for (const auto& [image, at] : state.covered)
        inBox(at, laid, [&](int x, int y, double s) {
            const auto [it, added] = holder.try_emplace({x, y}, s, image);
            if (!added && s > it->second.first)
                it->second = {s, image};
        });
    std::vector<Paint> paints;
    paints.reserve(holder.size());
    visited += static_cast<long long>(holder.size());
    for (const auto& [xy, hd] : holder) {
        const auto& tones = selected ? cached_->tones : state.tones[hd.second];
        if (tones.empty())
            continue;
        const auto cell = toneCell(stroke, tones, xy.first, xy.second, hd.first);
        const Cell kept = state.guard->canvas().at(xy.first, xy.second);
        paints.push_back({xy.first, xy.second, cell.value_or(kept), hd.first, hd.second});
    }
    lay(stroke, *state.guard, *state.next, std::move(paints));
}

} // namespace bw
