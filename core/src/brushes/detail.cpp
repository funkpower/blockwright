#include "blockwright/brushes/detail.h"

#include "blockwright/ansi.h"
#include "blockwright/brushes/guard.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/halfblock.h"
#include "blockwright/lint.h"
#include "blockwright/selection.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace bw {

namespace {

bool halfBlock(char32_t ch) { return ch == kUpperHalf || ch == kLowerHalf || ch == U'▌' || ch == U'▐'; }

// The ramp's ground: its first colour when that is black (the ground a
// piece is drawn on), never part of the shape; -1 when the ramp doesn't
// start at black and its first colour is part of the shape.
int groundOf(const Ramp& ramp) { return !ramp.colours.empty() && ramp.colours[0] == Black ? 0 : -1; }

// A cell as it shows mirrored across (flipX) and down (flipY): its glyph
// mirrored (a half block's halves swap), its colours kept.
Cell mirroredCell(Cell c, bool flipX, bool flipY) {
    if (flipX)
        c.ch = flipGlyph(c.ch, Flip::Horizontal);
    if (flipY)
        c.ch = flipGlyph(c.ch, Flip::Vertical);
    return c;
}

// A cell of the shape as the rim-light brush reads it, and which of its
// four sub-pixels (2 x 2: ▀ and ▄ split a cell top and bottom, ▌ and ▐ left
// and right) are in the shape.
struct Part {
    bool fringe = false;  // a half block with one half the black ground
    bool in[2][2] = {{true, true}, {true, true}};  // [row][column], top left first
    int level = 0;           // the darker ramp colour it shows (its index)
    bool halfBlock = false;  // a half block
};

thread_local long long cellsRead = 0;

std::optional<Part> partOf(const Ramp& ramp, const Cell& cell, bool ice) {
    ++cellsRead;
    const Cell c = ice ? cell : iceCell(cell, false);
    const int ground = groundOf(ramp);
    if (const auto rung = rungOf(ramp, c)) {
        if (*rung % kRungsPerStep == 0 && *rung / kRungsPerStep == ground)
            return std::nullopt;
        Part p;
        p.level = *rung / kRungsPerStep;
        return p;
    }
    // a half block whose halves are ramp colours, or one a ramp colour and
    // the other black: the fringe a shape tool or the pencil leaves
    if (!halfBlock(c.ch) || (c.attrs & attr::Blink) || !c.fg.isIndexed() || !c.bg.isIndexed())
        return std::nullopt;
    const int f = c.fg.index == Black ? ground : ramp.indexOf(c.fg.index),
              b = c.bg.index == Black ? ground : ramp.indexOf(c.bg.index);
    const bool fBlack = c.fg.index == Black, bBlack = c.bg.index == Black;
    if ((f < 0 && !fBlack) || (b < 0 && !bBlack))
        return std::nullopt;
    const bool fGround = fBlack || f == ground, bGround = bBlack || b == ground;
    if (fGround && bGround)
        return std::nullopt;
    // ▀ is the foreground on top, ▄ below, ▌ on the left, ▐ on the right
    Part p{fGround || bGround};
    p.halfBlock = true;
    p.level = fGround ? b : bGround ? f : std::min(f, b);
    const bool fIn = !fGround, bIn = !bGround;
    if (c.ch == kUpperHalf || c.ch == kLowerHalf) {
        const bool top = c.ch == kUpperHalf ? fIn : bIn, bottom = c.ch == kUpperHalf ? bIn : fIn;
        p.in[0][0] = p.in[0][1] = top;
        p.in[1][0] = p.in[1][1] = bottom;
    } else {
        const bool left = c.ch == U'▌' ? fIn : bIn, right = c.ch == U'▌' ? bIn : fIn;
        p.in[0][0] = p.in[1][0] = left;
        p.in[0][1] = p.in[1][1] = right;
    }
    return p;
}

// A full cell is on the edge when one of its eight neighbours is outside or
// a fringe; a fringe always is.
bool edgeAt(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice, const Part& p) {
    if (p.fringe)
        return true;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!(dx || dy) || !canvas.contains(x + dx, y + dy))
                continue;
            const auto q = partOf(ramp, canvas.at(x + dx, y + dy), ice);
            if (!q || q->fringe)
                return true;
        }
    return false;
}

// Whether the cell at (x, y) is outside the shape: on the canvas and not
// in it. Off the canvas is not outside.
bool outsideAt(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice) {
    return canvas.contains(x, y) && !partOf(ramp, canvas.at(x, y), ice);
}

// The outline's edge: a fringe, or a full cell with an outside cell among
// its eight neighbours (a fringe beside it takes the line in its own black
// half, so it doesn't put the cell on the outline's edge).
bool outlineEdgeAt(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice, const Part& p) {
    if (p.fringe)
        return true;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if ((dx || dy) && outsideAt(canvas, ramp, x + dx, y + dy, ice))
                return true;
    return false;
}

// The window an edge normal is read over, on a grid of 2 x 2 sub-pixels a
// cell, a sub-pixel half a column wide and a column tall on screen: the
// cells within three columns' distance (dx² + 4dy² ≤ 9: three columns each
// way on the cell's row, two on the rows above and below), a window round on
// screen so no direction is favoured. It is 14 sub-pixels across and 6
// down, row r and column col of it in cell (col / 2 - 3, r / 2 - 1) from
// the cell; the cell's own four are rows 2-3, columns 6-7.
struct Window {
    static constexpr int kW = 14, kH = 6;
    // A sub-pixel is outside when its cell is outside, or it is in a
    // fringe's black half; off the canvas and off the window is not outside.
    bool outside[kH][kW] = {};
    // The outside that reaches the cell (windowAt), which the rim light reads.
    bool reaches[kH][kW] = {};
};

// The offset on screen, in columns, from the cell's centre to sub-pixel
// (r, col) of its window: the sub-pixels' centres are a quarter column
// either side of their cell's and half a column above and below it.
std::pair<double, double> offsetOf(int r, int col) {
    return {col / 2 - 3 + (col % 2 ? 0.25 : -0.25), 2 * (r / 2 - 1) + (r % 2 ? 0.5 : -0.5)};
}

// The window round (x, y), and the outside in it that reaches the cell:
// each region of outside sub-pixels (joined by a side or a corner, within
// the window) that touches the cell's own sub-pixels. Where none does, as
// for a full cell under a row of ▄ fringes, whose coloured halves lie
// against it, each region with a sub-pixel in one of its eight neighbours:
// a fringe can't be lit, so the light falls past its coloured half onto the
// cell in from it. Outside beyond the shape doesn't reach the cell: not the
// outside past a narrow shape's far side, nor the outside past the coloured
// half of a fringe when the cell has outside of its own beside it.
Window windowAt(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice) {
    Window w;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            if (dx * dx + 4 * dy * dy > 9 || !canvas.contains(x + dx, y + dy))
                continue;
            const auto p = partOf(ramp, canvas.at(x + dx, y + dy), ice);
            for (int r = 0; r < 2; ++r)
                for (int col = 0; col < 2; ++col)
                    w.outside[2 * (dy + 1) + r][2 * (dx + 3) + col] = !(p && p->in[r][col]);
        }
    // the regions with an outside sub-pixel among rows r0-r1, columns c0-c1
    const auto reach = [&](int r0, int r1, int c0, int c1) {
        std::vector<std::pair<int, int>> todo;
        for (int r = r0; r <= r1; ++r)
            for (int col = c0; col <= c1; ++col)
                if (w.outside[r][col] && !w.reaches[r][col]) {
                    w.reaches[r][col] = true;
                    todo.push_back({r, col});
                }
        const bool any = !todo.empty();
        while (!todo.empty()) {
            const auto [r, col] = todo.back();
            todo.pop_back();
            for (int rr = std::max(r - 1, 0); rr <= std::min(r + 1, Window::kH - 1); ++rr)
                for (int cc = std::max(col - 1, 0); cc <= std::min(col + 1, Window::kW - 1); ++cc)
                    if (w.outside[rr][cc] && !w.reaches[rr][cc]) {
                        w.reaches[rr][cc] = true;
                        todo.push_back({rr, cc});
                    }
        }
        return any;
    };
    // the sub-pixels touching the cell's own, else its eight neighbours'
    if (!reach(1, 4, 5, 8))
        reach(0, Window::kH - 1, 4, 9);
    return w;
}

// The normal over a window: the sum of the offsets from the cell's centre
// to the outside sub-pixels (all of them, or only those that reach the
// cell), each weighted a half, normalised. Weighted so, an outside cell adds
// twice its centre's offset and a ▀/▄ fringe's black half its pixel's, so
// full cells and ▀/▄ fringes have the normals they had on the half-block
// pixel grid. The sums are multiples of an eighth and exact.
std::optional<std::pair<double, double>> normalOf(const Window& w, bool reaching) {
    double gx = 0, gy = 0;
    for (int r = 0; r < Window::kH; ++r)
        for (int col = 0; col < Window::kW; ++col)
            if (reaching ? w.reaches[r][col] : w.outside[r][col]) {
                const auto [ox, oy] = offsetOf(r, col);
                gx += 0.5 * ox;
                gy += 0.5 * oy;
            }
    const double len = std::hypot(gx, gy);
    if (len < 1e-9)
        return std::nullopt;
    return std::pair{gx / len, gy / len};
}

// Whether the cell has outside beside it on the light's side: an outside
// sub-pixel in one of its eight neighbours whose offset from the cell's
// centre, on screen, has a positive dot product with the light's direction
// (never 0: the offsets are odd multiples of a quarter across and of a half
// down). Light falls on a cell only past outside beside it, so a cell whose
// outside all lies behind it, however the outside that reaches it sums,
// isn't lit.
bool exposedTo(const Window& w, Compass light) {
    const auto [lx, ly] = compassVector(light);
    for (int r = 0; r < Window::kH; ++r)
        for (int col = 4; col <= 9; ++col)
            if (w.outside[r][col]) {
                const auto [ox, oy] = offsetOf(r, col);
                if (ox * lx + oy * ly > 0)
                    return true;
            }
    return false;
}

// Whether a light from `light` can reach (x, y) past its neighbours: not
// when the neighbour straight towards the light (a step along each axis the
// light comes from) is a full shape cell while the one straight away from
// it is outside or a fringe. Such a cell is taken to face away from the
// light, into the outside behind it, however its rim normal sums. Only that
// one pair of neighbours is checked; the known limits are in detail.h.
bool openTowards(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice, Compass light) {
    const auto [lx, ly] = compassVector(light);
    const int sx = lx > 1e-9 ? 1 : lx < -1e-9 ? -1 : 0, sy = ly > 1e-9 ? 1 : ly < -1e-9 ? -1 : 0;
    // a cell on the canvas that is outside or a fringe; off the canvas is
    // neither, nor a full shape cell
    const auto part = [&](int cx, int cy) { return partOf(ramp, canvas.at(cx, cy), ice); };
    const bool inside = canvas.contains(x + sx, y + sy) && part(x + sx, y + sy) && !part(x + sx, y + sy)->fringe;
    const bool behind = canvas.contains(x - sx, y - sy) && (!part(x - sx, y - sy) || part(x - sx, y - sy)->fringe);
    return !(inside && behind);
}

} // namespace

long long detailCellsRead() { return cellsRead + lint::cellsRead(); }

bool inShape(const Ramp& ramp, const Cell& cell, bool ice) { return partOf(ramp, cell, ice).has_value(); }

bool onEdge(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice) {
    if (!canvas.contains(x, y))
        return false;
    const auto p = partOf(ramp, canvas.at(x, y), ice);
    return p && edgeAt(canvas, ramp, x, y, ice, *p);
}

std::optional<std::pair<double, double>> edgeNormal(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice) {
    if (!onEdge(canvas, ramp, x, y, ice))
        return std::nullopt;
    return normalOf(windowAt(canvas, ramp, x, y, ice), false);
}

std::optional<std::pair<double, double>> rimNormal(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice) {
    if (!onEdge(canvas, ramp, x, y, ice))
        return std::nullopt;
    return normalOf(windowAt(canvas, ramp, x, y, ice), true);
}

namespace {

// The outline's reading of a canvas (PLAN §10, "Detail"): which cells are
// the shape, its edge and its interior, the level inside each edge cell,
// and the repairs, all read from the one canvas it is built on.
class Outliner {
public:
    // `inside`, when given, is the level inside each cell of a whole canvas
    // `stride` wide (insides() of an Outliner of it), this canvas its box at
    // (ox, oy): the box reads a thin part's level from a body outside it.
    // `shape`, when given, marks the cells of that whole canvas that may be
    // the shape (OutlineShape::Selection): the rest are outside it.
    Outliner(const Canvas& canvas, const Ramp& ramp, bool ice, const std::vector<int>* inside = nullptr, int ox = 0,
             int oy = 0, int stride = 0, const std::vector<char>* shape = nullptr)
        : canvas_(canvas), ramp_(ramp), ice_(ice), w_(canvas.width()), h_(canvas.height()), ground_(groundOf(ramp)),
          floor_((groundOf(ramp) + 1) * kRungsPerStep), part_(size()), edge_(size(), 0), inside_(size(), INT_MAX) {
        const auto inMask = [&](int x, int y) {
            return (*shape)[static_cast<size_t>(y + oy) * static_cast<size_t>(stride) + (x + ox)] != 0;
        };
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (!shape || inMask(x, y))
                    part_[at(x, y)] = partOf(ramp_, canvas_.at(x, y), ice_);
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                const auto& p = part_[at(x, y)];
                if (!p)
                    continue;
                if (!shape) {
                    edge_[at(x, y)] = outlineEdgeAt(canvas_, ramp_, x, y, ice_, *p);
                    continue;
                }
                // outlineEdgeAt, a cell outside the mask outside the shape
                bool edge = p->fringe;
                for (int dy = -1; dy <= 1 && !edge; ++dy)
                    for (int dx = -1; dx <= 1 && !edge; ++dx)
                        edge = (dx || dy) && canvas_.contains(x + dx, y + dy) && !part_[at(x + dx, y + dy)];
                edge_[at(x, y)] = edge;
            }
        if (!inside) {
            insideLevels();
            return;
        }
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (edge_[at(x, y)])
                    inside_[at(x, y)] = (*inside)[static_cast<size_t>(y + oy) * static_cast<size_t>(stride) + (x + ox)];
    }

    // The level inside each edge cell (INT_MAX for none, and off the edge).
    const std::vector<int>& insides() const { return inside_; }

    int width() const { return w_; }
    int height() const { return h_; }
    const Canvas& canvas() const { return canvas_; }
    bool edge(int x, int y) const { return edge_[at(x, y)] != 0; }

    // The cell's rung as it shows, or empty.
    std::optional<int> rung(const Cell& c) const { return rungOf(ramp_, ice_ ? c : iceCell(c, false)); }

    // The values the line may give the edge cell at (x, y), darkest first:
    // the outline colour of the level inside it (lint::outlineRung, or the
    // darkest colour above the ground where that is the ground), then each
    // rung lighter up to the cell's own (for a fringe, the whole cell where
    // it must close, then its black half). Empty where the line leaves it: off
    // the edge, no interior to read, a cell darker than the line already, a
    // half block of two colours, or a fringe whose shape half is no lighter
    // than the line. The cell as it is is always the last resort.
    std::vector<Cell> candidates(int x, int y) const {
        const auto& p = part_[at(x, y)];
        if (!p || !edge_[at(x, y)] || inside_[at(x, y)] == INT_MAX)
            return {};
        const int line = lint::outlineRung(ramp_, inside_[at(x, y)]).value_or(floor_);
        const Cell shown = ice_ ? canvas_.at(x, y) : iceCell(canvas_.at(x, y), false);
        if (p->halfBlock) {
            // the line goes into a fringe's black half, outside the shape's
            // half, where it is darker than that half
            if (!p->fringe || line / kRungsPerStep >= p->level)
                return {};
            std::vector<Cell> out;
            // a shape half with an outside cell beside it along the half
            // (left or right of a ▀/▄, above or below a ▌/▐) would still
            // touch the outside past the line: the whole cell takes the
            // line first (#53)
            const bool rows = shown.ch == kUpperHalf || shown.ch == kLowerHalf;
            const auto outside = [&](int nx, int ny) {
                return nx >= 0 && ny >= 0 && nx < w_ && ny < h_ && !part_[at(nx, ny)];
            };
            if (rows ? outside(x - 1, y) || outside(x + 1, y) : outside(x, y - 1) || outside(x, y + 1))
                out.push_back(rungCell(ramp_, line));
            Cell half = shown;
            const std::uint8_t ink = ramp_.colours[static_cast<size_t>(line / kRungsPerStep)];
            (half.fg.index == Black ? half.fg : half.bg) = Color::indexed(ink);
            if (!ice_ && half.bg.index >= 8) {
                if (half.fg.index >= 8)
                    return out;
                // the other glyph of the pair, colours swapped: the same
                // picture with the background dark
                half.ch = half.ch == kUpperHalf ? kLowerHalf
                          : half.ch == kLowerHalf ? kUpperHalf
                          : half.ch == U'▌'       ? U'▐'
                                                  : U'▌';
                std::swap(half.fg, half.bg);
            }
            out.push_back(half);
            return out;
        }
        const int own = *rung(shown);
        // a cell darker than the line is a line already; a shade into the
        // ground (k░K, the fade of a soft edge) takes the line
        const bool fade = own / kRungsPerStep == ground_;
        if (own < line && !fade)
            return {};
        std::vector<Cell> out;
        for (int r = line; r < std::max(own, line + 1); ++r) {
            if (r != line && fade)
                break;
            const Cell c = rungCell(ramp_, r);
            if (!ice_ && c.bg.isIndexed() && c.bg.index >= 8)
                continue;
            out.push_back(c);
        }
        return out;
    }

    // Whether the edge cell at (x, y) shows the line already: it has an
    // interior to read and is no lighter than the line, and is neither a half
    // block nor a shade into the ground.
    bool lineAlready(int x, int y) const {
        const auto& p = part_[at(x, y)];
        if (!p || p->halfBlock || !edge_[at(x, y)] || inside_[at(x, y)] == INT_MAX)
            return false;
        const int line = lint::outlineRung(ramp_, inside_[at(x, y)]).value_or(floor_);
        const int own = *rung(canvas_.at(x, y));
        return own <= line && own / kRungsPerStep != ground_;
    }

    // A cell of a fade into the ground inside the shape, which a repair may
    // darken.
    bool fade(int x, int y) const {
        const auto& p = part_[at(x, y)];
        if (!p || p->halfBlock || edge_[at(x, y)] || p->level != ground_)
            return false;
        const auto g = rung(canvas_.at(x, y));
        return g && *g % kRungsPerStep != 0;
    }

    // Repairs, from the cells `written` and their neighbours outwards: a fade cell that `covered`
    // allows, as `get` reads it unchanged, that is darker than each of its
    // three or four neighbours on the ladder, one of them changed, would be
    // a speck the line left; it takes the darkest colour above the ground,
    // the line's floor, and its neighbours are looked at again.
    template <class Get, class Covered, class Set>
    void repair(const std::vector<std::pair<int, int>>& written, Get&& get, Covered&& covered, Set&& set) const {
        std::vector<std::pair<int, int>> work;
        for (const auto& [x, y] : written)
            for (const auto& n : {std::pair{x, y}, {x, y - 1}, {x, y + 1}, {x - 1, y}, {x + 1, y}})
                work.push_back(n);
        while (!work.empty()) {
            const auto [x, y] = work.back();
            work.pop_back();
            if (x < 0 || y < 0 || x >= w_ || y >= h_ || !fade(x, y) || !covered(x, y) ||
                !(get(x, y) == canvas_.at(x, y)))
                continue;
            const int own = *rung(canvas_.at(x, y));
            int seen = 0;
            bool changed = false, speck = true;
            for (const auto& [nx, ny] : {std::pair{x, y - 1}, {x, y + 1}, {x - 1, y}, {x + 1, y}}) {
                if (!canvas_.contains(nx, ny))
                    continue;
                const Cell n = get(nx, ny);
                const auto g = rung(n);
                if (!g)
                    continue;
                speck = speck && *g > own;
                ++seen;
                changed = changed || !(n == canvas_.at(nx, ny));
            }
            if (!speck || seen < 3 || !changed)
                continue;
            set(x, y, rungCell(ramp_, floor_));
            for (const auto& n : {std::pair{x, y - 1}, {x, y + 1}, {x - 1, y}, {x + 1, y}})
                work.push_back(n);
        }
    }

private:
    size_t size() const { return static_cast<size_t>(w_) * static_cast<size_t>(h_); }
    size_t at(int x, int y) const { return static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x); }

    // The level inside each edge cell: the darkest rung of the shape's
    // interior cells (not on the edge, not half blocks) among its four
    // neighbours, else among its diagonal ones, else among the nearest
    // interior cells by a walk through the shape (eight ways, no limit), so
    // a thin part of any length takes the level of the body it grows from.
    // A part with no interior at all keeps none and is left alone.
    void insideLevels() {
        std::vector<int> dist(size(), -1), best(size(), INT_MAX);
        std::vector<std::pair<int, int>> layer;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (const auto& p = part_[at(x, y)]; p && !p->halfBlock && !edge_[at(x, y)]) {
                    dist[at(x, y)] = 0;
                    best[at(x, y)] = *rung(canvas_.at(x, y));
                    layer.push_back({x, y});
                }
        for (int d = 1; !layer.empty(); ++d) {
            std::vector<std::pair<int, int>> next;
            for (const auto& [x, y] : layer)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w_ || ny >= h_ || !part_[at(nx, ny)])
                            continue;
                        const size_t n = at(nx, ny);
                        if (dist[n] < 0) {
                            dist[n] = d;
                            next.push_back({nx, ny});
                        }
                        if (dist[n] == d)
                            best[n] = std::min(best[n], best[at(x, y)]);
                    }
            layer = std::move(next);
        }
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                if (!edge_[at(x, y)])
                    continue;
                int four = INT_MAX;
                for (const auto& [nx, ny] : {std::pair{x, y - 1}, {x, y + 1}, {x - 1, y}, {x + 1, y}})
                    if (nx >= 0 && ny >= 0 && nx < w_ && ny < h_ && dist[at(nx, ny)] == 0)
                        four = std::min(four, best[at(nx, ny)]);
                inside_[at(x, y)] = four != INT_MAX ? four : best[at(x, y)];
            }
    }

    const Canvas& canvas_;
    const Ramp& ramp_;
    bool ice_;
    int w_, h_, ground_, floor_;
    std::vector<std::optional<Part>> part_;
    std::vector<char> edge_;
    std::vector<int> inside_;
};

} // namespace

// A stroke's outline, a symmetry image (Dab::image) at a time: each image
// draws the line a stroke along it alone would, on its own copy of the
// stroke-start canvas.
// A mirror image is outlined in its own frame, the canvas mirrored back the
// way the image mirrors it, as the stroke itself is outlined: on a mirrored
// canvas each image's line is the mirror of the stroke's own.
struct OutlineBrush::Plan {
    struct Image {
        bool flipX = false, flipY = false;  // the image's mirrors (Dab::flipX, flipY)
        Canvas origin;                      // the stroke-start canvas, in the image's frame
        std::vector<double> cover;          // the strongest dab over each cell, -1 none
        Canvas result;                      // origin with the image's line
        Canvas work;                        // the same, but while a dab tries a line on it
        lint::LadderIndex index;            // the lint of result
        std::vector<int> inside;            // the level inside each edge cell of origin (Outliner)
        bool settled = false;               // a dab of the image has settled a line
        std::vector<char> shape;            // the cells that may be the shape (OutlineShape::Selection), or empty
        bool pixels = false;                // on a pixel layer (StrokeOptions::pixelLayer), which holds no shade
    };
    // The lint of the stroke-start canvas and its levels inside, which the
    // unmirrored images start from.
    std::optional<lint::LadderIndex> start;
    std::vector<int> inside;
    std::vector<char> shape;  // the unmirrored images' (Image::shape)
    std::map<int, Image> images;
    // The canvas as the stroke shows it, every image's line composited, and
    // its lint: each dab's composite is judged here as a whole.
    std::optional<Canvas> shown;
    std::optional<lint::LadderIndex> shownIndex;
    // The cells each image changed since the composite was last judged, as
    // they were then (in its frame), and the image of the last dab.
    std::map<int, std::map<std::pair<int, int>, Cell>> pending;
    int lastImage = -1;
};

namespace {

using Layer = OutlineBrush::Plan::Image;
using Findings = std::vector<lint::LadderFinding>;

bool global(const lint::LadderFinding& f) {
    return f.kind == lint::LadderFinding::Kind::OneGlyph || f.kind == lint::LadderFinding::Kind::Noise;
}

// Where a finding is, as the guard tells findings apart (LadderFinding::
// place): one of the whole canvas by its kind alone.
std::tuple<int, int, int, int, int> keyOf(const lint::LadderFinding& f) {
    if (global(f))
        return {int(f.kind), -1, -1, -1, -1};
    const auto p = f.place();
    return {int(p.kind), p.x, p.y, p.nx, p.ny};
}

// The line on `canvas`, the box of `layer`'s line at (ox, oy), over the cells
// `cover` allows, settled against the ladder lint of the whole canvas: each
// covered edge cell starts at its darkest value (Outliner::candidates), the
// line and its repairs (on covered cells) are laid over the layer, and where
// that leaves a lint finding the layer didn't have, the line cells it
// involves go one rung lighter, down to the cell as it was. Where no step
// clears a finding, the fewest line cells nearest it that, left as they
// were, clear it are left (all of them for a finding of the whole canvas:
// the ones nearest are taken in order). Then the line's lone fragments go:
// a piece of fewer than three line cells, all of them written here, beside
// an edge cell the line could take but doesn't, all such cells under the
// brush, is left as it was, unless that brings a finding back. The result
// is the box with the line, never a canvas with a finding it lacked.
Canvas settleWith(Layer& layer, int ox, int oy, const Canvas& canvas, const Ramp& ramp, bool ice,
                  const std::vector<char>& cover, const std::vector<int>& inside, bool careful) {
    const Outliner outliner(canvas, ramp, ice, &inside, ox, oy, layer.result.width(),
                            layer.shape.empty() ? nullptr : &layer.shape);
    const int w = canvas.width(), h = canvas.height();
    const auto at = [w](int x, int y) { return static_cast<size_t>(y) * static_cast<size_t>(w) + x; };
    std::vector<std::vector<Cell>> cands(static_cast<size_t>(w) * h);
    std::vector<char> open(cands.size(), 0), already(cands.size(), 0), kept(cands.size(), 0);
    std::vector<std::optional<Cell>> darkest(cands.size());
    std::vector<size_t> pick(cands.size(), 0), opened;
    std::vector<std::pair<int, int>> cells;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = at(x, y);
            if (cover[i])
                opened.push_back(i);
            already[i] = outliner.lineAlready(x, y);
            auto c = outliner.candidates(x, y);
            // a pixel layer holds pixel cells only: the line's shade rungs
            // are left out, so a step lighter is the next whole colour
            if (layer.pixels)
                std::erase_if(c, [](const Cell& k) { return !halfBlocks(k, true); });
            if (c.empty())
                continue;
            open[i] = 1;
            darkest[i] = c.front();
            if (cover[i]) {
                cands[i] = std::move(c);
                cells.push_back({x, y});
            }
        }
    if (cells.empty())
        return canvas;
    // the box with the line `pk` picks, and its repairs
    const auto build = [&](const std::vector<size_t>& pk) {
        Canvas sim = canvas;
        cellsRead += static_cast<long long>(cands.size());
        std::vector<std::pair<int, int>> changed;
        for (const auto& [x, y] : cells)
            if (const size_t i = at(x, y); pk[i] < cands[i].size() && !(cands[i][pk[i]] == canvas.at(x, y))) {
                sim.at(x, y) = cands[i][pk[i]];
                changed.push_back({x, y});
            }
        outliner.repair(
            changed, [&](int x, int y) { return sim.at(x, y); }, [&](int x, int y) { return cover[at(x, y)] != 0; },
            [&](int x, int y, const Cell& c) { sim.at(x, y) = c; });
        return sim;
    };
    // the box laid on the layer's work canvas: the findings it adds to the
    // layer, and the canvas cells it changed (untry puts them back)
    const auto tryOn = [&](const Canvas& sim, std::vector<std::pair<int, int>>& tried) {
        tried.clear();
        for (const size_t i : opened) {
            const int x = int(i % w), y = int(i / w);
            if (!(sim.at(x, y) == canvas.at(x, y))) {
                tried.push_back({x + ox, y + oy});
                layer.work.at(x + ox, y + oy) = sim.at(x, y);
            }
        }
        return layer.index.added(layer.work, tried);
    };
    const auto untry = [&](const std::vector<std::pair<int, int>>& tried) {
        for (const auto& [x, y] : tried)
            layer.work.at(x, y) = layer.result.at(x, y);
    };
    const Canvas& whole = layer.work;
    std::optional<std::vector<size_t>> beforePrune;
    std::vector<size_t> pruned;
    bool oneByOne = false, refined = false;
    for (;;) {
        const Canvas sim = build(pick);
        std::vector<std::pair<int, int>> tried;
        const Findings fresh = tryOn(sim, tried);
        // the line cells a new finding involves: changed ones, or the
        // ones beside a repair it involves (canvas positions, on the work
        // canvas with the box laid on it)
        // Each finding's own culprits, whatever another finding blamed: a
        // finding is blamed across only where nothing along it is to blame.
        std::set<std::pair<int, int>> culprits, mine;
        const auto changedAt = [&](int x, int y) {
            return x >= ox && y >= oy && x < ox + w && y < oy + h && !(whole.at(x, y) == layer.result.at(x, y));
        };
        const auto blame = [&](int x, int y) {
            if (!changedAt(x, y))
                return;
            if (!cands[at(x - ox, y - oy)].empty()) {
                mine.insert({x - ox, y - oy});
                return;
            }
            for (const auto& [nx, ny] : {std::pair{x, y - 1}, {x, y + 1}, {x - 1, y}, {x + 1, y}})
                if (changedAt(nx, ny) && !cands[at(nx - ox, ny - oy)].empty())
                    mine.insert({nx - ox, ny - oy});
        };
        const auto rungAt = [&](int x, int y) -> int {
            if (!whole.contains(x, y))
                return -1;
            ++cellsRead;
            const auto g = rungOf(ramp, whole.at(x, y));
            return g && !(*g % kRungsPerStep == 0 && ramp.colours[static_cast<size_t>(*g / kRungsPerStep)] == Black)
                       ? *g
                       : -1;
        };
        const auto blameRuns = [&](int x0, int y0, int dx, int dy) {
            if (rungAt(x0, y0) < 0)
                return;
            // back to the start of the run, then one run further back
            int x = x0, y = y0;
            for (int runs = 0; runs < 2; ++runs) {
                const int g = rungAt(x, y);
                while (rungAt(x - dx, y - dy) == g)
                    x -= dx, y -= dy;
                if (runs == 0 && rungAt(x - dx, y - dy) >= 0)
                    x -= dx, y -= dy;
                else
                    break;
            }
            // forward over four runs: that one, the finding's and two more
            for (int runs = 0; runs < 4 && rungAt(x, y) >= 0; ++runs) {
                const int g = rungAt(x, y);
                while (rungAt(x, y) == g) {
                    blame(x, y);
                    x += dx, y += dy;
                }
            }
        };
        const auto shadeAt = [&](const Cell& c) {
            const auto g = rungOf(ramp, c);
            return g && *g % kRungsPerStep != 0;
        };
        // Whether the cell's own change raises the noise: the pixels it
        // and its four neighbours show (lint::noise's, drawn and not black
        // with all four neighbours drawn; lonely, unlike all four), counted
        // with the cell as tried and as it was, the rest as tried.
        const double noiseWas = layer.index.noise();
        const auto raisesNoise = [&](int x, int y) {
            const int bx0 = std::max(0, x - 2), by0 = std::max(0, y - 2);
            const int bx1 = std::min(whole.width() - 1, x + 2), by1 = std::min(whole.height() - 1, y + 2);
            Canvas box(bx1 - bx0 + 1, by1 - by0 + 1);
            for (int yy = by0; yy <= by1; ++yy)
                for (int xx = bx0; xx <= bx1; ++xx)
                    box.at(xx - bx0, yy - by0) = whole.at(xx, yy);
            cellsRead += 2LL * box.width() * box.height();
            const auto count = [&](long long& lonely, long long& total) {
                const auto px = lint::pixels(box);
                const int pw = box.width(), ph = 2 * box.height();
                const int cx = x - bx0, cy = y - by0;
                // the cell's two pixels, the pixel above and below them, and
                // the two beside each
                const std::pair<int, int> near[] = {{2 * cy, cx},     {2 * cy + 1, cx},     {2 * cy - 1, cx},
                                                    {2 * cy + 2, cx}, {2 * cy, cx - 1},     {2 * cy, cx + 1},
                                                    {2 * cy + 1, cx - 1}, {2 * cy + 1, cx + 1}};
                for (const auto& [i, j] : near) {
                    if (i < 1 || j < 1 || i + 1 >= ph || j + 1 >= pw)
                        continue;
                    const auto c = px[size_t(i)][size_t(j)];
                    if (!c || *c == Black)
                        continue;
                    const std::optional<std::uint8_t> around[] = {px[size_t(i - 1)][size_t(j)], px[size_t(i + 1)][size_t(j)],
                                                                  px[size_t(i)][size_t(j - 1)], px[size_t(i)][size_t(j + 1)]};
                    if (std::any_of(std::begin(around), std::end(around), [](const auto& n) { return !n; }))
                        continue;
                    ++total;
                    lonely += std::all_of(std::begin(around), std::end(around), [&](const auto& n) { return *n != *c; });
                }
            };
            long long lonelyIs = 0, totalIs = 0, lonelyWas = 0, totalWas = 0;
            count(lonelyIs, totalIs);
            box.at(x - bx0, y - by0) = layer.result.at(x, y);
            count(lonelyWas, totalWas);
            return double(lonelyIs - lonelyWas) > noiseWas * double(totalIs - totalWas) + 1e-12;
        };
        // how many of the findings each cell is one of the cells of
        std::map<std::pair<int, int>, int> involved;
        for (const auto& f : fresh)
            if (f.kind == lint::LadderFinding::Kind::RungSkip) {
                ++involved[{f.x, f.y}];
                ++involved[{f.nx, f.ny}];
            } else if (!global(f)) {
                ++involved[{f.x, f.y}];
            }
        for (const auto& f : fresh) {
            using K = lint::LadderFinding::Kind;
            mine.clear();
            if (f.kind == K::RungSkip || f.kind == K::WideRung) {
                // along the finding's row or column (the way its other
                // cell lies): the run of one rung the finding is in, the
                // run before it and the two after (a skip's far level and
                // the one past it, or what ends a wide run), stopping at
                // black or off the ladder
                const int dx = f.ny == f.y ? 1 : 0, dy = 1 - dx;
                // A skip is first one of its own two cells': of two the line
                // changed, the one more of the findings involve, else the
                // darker (a candidate is never darker than the line, so only
                // a lighter step there closes the gap); of one, that one
                // where it is the darker.
                if (f.kind == K::RungSkip && careful) {
                    const bool one = changedAt(f.x, f.y), two = changedAt(f.nx, f.ny);
                    const bool firstDarker = rungAt(f.x, f.y) < rungAt(f.nx, f.ny);
                    if (one && two) {
                        const int a = involved[{f.x, f.y}], b = involved[{f.nx, f.ny}];
                        (a != b ? a > b : firstDarker) ? blame(f.x, f.y) : blame(f.nx, f.ny);
                    } else if (one && firstDarker) {
                        blame(f.x, f.y);
                    } else if (two && !firstDarker) {
                        blame(f.nx, f.ny);
                    }
                }
                if (mine.empty())
                    blameRuns(f.x, f.y, dx, dy);
                // A wide run's width is also the runs across it, where
                // nothing along it is to blame: the changed cells on the
                // lines across the run within the runs blameRuns reads
                // there (the run at the finding's line and the one before
                // it, and the two after), searched among the trial's changes
                // and walked to no further than the box's side, so the search
                // costs the changes, not the canvas.
                if (f.kind == K::WideRung && mine.empty()) {
                    const int first = dx ? f.x : f.y, last = dx ? f.nx : f.ny, at0 = dx ? f.y : f.x;
                    const int cap = dx ? h : w;
                    for (const auto& [x, y] : tried) {
                        ++cellsRead;
                        const int along = dx ? x : y, across = dx ? y : x;
                        if (along < first || along > last || std::abs(across - at0) > cap)
                            continue;
                        const auto rung = [&](int k) { return dx ? rungAt(x, k) : rungAt(k, y); };
                        const int s = across < at0 ? -1 : 1, most = s < 0 ? 1 : 2;
                        int runs = 0, g = rung(at0);
                        bool within = g >= 0;
                        for (int k = at0; k != across && within;) {
                            k += s;
                            const int n = rung(k);
                            runs += n != g;
                            g = n;
                            within = n >= 0 && runs <= most;
                        }
                        if (within)
                            blame(x, y);
                    }
                }
            } else if (f.kind == K::Speckle) {
                // the speck, or what is round it where the line left it
                blame(f.x, f.y);
                if (mine.empty())
                    for (const auto& [nx, ny] : {std::pair{f.x, f.y - 1}, {f.x, f.y + 1}, {f.x - 1, f.y}, {f.x + 1, f.y}})
                        blame(nx, ny);
            } else if (f.kind == K::Noise) {
                // the cells whose own change raises it
                for (const auto& [x, y] : tried)
                    if (raisesNoise(x, y))
                        blame(x, y);
            } else {
                // one glyph: the cells that change a shade glyph, else all
                for (const auto& [x, y] : tried)
                    if (shadeAt(whole.at(x, y)) || shadeAt(layer.result.at(x, y)))
                        blame(x, y);
                if (mine.empty())
                    for (const auto& [x, y] : tried)
                        blame(x, y);
            }
            culprits.insert(mine.begin(), mine.end());
        }
        // A culprit beside a darker one waits for it: the darker steps
        // first, since a step lighter there can clear what blames both.
        {
            std::set<std::pair<int, int>> first;
            for (const auto& [x, y] : culprits) {
                const int g = rungAt(x + ox, y + oy);
                bool waits = false;
                for (const auto& n : {std::pair{x, y - 1}, {x, y + 1}, {x - 1, y}, {x + 1, y}})
                    waits = waits || (culprits.count(n) && rungAt(n.first + ox, n.second + oy) < g);
                if (!waits)
                    first.insert({x, y});
            }
            culprits = std::move(first);
        }
        untry(tried);
        if (!fresh.empty()) {
            bool moved = false;
            for (const auto& [x, y] : culprits)
                if (const size_t i = at(x, y); pick[i] < cands[i].size()) {
                    ++pick[i];
                    moved = true;
                }
            if (moved)
                continue;
            // no step clears it: undo the last pruning, and try the
            // fragments it took away one by one, keeping one that fails
            // alone
            if (beforePrune) {
                pick = std::move(*beforePrune);
                beforePrune.reset();
                if (oneByOne)
                    for (const size_t i : pruned)
                        kept[i] = 1;
                oneByOne = true;
                continue;
            }
            // else leave the fewest line cells as they were that clears
            // them, nearest the findings first
            std::set<std::tuple<int, int, int, int, int>> keys;
            for (const auto& f : fresh)
                keys.insert(keyOf(f));
            std::vector<std::pair<int, size_t>> order;
            for (const auto& [x, y] : cells) {
                const size_t i = at(x, y);
                if (pick[i] >= cands[i].size() || cands[i][pick[i]] == canvas.at(x, y))
                    continue;
                int near = INT_MAX;
                for (const auto& f : fresh)
                    near = std::min(near, global(f) ? 0 : std::max(std::abs(x + ox - f.x), std::abs(y + oy - f.y)));
                order.push_back({near, i});
            }
            if (order.empty())
                return canvas;
            std::sort(order.begin(), order.end());
            const auto clears = [&](const std::vector<size_t>& pk) {
                std::vector<std::pair<int, int>> t;
                const Findings left = tryOn(build(pk), t);
                untry(t);
                return std::none_of(left.begin(), left.end(), [&](const auto& f) { return keys.count(keyOf(f)); });
            };
            // Noise, which is nowhere, a line raises as a whole (its cells
            // take pixels each other's neighbours count beside), so no cell
            // of it need raise it alone, nor leaving one lower it: from the
            // line all left as it was, its cells go back on the index one at
            // a time, the ones that raise the noise least alone first, each
            // kept where the noise stays at what it was or the most allowed.
            if (std::all_of(fresh.begin(), fresh.end(), [](const auto& f) { return f.kind == lint::LadderFinding::Kind::Noise; })) {
                const double was = layer.index.noise(), most = lint::LadderOptions{}.maxNoise;
                const auto lay = [&](size_t i, bool on) {
                    const int x = int(i % w) + ox, y = int(i / w) + oy;
                    layer.work.at(x, y) = on ? cands[i][pick[i]] : layer.result.at(x, y);
                    layer.index.apply(layer.work, {{x, y}});
                    cellsRead += 2;
                    return layer.index.noise();
                };
                std::vector<std::pair<double, size_t>> turn;
                for (const auto& [near, i] : order) {
                    turn.push_back({lay(i, true), i});
                    lay(i, false);
                }
                std::stable_sort(turn.begin(), turn.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                std::vector<char> on(cands.size(), 0);
                for (const auto& [alone, i] : turn) {
                    if (const double n = lay(i, true); n > most && n > was)
                        lay(i, false);
                    else
                        on[i] = 1;
                }
                std::vector<size_t> pk = pick;
                for (const auto& [alone, i] : turn)
                    if (on[i])
                        lay(i, false);
                    else
                        pk[i] = cands[i].size();
                // the line as a whole, with its repairs, judges it
                if (clears(pk)) {
                    pick = std::move(pk);
                    continue;
                }
            }
            const auto leaving = [&](size_t k) {
                std::vector<size_t> pk = pick;
                for (size_t j = 0; j < k; ++j)
                    pk[order[j].second] = cands[order[j].second].size();
                return pk;
            };
            size_t lo = 1, hi = order.size();
            while (lo < hi) {
                const size_t mid = (lo + hi) / 2;
                if (clears(leaving(mid)))
                    hi = mid;
                else
                    lo = mid + 1;
            }
            pick = leaving(hi);
            continue;
        }
        // Careful, once the line has settled: each cell the settle left as
        // it was goes back a step at a time, alone or with a stepped cell
        // beside it, while that adds nothing to the line as it stands. A
        // step forced by a finding the steps beside it cleared later comes
        // back; the loop then judges the whole again.
        if (careful && !refined) {
            refined = true;
            const std::vector<size_t> before = pick;
            std::vector<std::pair<int, int>> laid;
            for (const size_t i : opened) {
                const int x = int(i % w), y = int(i / w);
                if (!(sim.at(x, y) == canvas.at(x, y))) {
                    laid.push_back({x + ox, y + oy});
                    layer.work.at(x + ox, y + oy) = sim.at(x, y);
                }
            }
            layer.index.apply(layer.work, laid);
            bool any = false;
            for (const auto& [x, y] : cells) {
                const size_t i = at(x, y);
                // only a cell the settle left as it was, the line lost there
                if (pick[i] < cands[i].size() || std::find(opened.begin(), opened.end(), i) == opened.end())
                    continue;
                const auto step = [&](const std::vector<size_t>& is) {
                    std::vector<std::pair<int, int>> at;
                    std::vector<Cell> was;
                    for (const size_t k : is) {
                        at.push_back({int(k % w) + ox, int(k / w) + oy});
                        Cell& cell = layer.work.at(at.back().first, at.back().second);
                        was.push_back(cell);
                        cell = cands[k][pick[k] - 1];
                    }
                    if (!layer.index.added(layer.work, at).empty()) {
                        for (size_t k = 0; k < at.size(); ++k)
                            layer.work.at(at[k].first, at[k].second) = was[k];
                        return false;
                    }
                    layer.index.apply(layer.work, at);
                    for (size_t k = 0; k < is.size(); ++k) {
                        --pick[is[k]];
                        if (std::find(laid.begin(), laid.end(), at[k]) == laid.end())
                            laid.push_back(at[k]);
                    }
                    any = true;
                    return true;
                };
                while (pick[i] > 0 && !kept[i]) {
                    if (step({i}))
                        continue;
                    // or with a stepped cell beside it, the two together
                    bool together = false;
                    for (const auto& [nx, ny] : {std::pair{x + 1, y}, {x, y + 1}, {x - 1, y}, {x, y - 1}}) {
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                            continue;
                        const size_t n = at(nx, ny);
                        if (pick[n] > 0 && !kept[n] && !cands[n].empty() && step({i, n})) {
                            together = true;
                            break;
                        }
                    }
                    if (!together)
                        break;
                }
            }
            for (const auto& [x, y] : laid)
                layer.work.at(x, y) = layer.result.at(x, y);
            layer.index.apply(layer.work, laid);
            // kept only where the refined line, repairs and all, adds
            // nothing as a whole; else the settled line stands
            if (any) {
                std::vector<std::pair<int, int>> t;
                const Findings left = tryOn(build(pick), t);
                untry(t);
                if (left.empty())
                    continue;
                pick = before;
            }
        }
        // the line's lone fragments
        // (a fringe the line went into, whole or by its black half, is of it)
        const auto written = [&](size_t i) { return !(sim.at(i % w, i / w) == canvas.at(i % w, i / w)); };
        const auto lineCell = [&](size_t i) {
            return already[i] || (darkest[i] && (sim.at(i % w, i / w) == *darkest[i] ||
                                                 (!outliner.rung(canvas.at(i % w, i / w)) && written(i))));
        };
        std::vector<char> seen(cands.size(), 0);
        std::vector<size_t> prune, piece(cands.size(), 0);  // the size of each line cell's piece
        for (const auto& [x0, y0] : cells) {
            const size_t i0 = at(x0, y0);
            if (seen[i0] || !lineCell(i0))
                continue;
            std::vector<size_t> part{i0}, members;
            seen[i0] = 1;
            bool alone = true, beside = false, covered = true;
            while (!part.empty()) {
                const size_t i = part.back();
                part.pop_back();
                members.push_back(i);
                const int x = int(i % w), y = int(i / w);
                if (sim.at(x, y) == canvas.at(x, y) || kept[i])
                    alone = false;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if ((!dx && !dy) || !sim.contains(x + dx, y + dy))
                            continue;
                        const size_t n = at(x + dx, y + dy);
                        if (lineCell(n)) {
                            if (!seen[n]) {
                                seen[n] = 1;
                                part.push_back(n);
                            }
                        } else if (open[n]) {
                            beside = true;
                            covered = covered && cover[n];
                        }
                    }
            }
            for (const size_t i : members)
                piece[i] = members.size();
            if (alone && (members.size() == 1 || (beside && covered && members.size() < 3))) {
                prune.insert(prune.end(), members.begin(), members.end());
                if (oneByOne)
                    break;
            }
        }
        // and a corner: a cell written as the line that the line reaches
        // along a row or a column, beside a covered edge cell across it the
        // line leaves, with no line across it (the end of a lined side where
        // the side round the corner stays unlined), in a piece of four or
        // more: the rule never leaves a piece of three or more a fragment.
        if (prune.empty() || !oneByOne)
            for (const auto& [x, y] : cells) {
                const size_t i = at(x, y);
                if (!written(i) || !lineCell(i) || kept[i] || piece[i] < 4)
                    continue;
                const auto lineAt = [&](int nx, int ny) { return sim.contains(nx, ny) && lineCell(at(nx, ny)); };
                const auto leftAt = [&](int nx, int ny) {
                    if (!sim.contains(nx, ny))
                        return false;
                    const size_t n = at(nx, ny);
                    return open[n] && cover[n] && !lineCell(n) && !written(n);
                };
                bool corner = false;
                for (const auto& [dx, dy] : {std::pair{1, 0}, {0, 1}})
                    corner = corner || ((lineAt(x - dx, y - dy) || lineAt(x + dx, y + dy)) &&
                                        (leftAt(x - dy, y - dx) || leftAt(x + dy, y + dx)) &&
                                        !lineAt(x - dy, y - dx) && !lineAt(x + dy, y + dx));
                if (corner) {
                    prune.push_back(i);
                    if (oneByOne)
                        break;
                }
            }
        // and a cell written a step lighter with nothing of the line or
        // nothing written around it
        if (prune.empty() || !oneByOne)
            for (const auto& [x, y] : cells) {
                const size_t i = at(x, y);
                if (!written(i) || lineCell(i) || kept[i])
                    continue;
                bool lone = true;
                for (int dy = -1; dy <= 1 && lone; ++dy)
                    for (int dx = -1; dx <= 1 && lone; ++dx)
                        if ((dx || dy) && sim.contains(x + dx, y + dy)) {
                            const size_t n = at(x + dx, y + dy);
                            lone = !lineCell(n) && !written(n);
                        }
                if (lone) {
                    prune.push_back(i);
                    if (oneByOne)
                        break;
                }
            }
        if (prune.empty())
            return sim;
        beforePrune = pick;
        pruned = prune;
        for (const size_t i : prune)
            pick[i] = cands[i].size();
    }
}

// The cells an outline may read (detailCellsRead) before a careful pass
// starts no more: about a third of a second on the dev machine.
thread_local long long carefulReads = 50'000'000;
// What the last dab's outline read, 0 where it ran none.
thread_local long long outlineRead = 0;

} // namespace

long long setCarefulReads(long long reads) {
    const long long was = carefulReads;
    carefulReads = reads;
    return was;
}

long long outlineCellsRead() { return outlineRead; }

namespace {

// settleWith, then, on an image's first settle (a click, a stroke's start)
// or a dab's settle over cells the stroke has written (layer.settled is
// false for both), where it writes nothing and the outline has read under
// carefulReads since `start`, again with the careful skip blame (a skip
// between two cells the line changed is first the one more findings
// involve), keeping whichever writes more of the box.
Canvas settle(Layer& layer, int ox, int oy, const Canvas& canvas, const Ramp& ramp, bool ice,
              const std::vector<char>& cover, const std::vector<int>& inside, long long start) {
    Canvas out = settleWith(layer, ox, oy, canvas, ramp, ice, cover, inside, false);
    if (layer.settled)
        return out;
    const int w = canvas.width(), h = canvas.height();
    const auto written = [&](const Canvas& c) {
        int n = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                n += !(c.at(x, y) == canvas.at(x, y));
        return n;
    };
    cellsRead += static_cast<long long>(w) * h;
    // a line of three or more connected cells needs three covered
    int covered = 0;
    for (const char c : cover)
        covered += c;
    if (written(out) > 0 || covered < 3 || detailCellsRead() - start >= carefulReads)
        return out;
    Canvas again = settleWith(layer, ox, oy, canvas, ramp, ice, cover, inside, true);
    return written(again) > written(out) ? again : out;
}

// The line over the cells `cover` allows in the box (ox, oy, cw x ch) of the
// layer, settled again on its own result until that changes nothing, and laid
// on the layer: the outline is its own fixed point, and each round adds no
// finding. `was` gets each cell it changes as it was. A round whose plain
// settle writes nothing settles carefully, so the line is careful's fixed
// point too, while the outline has read under carefulReads: a careful pass
// that writes starts more rounds, and each round's work is O(W + H) a
// changed cell, so on a long canvas a click went on for seconds.
void outline(Layer& layer, int ox, int oy, int cw, int ch, const Ramp& ramp, bool ice, const std::vector<char>& cover,
             const std::vector<int>& inside, std::map<std::pair<int, int>, Cell>& was) {
    const long long start = detailCellsRead();
    for (int round = 0; round < 16; ++round) {
        Canvas box(cw, ch);
        cellsRead += static_cast<long long>(cw) * ch;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                box.at(x, y) = layer.result.at(x + ox, y + oy);
        const Canvas out = settle(layer, ox, oy, box, ramp, ice, cover, inside, start);
        std::vector<std::pair<int, int>> changed;
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                if (!(out.at(x, y) == box.at(x, y))) {
                    changed.push_back({x + ox, y + oy});
                    layer.work.at(x + ox, y + oy) = out.at(x, y);
                    was.emplace(std::pair{x + ox, y + oy}, box.at(x, y));
                }
        if (changed.empty())
            break;
        layer.index.apply(layer.work, changed);
        for (const auto& [x, y] : changed)
            layer.result.at(x, y) = layer.work.at(x, y);
    }
    outlineRead = detailCellsRead() - start;
}

// A dab's new cells are settled with the line within kReach of them open
// to change and kContext more of the canvas read round that.
constexpr int kReach = 3;
constexpr int kContext = 8;

// Whether covering (x, y) can change the line: a shape cell on the edge,
// where the line goes, or of the ground's level, a fade a repair may darken.
// `shape`, unless empty, marks the canvas's cells that may be the shape.
bool relevantAt(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice, const std::vector<char>& shape) {
    const int w = canvas.width();
    const auto in = [&](int cx, int cy) { return shape.empty() || shape[static_cast<size_t>(cy) * w + cx]; };
    if (!in(x, y))
        return false;
    const auto p = partOf(ramp, canvas.at(x, y), ice);
    if (!p || p->level == groundOf(ramp) || outlineEdgeAt(canvas, ramp, x, y, ice, *p))
        return p.has_value();
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if ((dx || dy) && canvas.contains(x + dx, y + dy) && !in(x + dx, y + dy))
                return true;
    return false;
}

// The cells of a w x h canvas in the selection, mirrored into an image's
// frame, for OutlineShape::Selection; empty for no selection.
std::vector<char> shapeOf(const std::shared_ptr<const Mask>& selection, int w, int h, bool flipX, bool flipY) {
    std::vector<char> out;
    if (!selection)
        return out;
    out.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[static_cast<size_t>(y) * w + x] = selection->contains(flipX ? w - 1 - x : x, flipY ? h - 1 - y : y);
    return out;
}

} // namespace

OutlineBrush::OutlineBrush(Ramp ramp, OutlineShape shape) : ramp_(std::move(ramp)), shape_(shape) {}

namespace {

// The canvas mirrored across (flipX) and down (flipY), glyphs and all.
Canvas mirrored(const Canvas& c, bool flipX, bool flipY) {
    if (!flipX && !flipY)
        return c;
    const int w = c.width(), h = c.height();
    Canvas out(w, h);
    cellsRead += 2LL * w * h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out.at(flipX ? w - 1 - x : x, flipY ? h - 1 - y : y) = mirroredCell(c.at(x, y), flipX, flipY);
    return out;
}

} // namespace

// The composite is judged once all the images of a dab have drawn (the
// stroke stamps them in order, the dab itself first), so a mirrored stroke
// keeps or refuses a dab and its images together; a dab of a lower image
// than the last one's starts the next group.
void OutlineBrush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    auto& state = stroke.brushState();
    if (!state)
        state = std::make_shared<Plan>();
    Plan* const plan = static_cast<Plan*>(state.get());
    const Canvas& canvas = stroke.canvas();
    if (!plan->pending.empty() && d.image <= plan->lastImage)
        composite(stroke, *plan);
    settleImage(stroke, *plan, d, size, strength);
    plan->lastImage = d.image;
    int last = 0;
    for (const Dab& image : symmetryDabs(stroke.options().symmetry, d, canvas.width(), canvas.height()))
        last = std::max(last, image.image);
    if (d.image >= last && !plan->pending.empty())
        composite(stroke, *plan);
}

void OutlineBrush::settleImage(Stroke& stroke, Plan& planRef, const Dab& d, int size, double strength) const {
    outlineRead = 0;
    const Canvas& canvas = stroke.canvas();
    const bool ice = stroke.options().ice;
    const int w = canvas.width(), h = canvas.height();
    Plan* const plan = &planRef;
    const auto at = [w](int x, int y) { return static_cast<size_t>(y) * static_cast<size_t>(w) + x; };
    if (!plan->start) {
        // once a stroke: the canvas's lint, the level inside its edge cells,
        // and the shown canvas with its lint
        plan->start.emplace(canvas, ramp_);
        if (shape_ == OutlineShape::Selection)
            plan->shape = shapeOf(stroke.options().selection, w, h, false, false);
        plan->inside =
            Outliner(canvas, ramp_, ice, nullptr, 0, 0, w, plan->shape.empty() ? nullptr : &plan->shape).insides();
        plan->shown = canvas;
        plan->shownIndex = *plan->start;
        cellsRead += 2LL * w * h;
    }
    auto image = plan->images.find(d.image);
    if (image == plan->images.end()) {
        // the image's first dab: its frame of the canvas, the lint of it and
        // its levels inside
        cellsRead += 2LL * w * h;
        const bool fx = d.flipX, fy = d.flipY;
        Canvas origin = mirrored(canvas, fx, fy);
        lint::LadderIndex index = fx || fy ? lint::LadderIndex(origin, ramp_) : *plan->start;
        std::vector<char> shape = fx || fy ? std::vector<char>() : plan->shape;
        if ((fx || fy) && !plan->shape.empty())
            shape = shapeOf(stroke.options().selection, w, h, fx, fy);
        std::vector<int> inside =
            fx || fy ? Outliner(origin, ramp_, ice, nullptr, 0, 0, w, shape.empty() ? nullptr : &shape).insides()
                     : plan->inside;
        image = plan->images
                    .emplace(d.image, Layer{fx, fy, origin, std::vector<double>(static_cast<size_t>(w) * h, -1.0), origin,
                                            origin, std::move(index), std::move(inside), false, std::move(shape)})
                    .first;
    }
    Layer& layer = image->second;
    layer.pixels = stroke.options().pixelLayer;
    const Canvas& origin = layer.origin;
    // a cell of the image's frame on the canvas, and back (the same mirror)
    const auto real = [&](const Layer& l, int x, int y) {
        return std::pair{l.flipX ? w - 1 - x : x, l.flipY ? h - 1 - y : y};
    };
    // The cells this dab covers first, or more strongly than a dab before;
    // what an earlier dab wrote on a cell this one covers more strongly goes
    // back to the stroke-start value, to be settled again. In the image's
    // frame the dab is the stroke's own, unflipped.
    const auto [ux, uy] = real(layer, d.x, d.y);
    std::map<std::pair<int, int>, Cell> was;  // (x, y): the cell before this dab
    std::vector<std::pair<int, int>> fresh, reset;
    for (const auto& [x, y] : footprint(ux, uy, size, stroke.settings().tip, w, h, false, false)) {
        double& s = layer.cover[at(x, y)];
        const auto [rx, ry] = real(layer, x, y);
        if (s >= strength || !stroke.paintable(rx, ry))
            continue;
        if (s >= 0 && !(layer.result.at(x, y) == origin.at(x, y))) {
            reset.push_back({x, y});
            was.emplace(std::pair{x, y}, layer.result.at(x, y));
            layer.work.at(x, y) = origin.at(x, y);
        }
        s = strength;
        fresh.push_back({x, y});
    }
    if (fresh.empty())
        return;
    if (!reset.empty()) {
        layer.index.apply(layer.work, reset);
        for (const auto& [x, y] : reset)
            layer.result.at(x, y) = origin.at(x, y);
    }
    // The dab settles the line again from the stroke-start values over the
    // covered cells within kReach of its new cells, against the line beyond
    // them and kContext more of the canvas, so the line there is the covered
    // cells' and not the order of the dabs. A window the stroke has drawn in
    // settles as an image's first settle does, carefully where the plain
    // settle writes nothing. Where that would take back a cell the stroke has
    // written or leave a lone cell of line, the dab settles only the covered
    // cells the stroke has left as they were, and the line so far stays: a
    // dab never takes back the line an earlier one drew.
    {
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (const auto& [x, y] : fresh)
            x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
        x0 = std::max(0, x0 - kReach), y0 = std::max(0, y0 - kReach);
        x1 = std::min(w - 1, x1 + kReach), y1 = std::min(h - 1, y1 + kReach);
        const int cx0 = std::max(0, x0 - kContext), cy0 = std::max(0, y0 - kContext);
        const int cx1 = std::min(w - 1, x1 + kContext), cy1 = std::min(h - 1, y1 + kContext);
        const int cw = cx1 - cx0 + 1, ch = cy1 - cy0 + 1;
        const bool wasSettled = layer.settled;
        // settles the window's covered cells, the ones the stroke has written
        // too (from their stroke-start values) when `again`; the cells it
        // opened again
        const auto settleWindow = [&](bool again) {
            std::vector<char> cover(static_cast<size_t>(cw) * static_cast<size_t>(ch), 0);
            bool any = false;
            std::vector<std::pair<int, int>> reopen;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    if (layer.cover[at(x, y)] < 0 || !relevantAt(origin, ramp_, x, y, ice, layer.shape))
                        continue;
                    if (!(layer.result.at(x, y) == origin.at(x, y))) {
                        if (!again)
                            continue;
                        reopen.push_back({x, y});
                        was.emplace(std::pair{x, y}, layer.result.at(x, y));
                        layer.work.at(x, y) = origin.at(x, y);
                    }
                    cover[static_cast<size_t>(y - cy0) * static_cast<size_t>(cw) + (x - cx0)] = 1, any = true;
                }
            if (!reopen.empty()) {
                layer.index.apply(layer.work, reopen);
                for (const auto& [x, y] : reopen)
                    layer.result.at(x, y) = origin.at(x, y);
                layer.settled = false;
            }
            if (any) {
                outline(layer, cx0, cy0, cw, ch, ramp_, ice, cover, layer.inside, was);
                layer.settled = true;
            }
            return reopen;
        };
        const auto reopened = settleWindow(true);
        const auto written = [&](int x, int y) {
            return x >= 0 && y >= 0 && x < w && y < h && !(layer.result.at(x, y) == origin.at(x, y));
        };
        bool takesBack = false;
        for (const auto& [x, y] : reopened)
            takesBack = takesBack || !written(x, y);
        for (int y = y0 - 1; !takesBack && y <= y1 + 1; ++y)
            for (int x = x0 - 1; !takesBack && x <= x1 + 1; ++x) {
                bool beside = false;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        beside = beside || ((dx || dy) && written(x + dx, y + dy));
                takesBack = written(x, y) && !beside;
            }
        if (takesBack) {
            // back to the line before the dab, with its stronger cells reset
            const std::set<std::pair<int, int>> wasReset(reset.begin(), reset.end());
            std::vector<std::pair<int, int>> undo;
            for (const auto& [p, old] : was) {
                const Cell to = wasReset.count(p) ? origin.at(p.first, p.second) : old;
                if (!(layer.result.at(p.first, p.second) == to)) {
                    layer.work.at(p.first, p.second) = to;
                    undo.push_back(p);
                }
            }
            layer.index.apply(layer.work, undo);
            for (const auto& [x, y] : undo)
                layer.result.at(x, y) = layer.work.at(x, y);
            layer.settled = wasSettled;
            settleWindow(false);
        }
    }
    // The cells the dab changed, and the image as it was before it (on the
    // work canvas).
    std::vector<std::pair<int, int>> changed;
    for (const auto& [p, old] : was)
        if (!(layer.result.at(p.first, p.second) == old)) {
            changed.push_back(p);
            layer.work.at(p.first, p.second) = old;
        }
    if (changed.empty())
        return;
    // The dab adds no finding to the image's line, judged from the image as
    // it was before the dab: the index goes back, then forward again. A dab
    // that would adds nothing.
    {
        const Canvas& after = layer.result;
        layer.index.apply(layer.work, changed);
        const bool adds = !layer.index.apply(after, changed).empty();
        if (adds) {
            layer.index.apply(layer.work, changed);
            for (const auto& [x, y] : changed)
                layer.result.at(x, y) = layer.work.at(x, y);
        } else {
            auto& keep = plan->pending[d.image];
            for (const auto& [x, y] : changed)
                keep.emplace(std::pair{x, y}, layer.work.at(x, y));
        }
        for (const auto& [x, y] : changed)
            layer.work.at(x, y) = layer.result.at(x, y);
    }
}

void OutlineBrush::composite(Stroke& stroke, Plan& plan) const {
    const Canvas& canvas = stroke.canvas();
    const int w = canvas.width(), h = canvas.height();
    const auto at = [w](int x, int y) { return static_cast<size_t>(y) * static_cast<size_t>(w) + x; };
    const auto real = [&](const Layer& l, int x, int y) {
        return std::pair{l.flipX ? w - 1 - x : x, l.flipY ? h - 1 - y : y};
    };
    std::set<std::pair<int, int>> cells;
    for (const auto& [k, keep] : plan.pending)
        for (const auto& [p, old] : keep)
            cells.insert(real(plan.images.at(k), p.first, p.second));
    // Each cell shows the line of the image that covered it most strongly
    // and changed it. On a tie, the image whose frame has the cell in the
    // canvas's first half across, then down, wins, then the lower image: a
    // cell and its mirror cell take the lines of two images that mirror each
    // other, so a mirrored canvas and stroke get a mirrored line.
    Canvas& shown = *plan.shown;
    std::vector<std::pair<int, int>> shows;
    std::vector<std::pair<std::pair<int, int>, Cell>> before;
    std::vector<double> strengths;
    for (const auto& [x, y] : cells) {
        std::optional<Cell> best;
        double strongest = -1;
        std::tuple<bool, bool, int> bestRank{};
        for (const auto& [k, other] : plan.images) {
            const auto [ox, oy] = real(other, x, y);
            const double s = other.cover[at(ox, oy)];
            if (s < 0 || other.result.at(ox, oy) == other.origin.at(ox, oy))
                continue;
            const std::tuple<bool, bool, int> r{2 * ox > w - 1, 2 * oy > h - 1, k};
            if (!best || s > strongest || (s == strongest && r < bestRank)) {
                best = mirroredCell(other.result.at(ox, oy), other.flipX, other.flipY);
                strongest = s;
                bestRank = r;
            }
        }
        const Cell cell = best ? *best : canvas.at(x, y);
        if (cell == shown.at(x, y))
            continue;
        before.push_back({{x, y}, shown.at(x, y)});
        shown.at(x, y) = cell;
        shows.push_back({x, y});
        strengths.push_back(best ? strongest : 0.0);
    }
    // The composite adds no finding to the canvas the stroke shows. Images
    // settled apart can meet in a finding, so the composite settles first:
    // a cell to blame goes back to what the stroke showed, with its mirror
    // cells, an orbit at a time, until it adds nothing. Where nothing is to
    // blame, the images' dabs since the last judgement add nothing.
    if (shows.empty()) {
        plan.pending.clear();
        return;
    }
    std::map<std::pair<int, int>, Cell> was(before.begin(), before.end());
    const auto changed = [&](int x, int y) {
        const auto it = was.find({x, y});
        return it != was.end() && !(shown.at(x, y) == it->second);
    };
    const auto rung = [&](int x, int y) {
        ++cellsRead;
        return shown.contains(x, y) ? rungOf(ramp_, shown.at(x, y)).value_or(-1) : -1;
    };
    const auto shade = [&](const Cell& c) {
        const auto g = rungOf(ramp_, c);
        return g && *g % kRungsPerStep != 0;
    };
    // the cell and its images under the stroke's mirrors
    std::set<std::pair<bool, bool>> flips;
    for (const auto& [k, l] : plan.images)
        flips.insert({l.flipX, l.flipY});
    const auto orbit = [&](int x, int y) {
        std::set<std::pair<int, int>> out;
        for (const auto& [fx, fy] : flips)
            out.insert({fx ? w - 1 - x : x, fy ? h - 1 - y : y});
        return out;
    };
    std::set<std::pair<int, int>> reverted;
    // What the composite adds to the stroke's start, the index moved to
    // `shown`: what the move adds, but for skips, read from the lines the
    // index judged again (the rows and columns within two of a shown cell)
    // against the start's, since a skip that matches one the canvas showed
    // may have moved off the start's (a level slid a composite at a time).
    std::vector<int> skipRows, skipColumns;
    for (const auto& [x, y] : shows)
        for (int d = -2; d <= 2; ++d) {
            if (y + d >= 0 && y + d < h)
                skipRows.push_back(y + d);
            if (x + d >= 0 && x + d < w)
                skipColumns.push_back(x + d);
        }
    for (auto* lines : {&skipRows, &skipColumns}) {
        std::sort(lines->begin(), lines->end());
        lines->erase(std::unique(lines->begin(), lines->end()), lines->end());
    }
    const auto adds = [&] {
        Findings out, rest;
        for (auto& f : plan.shownIndex->apply(shown, shows))
            if (f.kind != lint::LadderFinding::Kind::RungSkip)
                rest.push_back(std::move(f));
        for (const bool across : {true, false})
            for (const int line : across ? skipRows : skipColumns)
                for (auto& f : plan.shownIndex->skipsAdded(across, line, plan.start->skips(across, line)))
                    out.push_back(std::move(f));
        out.insert(out.end(), rest.begin(), rest.end());
        return out;
    };
    // most composites add nothing: one apply; else the index goes back and
    // the composite settles against it
    auto fresh = adds();
    if (fresh.empty()) {
        plan.pending.clear();
        for (size_t k = 0; k < shows.size(); ++k)
            stroke.set(shows[k].first, shows[k].second, shown.at(shows[k].first, shows[k].second), strengths[k]);
        return;
    }
    // the index goes back to what the stroke showed: `shown` with the
    // composite's cells as they were before it, swapped in for the apply
    // and out again (the index holds its own canvas), so going back reads
    // the composite's cells and not the canvas; out again if the apply
    // throws too
    const auto indexBack = [&] {
        std::vector<Cell> now;
        now.reserve(before.size());
        for (const auto& [p, cell] : before) {
            now.push_back(shown.at(p.first, p.second));
            shown.at(p.first, p.second) = cell;
        }
        struct Out {
            Canvas& shown;
            const std::vector<std::pair<std::pair<int, int>, Cell>>& at;
            const std::vector<Cell>& now;
            ~Out() {
                for (size_t k = 0; k < at.size(); ++k)
                    shown.at(at[k].first.first, at[k].first.second) = now[k];
            }
        } out{shown, before, now};
        cellsRead += 2LL * static_cast<long long>(before.size());
        plan.shownIndex->apply(shown, shows);
    };
    indexBack();
    for (size_t round = 0; !fresh.empty() && round < shows.size(); ++round) {
        using K = lint::LadderFinding::Kind;
        // the first finding's cells to blame, and the one that goes back:
        // nearest it, or for one of the whole canvas the one most changed
        // cells are beside; ties to the orbit first in the canvas
        const auto& f = fresh.front();
        std::vector<std::pair<int, int>> blamed;
        if (f.kind == K::RungSkip) {
            const bool one = changed(f.x, f.y), two = changed(f.nx, f.ny);
            if (one && two)
                blamed.push_back(rung(f.x, f.y) < rung(f.nx, f.ny) ? std::pair{f.x, f.y} : std::pair{f.nx, f.ny});
            else if (one || two)
                blamed.push_back(one ? std::pair{f.x, f.y} : std::pair{f.nx, f.ny});
        } else if (f.kind == K::Speckle) {
            for (const auto& p : {std::pair{f.x, f.y}, {f.x, f.y - 1}, {f.x, f.y + 1}, {f.x - 1, f.y}, {f.x + 1, f.y}})
                if (changed(p.first, p.second))
                    blamed.push_back(p);
        }
        if (blamed.empty())
            for (const auto& [p, cell] : before) {
                ++cellsRead;
                if (!changed(p.first, p.second))
                    continue;
                if (f.kind == K::OneGlyph && !shade(cell) && !shade(shown.at(p.first, p.second)))
                    continue;
                if (f.kind == K::WideRung) {
                    const bool across = f.ny == f.y;
                    const int along = across ? p.first : p.second, first = across ? f.x : f.y,
                              last = across ? f.nx : f.ny;
                    if (along < first || along > last)
                        continue;
                }
                blamed.push_back(p);
            }
        if (blamed.empty())
            break;
        const auto key = [&](const std::pair<int, int>& p) {
            int near = 0;
            if (global(f)) {
                for (const auto& n : {std::pair{p.first, p.second - 1}, {p.first, p.second + 1}, {p.first - 1, p.second},
                                      {p.first + 1, p.second}})
                    near -= changed(n.first, n.second);
            } else {
                near = std::max(std::abs(p.first - f.x), std::abs(p.second - f.y));
            }
            const auto o = orbit(p.first, p.second);
            const auto first = *o.begin();
            return std::tuple{near, first.second, first.first};
        };
        const auto pick = *std::min_element(blamed.begin(), blamed.end(),
                                            [&](const auto& a, const auto& b) { return key(a) < key(b); });
        for (const auto& p : orbit(pick.first, pick.second))
            if (changed(p.first, p.second)) {
                shown.at(p.first, p.second) = was.at(p);
                reverted.insert(p);
            }
        fresh = adds();
        indexBack();
    }
    if (!fresh.empty()) {
        for (const auto& [p, cell] : before)
            shown.at(p.first, p.second) = cell;
        reverted.clear();
        for (const auto& [p, cell] : before)
            reverted.insert(p);
    }
    plan.shownIndex->apply(shown, shows);
    // the images take back what the composite did: their cells shown as the
    // stroke showed them before go back in each image too
    for (auto& [k, keep] : plan.pending) {
        Layer& l = plan.images.at(k);
        std::vector<std::pair<int, int>> back;
        for (const auto& [p, old] : keep)
            if (reverted.count(real(l, p.first, p.second)) || !fresh.empty()) {
                l.work.at(p.first, p.second) = old;
                back.push_back(p);
            }
        l.index.apply(l.work, back);
        for (const auto& [x, y] : back)
            l.result.at(x, y) = l.work.at(x, y);
    }
    plan.pending.clear();
    if (!fresh.empty())
        return;
    for (size_t k = 0; k < shows.size(); ++k)
        if (!reverted.count(shows[k]))
            stroke.set(shows[k].first, shows[k].second, shown.at(shows[k].first, shows[k].second), strengths[k]);
}

namespace {

thread_local long long refusalLookupCount = 0;

} // namespace

long long RimLightBrush::refusalLookups() { return refusalLookupCount; }

RimLightBrush::RimLightBrush(Ramp ramp, Compass light, bool keepLint)
    : ramp_(std::move(ramp)), light_(light), keepLint_(keepLint) {}

bool RimLightBrush::facesLight(std::pair<double, double> normal, Compass light) {
    // the compass is on screen, as the normal is
    const auto [lx, ly] = compassVector(light);
    return normal.first * lx + normal.second * ly > 0.3;
}

std::optional<Cell> RimLightBrush::paintCell(const Stroke& stroke, int x, int y, double) const {
    const Canvas& c = stroke.canvas();
    const bool ice = stroke.options().ice;
    // A fringe can't go a rung lighter (a half block has one colour a half,
    // and a rung is a shade between two), so it stays; the full cells round
    // it are on the edge, and lit where their own normal faces the light
    const auto part = partOf(ramp_, c.at(x, y), ice);
    if (!part || part->fringe || !edgeAt(c, ramp_, x, y, ice, *part))
        return std::nullopt;
    const Window w = windowAt(c, ramp_, x, y, ice);
    const auto n = normalOf(w, true);
    if (!n || !facesLight(*n, light_) || !exposedTo(w, light_) || !openTowards(c, ramp_, x, y, ice, light_))
        return std::nullopt;
    return stepRung(ramp_, c.at(x, y), 1, ice);
}

void RimLightBrush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    if (!keepLint_) {
        Brush::dab(stroke, d, size, strength);
        return;
    }
    const Canvas& c = stroke.canvas();
    const int W = c.width(), H = c.height();
    const Tip tip = stroke.settings().tip;
    using Pos = std::pair<int, int>;
    // The stroke's lint guard, read from its start at the first dab, the
    // canvas a stamp is judged on (the guard's, between stamps), and the
    // cells a stamp would have lit and the guard refused.
    struct State {
        std::set<std::tuple<int, int, bool, bool, int, double>> done;
        int lastImage = 0;
        std::optional<StrokeLint> guard;
        std::optional<Canvas> next;
        std::set<Pos> refused;
    };
    auto& slot = stroke.brushState();
    if (!slot)
        slot = std::make_shared<State>();
    State& st = *static_cast<State*>(slot.get());
    if (!st.guard) {
        st.guard.emplace(c, ramp_);
        st.next = c;
    }
    // Under a mirror the dab's images are lit together, each cell with its
    // images, as the despeckle brush cleans them, so a symmetric canvas
    // stays symmetric.
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
    // The cells the stamp would light, and the ones refused before beside
    // them: a cell that would be a speck alone can be lit with the next one
    // along the rim.
    std::map<Pos, Cell> lit;  // (y, x) -> its lit cell
    auto consider = [&](int x, int y) {
        if (lit.count({y, x}))
            return true;
        if (stroke.painted(x, y) || !stroke.paintable(x, y))
            return false;
        const auto out = paintCell(stroke, x, y, strength);
        if (!out || *out == c.at(x, y))
            return false;
        lit.emplace(Pos{y, x}, *out);
        return true;
    };
    // A cell with its images under the mirror, itself first.
    auto orbit = [&](int x, int y) {
        std::vector<Pos> out{{x, y}};
        if (mirror)
            for (const Dab& o : symmetryDabs(sym, Dab{x, y}, W, H))
                if (std::find(out.begin(), out.end(), Pos{o.x, o.y}) == out.end())
                    out.push_back({o.x, o.y});
        return out;
    };
    for (const Dab& o : images)
        for (const auto& [x, y] : footprint(o.x, o.y, size, tip, W, H, o.flipX, o.flipY))
            consider(x, y);
    if (lit.empty())
        return;
    // The refused cells among the eight neighbours of the cells the stamp
    // would light, looked up there: the work a stamp does is its own cells',
    // however many cells the stroke has refused. Each is tried again with
    // its images; one that can no longer light leaves the refused cells.
    std::set<Pos> again;  // (x, y)
    for (const auto& [at, out] : lit)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx || dy) {
                    ++refusalLookupCount;
                    if (const Pos p{at.second + dx, at.first + dy}; st.refused.count(p))
                        again.insert(p);
                }
    for (const auto& [x, y] : again) {
        const auto members = orbit(x, y);
        if (!std::all_of(members.begin(), members.end(), [&](const Pos& p) { return consider(p.first, p.second); }))
            for (const Pos& p : members)
                st.refused.erase(p);
    }
    // Each cell with its images under the mirror: a group lights or gives
    // way as one, and a cell lights only when every image of it would (each
    // image unpainted, selected and facing the light), so an orbit with an
    // image the stamp can't light stays as it is. Groups in row order, by
    // their first cell.
    std::vector<std::vector<std::pair<Pos, Cell>>> groups;  // ((x, y), cell)
    {
        std::set<Pos> seen;  // (y, x)
        for (const auto& [at, out] : lit) {
            if (seen.count(at))
                continue;
            std::vector<std::pair<Pos, Cell>> group;
            bool whole = true;
            for (const Pos& p : orbit(at.second, at.first)) {
                seen.insert({p.second, p.first});
                if (const auto it = lit.find({p.second, p.first}); it != lit.end())
                    group.push_back({p, it->second});
                else
                    whole = false;
            }
            if (whole)
                groups.push_back(std::move(group));
            else
                for (const auto& [p, cell] : group)
                    st.refused.erase(p);
        }
    }
    if (groups.empty())
        return;
    // The stamp is judged by the findings it would add to the stroke's
    // start, by the guard. A finding takes back the groups with a cell at it
    // or next to it, either end of a skip or a wide rung's run, and where no
    // group is that near every one, as the form brush takes cells back; then
    // each group taken back is tried alone, in row order, again while one
    // lights, so a group the stamp tries is left unlit only where lighting
    // it alone would add a finding.
    StrokeLint& guard = *st.guard;
    Canvas& next = *st.next;
    std::vector<char> on(groups.size(), 1);
    auto judge = [&](const std::vector<size_t>& trying) {
        std::vector<std::pair<int, int>> changed;
        for (size_t g : trying)
            for (const auto& [p, out] : groups[g]) {
                next.at(p.first, p.second) = out;
                changed.push_back(p);
            }
        const auto found = guard.judge(next, changed);
        if (found.empty())
            guard.keep();
        else
            for (const auto& p : changed)
                next.at(p.first, p.second) = guard.canvas().at(p.first, p.second);
        return found;
    };
    for (;;) {
        std::vector<size_t> trying;
        for (size_t g = 0; g < groups.size(); ++g)
            if (on[g])
                trying.push_back(g);
        if (trying.empty())
            break;
        const auto found = judge(trying);
        if (found.empty())
            break;
        bool dropped = false;
        for (size_t g : trying)
            for (const auto& [p, out] : groups[g]) {
                const bool near = std::any_of(found.begin(), found.end(), [&](const lint::LadderFinding& f) {
                    return (std::abs(p.first - f.x) <= 1 && std::abs(p.second - f.y) <= 1) ||
                           (std::abs(p.first - f.nx) <= 1 && std::abs(p.second - f.ny) <= 1);
                });
                if (near) {
                    on[g] = 0;
                    dropped = true;
                    break;
                }
            }
        if (!dropped)
            std::fill(on.begin(), on.end(), 0);
    }
    for (bool more = true; more;) {
        more = false;
        for (size_t g = 0; g < groups.size(); ++g)
            if (!on[g] && judge({g}).empty())
                on[g] = 1, more = true;
    }
    for (size_t g = 0; g < groups.size(); ++g)
        for (const auto& [p, out] : groups[g]) {
            if (on[g]) {
                stroke.set(p.first, p.second, out, strength);
                st.refused.erase(p);
            } else {
                st.refused.insert(p);
            }
        }
}

} // namespace bw
