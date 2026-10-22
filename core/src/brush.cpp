#include "blockwright/brush.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace bw {

Mask::Mask(int width, int height, bool all)
    : w_(std::max(0, width)), h_(std::max(0, height)), bits_(static_cast<size_t>(w_) * h_, all) {}

Mask Mask::rect(int width, int height, int x0, int y0, int x1, int y1) {
    Mask m(width, height);
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    for (int y = std::max(0, y0); y <= std::min(m.h_ - 1, y1); ++y)
        for (int x = std::max(0, x0); x <= std::min(m.w_ - 1, x1); ++x)
            m.set(x, y);
    return m;
}

void Mask::set(int x, int y, bool on) {
    if (x >= 0 && y >= 0 && x < w_ && y < h_)
        bits_[static_cast<size_t>(y) * w_ + x] = on;
}

std::shared_ptr<const Mask> selectionMask(const Selection& sel, int width, int height) {
    if (sel.empty())
        return nullptr;
    auto m = std::make_shared<Mask>(width, height);
    const CellRect r = sel.bounds().intersected(CellRect{0, 0, width, height});
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x)
            if (sel.contains(x, y))
                m->set(x, y);
    return m;
}

namespace {

// The footprint's cells in columns x0..x1 and rows y0..y1, which lie within
// int. Only the cells in range are visited, so a big brush costs no more
// than the canvas.
std::vector<std::pair<int, int>> cover(int x, int y, int size, Tip tip, long long x0, long long y0, long long x1,
                                       long long y1) {
    size = std::clamp(size, 1, kMaxBrushSize);
    std::vector<std::pair<int, int>> out;
    const int lo = -(size / 2), hi = lo + size - 1;
    const long long dxLo = std::max<long long>(lo, x0 - x), dxHi = std::min<long long>(hi, x1 - x);
    // Round: cell centres within a circle `size` columns across, rows two
    // columns tall. An odd size centres on the dab; an even one is centred
    // half a cell left of it, and a multiple of four (an even number of
    // rows) half a row up as well, so every size keeps its rows.
    const double r = size / 2.0, shiftX = size % 2 ? 0.0 : 0.5, shiftY = size % 4 == 0 ? 1.0 : 0.0;
    const int rows = static_cast<int>(r / 2.0) + 2;
    const long long dyLo = std::max<long long>(tip == Tip::Square ? lo : -rows, y0 - y),
                    dyHi = std::min<long long>(tip == Tip::Square ? hi : rows, y1 - y);
    for (long long dy = dyLo; dy <= dyHi; ++dy)
        for (long long dx = dxLo; dx <= dxHi; ++dx) {
            if (tip == Tip::Round) {
                const double fx = double(dx) + shiftX, fy = 2.0 * double(dy) + shiftY;
                if (fx * fx + fy * fy > r * r + 1e-9)
                    continue;
            }
            out.emplace_back(x + static_cast<int>(dx), y + static_cast<int>(dy));
        }
    return out;
}

} // namespace

std::vector<std::pair<int, int>> footprint(int x, int y, int size, Tip tip) {
    // clipped to int, so x + dx can't overflow: cells past the ends are dropped
    return cover(x, y, size, tip, INT_MIN, INT_MIN, INT_MAX, INT_MAX);
}

std::vector<std::pair<int, int>> footprint(int x, int y, int size, Tip tip, int width, int height) {
    return cover(x, y, size, tip, 0, 0, width - 1LL, height - 1LL);
}

namespace {

// The 4-connected line from x0,y0 to x1,y1, walked in steps along it: i
// columns and j rows from the start (dx and dy long, toward the end). Both
// grow by one at a time, so the line is a staircase: in each iteration the
// error term picks a step across, a step down, or both, the cell beside the
// corner first so the line stays 4-connected. After i steps across and j
// down the error term is dx(j + 1) - dy(i + 1) whatever the order, so the
// walk can start from any point it would pass at the top of the loop.
// 64-bit throughout: dx and dy are below 2^32, so their products with a
// step count fit unsigned.
struct Line {
    long long x0, y0, sx, sy;
    unsigned long long dx, dy;

    Line(long long ax, long long ay, long long bx, long long by)
        : x0(ax), y0(ay), sx(ax < bx ? 1 : -1), sy(ay < by ? 1 : -1),
          dx(static_cast<unsigned long long>(ax < bx ? bx - ax : ax - bx)),
          dy(static_cast<unsigned long long>(ay < by ? by - ay : ay - by)) {}

    // floor((2ab + c) / 2d) without overflow, for ab < 2^64 and small c.
    static unsigned long long halfDiv(unsigned long long a, unsigned long long b, unsigned long long c,
                                      unsigned long long d) {
        const unsigned long long ab = a * b;
        return ab / d + (2 * (ab % d) + c) / (2 * d);
    }

    // Where the loop starts an iteration. A line at least as wide as it is
    // tall steps across every iteration, so it starts one at each column i,
    // in row floor((2 dy i + dx) / 2dx); a taller one starts one at each row.
    unsigned long long rowAt(unsigned long long i) const { return dx ? halfDiv(dy, i, dx, dx) : 0; }
    unsigned long long colAt(unsigned long long j) const { return halfDiv(dx, j, dy, dy); }

    // Walks from the loop's start at i, j to the end, calling visit with
    // each cell's steps; visit returns false to stop.
    template <class Visit>
    void walkFrom(unsigned long long i, unsigned long long j, Visit&& visit) const {
        const long long w = static_cast<long long>(dx), h = static_cast<long long>(dy);
        // dx(j + 1) - dy(i + 1), small at the loop's start; the products
        // wrap but their difference doesn't
        long long err = w - h + static_cast<long long>(dx * j - dy * i);
        if (!visit(i, j))
            return;
        while (i != dx || j != dy) {
            const long long e2 = 2 * err;
            const bool stepX = e2 >= -h, stepY = e2 <= w;
            if (stepX && stepY) {
                err -= h;
                ++i;
                if (!visit(i, j))
                    return;
                err += w;
                ++j;
            } else if (stepX) {
                err -= h;
                ++i;
            } else {
                err += w;
                ++j;
            }
            if (!visit(i, j))
                return;
        }
    }

    long long x(unsigned long long i) const { return x0 + sx * static_cast<long long>(i); }
    long long y(unsigned long long j) const { return y0 + sy * static_cast<long long>(j); }

    // The steps within lo..hi of a start at a going s each step, or false.
    static bool span(long long a, long long s, unsigned long long n, long long lo, long long hi,
                     unsigned long long& first, unsigned long long& last) {
        // in steps from a: (lo - a) * s .. (hi - a) * s, ordered
        long long f = s > 0 ? lo - a : a - hi, l = s > 0 ? hi - a : a - lo;
        if (l < 0 || f > static_cast<long long>(n) || f > l)
            return false;
        first = static_cast<unsigned long long>(std::max(0LL, f));
        last = std::min(n, static_cast<unsigned long long>(l));
        return true;
    }
};

// Walks the cells from one point to another, 4-connected, calling visit on
// each. 64-bit throughout, so the ends of int don't overflow.
template <class Visit>
void walk4(long long x0, long long y0, long long x1, long long y1, Visit&& visit) {
    const Line l(x0, y0, x1, y1);
    l.walkFrom(0, 0, [&](unsigned long long i, unsigned long long j) {
        visit(l.x(i), l.y(j));
        return true;
    });
}

// The same line's cells inside the rectangle (corners inclusive), in order,
// and only those: the walk starts from the last point the loop would pass
// before the line enters the rectangle, found by arithmetic, and stops where
// it leaves. Coordinates within 2^32 of each other.
template <class Visit>
void walk4(long long x0, long long y0, long long x1, long long y1, long long cx0, long long cy0, long long cx1,
           long long cy1, Visit&& visit) {
    const Line l(x0, y0, x1, y1);
    unsigned long long ia, ib, ja, jb;
    if (cx0 > cx1 || cy0 > cy1 || !Line::span(x0, l.sx, l.dx, cx0, cx1, ia, ib) ||
        !Line::span(y0, l.sy, l.dy, cy0, cy1, ja, jb))
        return;
    // The first column (or row) of the loop's starts whose cells reach the
    // rectangle's first row (column), then the start before it.
    unsigned long long si, sj;
    if (l.dx >= l.dy) {
        // the first column i whose row reaches ja: 2 dy i + dx >= 2 dx ja
        const unsigned long long iy = ja == 0 ? 0 : Line::halfDiv(l.dx, ja - 1, l.dx + 2 * l.dy - 1, l.dy);
        si = std::max(ia, iy);
        si -= si > 0;
        sj = l.rowAt(si);
    } else {
        const unsigned long long jx = ia == 0 ? 0 : Line::halfDiv(l.dy, ia - 1, l.dy + 2 * l.dx - 1, l.dx);
        sj = std::max(ja, jx);
        sj -= sj > 0;
        si = l.colAt(sj);
    }
    l.walkFrom(si, sj, [&](unsigned long long i, unsigned long long j) {
        if (i > ib || j > jb)
            return false;
        if (i >= ia && j >= ja)
            visit(l.x(i), l.y(j));
        return true;
    });
}

} // namespace

std::vector<std::pair<int, int>> line4(int x0, int y0, int x1, int y1) {
    std::vector<std::pair<int, int>> out;
    walk4(x0, y0, x1, y1, [&](long long x, long long y) { out.emplace_back(int(x), int(y)); });
    return out;
}

std::vector<std::pair<int, int>> line4(int x0, int y0, int x1, int y1, long long cx0, long long cy0, long long cx1,
                                       long long cy1) {
    std::vector<std::pair<int, int>> out;
    cx0 = std::max<long long>(cx0, INT_MIN), cy0 = std::max<long long>(cy0, INT_MIN);
    cx1 = std::min<long long>(cx1, INT_MAX), cy1 = std::min<long long>(cy1, INT_MAX);
    walk4(x0, y0, x1, y1, cx0, cy0, cx1, cy1, [&](long long x, long long y) { out.emplace_back(int(x), int(y)); });
    return out;
}

void Brush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    const Canvas& c = stroke.canvas();
    for (const auto& [x, y] : footprint(d.x, d.y, size, stroke.settings().tip, c.width(), c.height())) {
        if (!stroke.paintable(x, y) || stroke.painted(x, y))
            continue;
        if (const auto c = paintCell(stroke, x, y, strength))
            stroke.set(x, y, *c);
    }
}

std::optional<Cell> Brush::paintCell(const Stroke&, int, int, double) const { return std::nullopt; }

Stroke::Stroke(const Canvas& canvas, const Brush& brush, BrushSettings settings, StrokeOptions options)
    : canvas_(canvas), brush_(brush), settings_(settings), options_(std::move(options)) {
    settings_.size = std::clamp(settings_.size, 1, kMaxBrushSize);
    settings_.strength = std::clamp(settings_.strength, 0.0, 1.0);
}

void Stroke::addDab(const Dab& d, bool join) {
    if (!last_ || !join || (last_->x == d.x && last_->y == d.y)) {
        // a new segment, or the pen pressed again where it is
        stamp(d);
    } else {
        // Only the part of the line within reach of the canvas: the cells of
        // the whole line that are, in order, so the stroke paints as if every
        // dab of it were stamped. A dab further off than the brush's half
        // width (plus one) can't touch the canvas.
        const long long m = settings_.size / 2 + 1;
        const Dab from = *last_;
        walk4(from.x, from.y, d.x, d.y, -m, -m, canvas_.width() - 1LL + m, canvas_.height() - 1LL + m,
              [&](long long x, long long y) {
                  if (x != from.x || y != from.y)
                      stamp(Dab{int(x), int(y), d.pressure});
              });
    }
    last_ = d;
}

void Stroke::stamp(const Dab& d) {
    ++stamps_;
    const double p = std::clamp(d.pressure, 0.0, 1.0);
    int size = settings_.size;
    double strength = settings_.strength;
    if (settings_.pressure == PressureTo::Size)
        size = std::clamp(static_cast<int>(std::lround(size * p)), 1, kMaxBrushSize);
    else if (settings_.pressure == PressureTo::Strength)
        strength *= p;
    brush_.dab(*this, d, size, strength);
}

std::optional<Cell> Stroke::cellAt(int x, int y) const {
    if (!canvas_.contains(x, y))
        return std::nullopt;
    const auto it = painted_.find({y, x});
    return it != painted_.end() ? it->second : canvas_.at(x, y);
}

std::optional<Cell> Stroke::originalAt(int x, int y) const {
    if (!canvas_.contains(x, y))
        return std::nullopt;
    return canvas_.at(x, y);
}

bool Stroke::paintable(int x, int y) const {
    if (!canvas_.contains(x, y))
        return false;
    if (options_.selection && !options_.selection->contains(x, y))
        return false;
    return !(options_.lockTransparent && canvas_.at(x, y).transparent);
}

bool Stroke::set(int x, int y, const Cell& c) {
    if (!paintable(x, y))
        return false;
    painted_[{y, x}] = c;
    return true;
}

Canvas Stroke::preview() const {
    Canvas out = canvas_;
    for (const auto& [yx, c] : painted_)
        out.at(yx.second, yx.first) = c;
    return out;
}

CellEdit Stroke::edit() const {
    CellEdit e;
    for (const auto& [yx, c] : painted_) {
        const Cell& before = canvas_.at(yx.second, yx.first);
        if (!(before == c))
            e.changes.push_back({yx.second, yx.first, before, c});
    }
    return e;
}

CellEdit paintStroke(const Canvas& canvas, const Brush& brush, const BrushSettings& settings,
                      const std::vector<Dab>& dabs, const StrokeOptions& options) {
    Stroke s(canvas, brush, settings, options);
    for (const Dab& d : dabs)
        s.addDab(d);
    return s.edit();
}

CellEdit paintRegion(const Canvas& canvas, const Brush& brush, const BrushSettings& settings,
                      const StrokeOptions& options) {
    BrushSettings one = settings;
    one.size = 1;
    one.pressure = PressureTo::Nothing;
    Stroke s(canvas, brush, one, options);
    for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x)
            if (s.paintable(x, y))
                s.addDab(Dab{x, y}, false);
    return s.edit();
}

} // namespace bw
