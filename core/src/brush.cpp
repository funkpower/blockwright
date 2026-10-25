#include "blockwright/brush.h"

#include "blockwright/layer.h"

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
// Mirrored, the tip's cell at dx lands at x - dx instead, so an even size
// reaches one column further right; the columns are still visited left to
// right. Flipped top to bottom, the cell at dy lands at y - dy, the rows
// still visited top to bottom.
std::vector<std::pair<int, int>> cover(int x, int y, int size, Tip tip, long long x0, long long y0, long long x1,
                                       long long y1, bool mirrored = false, bool flippedY = false) {
    size = std::clamp(size, 1, kMaxBrushSize);
    std::vector<std::pair<int, int>> out;
    const int lo = -(size / 2), hi = lo + size - 1;
    const long long dxLo = std::max<long long>(lo, mirrored ? x - x1 : x0 - x),
                    dxHi = std::min<long long>(hi, mirrored ? x - x0 : x1 - x);
    // Round: cell centres within a circle `size` columns across, rows two
    // columns tall. An odd size centres on the dab; an even one is centred
    // half a cell left of it, and a multiple of four (an even number of
    // rows) half a row up as well, so every size keeps its rows.
    const double r = size / 2.0, shiftX = size % 2 ? 0.0 : 0.5, shiftY = size % 4 == 0 ? 1.0 : 0.0;
    const int rows = static_cast<int>(r / 2.0) + 2;
    const long long dyLo = std::max<long long>(tip == Tip::Square ? lo : -rows, flippedY ? y - y1 : y0 - y),
                    dyHi = std::min<long long>(tip == Tip::Square ? hi : rows, flippedY ? y - y0 : y1 - y);
    for (long long j = dyLo; j <= dyHi; ++j)
        for (long long i = dxLo; i <= dxHi; ++i) {
            const long long dy = flippedY ? dyHi - (j - dyLo) : j;
            const long long dx = mirrored ? dxHi - (i - dxLo) : i;
            if (tip == Tip::Round) {
                const double fx = double(dx) + shiftX, fy = 2.0 * double(dy) + shiftY;
                if (fx * fx + fy * fy > r * r + 1e-9)
                    continue;
            }
            out.emplace_back(static_cast<int>(mirrored ? x - dx : x + dx),
                             static_cast<int>(flippedY ? y - dy : y + dy));
        }
    return out;
}

} // namespace

std::vector<std::pair<int, int>> footprint(int x, int y, int size, Tip tip) {
    // clipped to int, so x + dx can't overflow: cells past the ends are dropped
    return cover(x, y, size, tip, INT_MIN, INT_MIN, INT_MAX, INT_MAX);
}

std::vector<std::pair<int, int>> footprint(int x, int y, int size, Tip tip, int width, int height, bool mirrored,
                                           bool flippedY) {
    return cover(x, y, size, tip, 0, 0, width - 1LL, height - 1LL, mirrored, flippedY);
}

std::pair<double, double> compassVector(Compass c) {
    constexpr double d = 0.70710678118654752440;
    switch (c) {
    case Compass::N: return {0, -1};
    case Compass::NE: return {d, -d};
    case Compass::E: return {1, 0};
    case Compass::SE: return {d, d};
    case Compass::S: return {0, 1};
    case Compass::SW: return {-d, d};
    case Compass::W: return {-1, 0};
    case Compass::NW: return {-d, -d};
    }
    return {0, -1};
}

namespace {

// A point's images. Coordinates are doubled so a centre between cells is
// whole: c2x, c2y are twice the centre, `aspect` how many columns tall a row
// is on screen. Mirrors and half turns are exact reflections; other turns
// are made on screen and rounded to the nearest point, halves down and
// right (sines and cosines within 1e-12 of 0 or ±1 taken as exact, so a
// quarter turn is exact where the grid allows).
struct Image {
    long long x, y;
    bool flipX, flipY;
    int turns = 0;  // quarter turns clockwise on screen
};

std::vector<Image> images(const Symmetry& s, long long x, long long y, long long c2x, long long c2y,
                          long long mx2, long long my2, double aspect) {
    std::vector<Image> out{{x, y, false, false}};
    switch (s.mode) {
    case SymmetryMode::None: break;
    case SymmetryMode::MirrorX: out.push_back({mx2 - x, y, true, false}); break;
    case SymmetryMode::MirrorY: out.push_back({x, my2 - y, false, true}); break;
    case SymmetryMode::MirrorXY:
        out.push_back({mx2 - x, y, true, false});
        out.push_back({x, my2 - y, false, true});
        out.push_back({mx2 - x, my2 - y, true, true});
        break;
    case SymmetryMode::Radial: {
        const int n = std::clamp(s.folds, kMinFolds, kMaxFolds);
        const auto snap = [](double v) {
            for (const double e : {0.0, 1.0, -1.0})
                if (std::abs(v - e) < 1e-12)
                    return e;
            return v;
        };
        // offsets from the centre on screen, doubled
        const double u = double(2 * x - c2x), v = double(2 * y - c2y) * aspect;
        for (int k = 1; k < n; ++k) {
            if (2 * k == n) {
                out.push_back({c2x - x, c2y - y, true, true});
                continue;
            }
            const double a = 2.0 * 3.14159265358979323846 * k / n, cs = snap(std::cos(a)), sn = snap(std::sin(a));
            const double ru = u * cs - v * sn, rv = (u * sn + v * cs) / aspect;
            // y runs down, so a positive angle turns clockwise on screen
            const int turns = 4 * k % n == 0 ? 4 * k / n : 0;
            out.push_back({static_cast<long long>(std::floor((double(c2x) + ru) / 2.0 + 0.5)),
                           static_cast<long long>(std::floor((double(c2y) + rv) / 2.0 + 0.5)), false, false, turns});
        }
        break;
    }
    }
    return out;
}

bool fitsInt(long long v) { return v >= INT_MIN && v <= INT_MAX; }

// A dab's images on a width × height canvas, in 64 bits.
std::vector<Image> dabImages(const Symmetry& s, const Dab& d, int width, int height) {
    const long long c2x = s.centre ? 2LL * s.centre->first : width - 1LL,
                    c2y = s.centre ? 2LL * s.centre->second : height - 1LL;
    return images(s, d.x, d.y, c2x, c2y, width - 1LL, height - 1LL, 2.0);
}

// The image's dab, but for its position.
Dab imageDab(const Dab& d, const Image& im, int image) {
    return Dab{0, 0, d.pressure, image, d.flipX != im.flipX, d.flipY != im.flipY, (d.turns + im.turns) % 4};
}

} // namespace

std::vector<Dab> symmetryDabs(const Symmetry& s, const Dab& d, int width, int height) {
    std::vector<Dab> out;
    const std::vector<Image> ims = dabImages(s, d, width, height);
    for (size_t i = 0; i < ims.size(); ++i) {
        if (!fitsInt(ims[i].x) || !fitsInt(ims[i].y))
            continue;
        Dab dab = imageDab(d, ims[i], int(i));
        dab.x = int(ims[i].x);
        dab.y = int(ims[i].y);
        out.push_back(dab);
    }
    return out;
}

std::vector<std::pair<long long, long long>> symmetryPixels(const Symmetry& s, int x, int py, int width, int height) {
    // a set centre is its cell's centre: pixel row 2y + 0.5, doubled 4y + 1
    const long long c2x = s.centre ? 2LL * s.centre->first : width - 1LL,
                    c2y = s.centre ? 4LL * s.centre->second + 1 : 2LL * height - 1;
    std::vector<std::pair<long long, long long>> out;
    for (const Image& im : images(s, x, py, c2x, c2y, width - 1LL, 2LL * height - 1, 1.0))
        out.emplace_back(im.x, im.y);
    return out;
}

namespace {

// The 4-connected line from x0,y0 to x1,y1, walked in steps along it: i
// columns and j rows from the start (dx and dy long, toward the end). Both
// grow by one at a time, so the line is a staircase: in each iteration the
// error term picks a step across, a step down, or both, the cell beside the
// corner first so the line stays 4-connected. After i steps across and j
// down the error term is dx(j + 1) - dy(i + 1) whatever the order, so the
// walk can start from any point it would pass at the top of the loop.
// 64-bit throughout: dx and dy are below 2^62; their products with a step
// count are divided in 128 bits (halfDiv), and the error term is their
// difference, which stays small however the products wrap.
struct Line {
    long long x0, y0, sx, sy;
    unsigned long long dx, dy;

    Line(long long ax, long long ay, long long bx, long long by)
        : x0(ax), y0(ay), sx(ax < bx ? 1 : -1), sy(ay < by ? 1 : -1),
          dx(static_cast<unsigned long long>(ax < bx ? bx - ax : ax - bx)),
          dy(static_cast<unsigned long long>(ay < by ? by - ay : ay - by)) {}

    // floor((2ab + c) / 2d) without overflow, for ab / d < 2^64, d < 2^62
    // and small c: ab is taken in 128 bits (two 64-bit halves, as there is
    // no portable 128-bit integer), then divided a bit at a time.
    static unsigned long long halfDiv(unsigned long long a, unsigned long long b, unsigned long long c,
                                      unsigned long long d) {
        constexpr unsigned long long lo32 = 0xffffffffULL;
        const unsigned long long al = a & lo32, ah = a >> 32, bl = b & lo32, bh = b >> 32;
        const unsigned long long ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
        const unsigned long long mid = (ll >> 32) + (lh & lo32) + (hl & lo32);
        const unsigned long long lo = (ll & lo32) | (mid << 32), hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
        unsigned long long q = 0, r = hi % d;
        for (int bit = 63; bit >= 0; --bit) {
            r = (r << 1) | ((lo >> bit) & 1);
            q <<= 1;
            if (r >= d) {
                r -= d;
                q |= 1;
            }
        }
        return q + (2 * r + c) / (2 * d);
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
// it leaves. Coordinates within 2^61 of each other.
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

std::vector<std::pair<int, int>> line4(long long x0, long long y0, long long x1, long long y1, long long cx0,
                                       long long cy0, long long cx1, long long cy1) {
    std::vector<std::pair<int, int>> out;
    cx0 = std::max<long long>(cx0, INT_MIN), cy0 = std::max<long long>(cy0, INT_MIN);
    cx1 = std::min<long long>(cx1, INT_MAX), cy1 = std::min<long long>(cy1, INT_MAX);
    walk4(x0, y0, x1, y1, cx0, cy0, cx1, cy1, [&](long long x, long long y) { out.emplace_back(int(x), int(y)); });
    return out;
}

void Brush::dab(Stroke& stroke, const Dab& d, int size, double strength) const {
    const Canvas& c = stroke.canvas();
    for (const auto& [x, y] : footprint(d.x, d.y, size, stroke.settings().tip, c.width(), c.height(),
                                        d.flipX, d.flipY)) {
        if (!stroke.paintable(x, y))
            continue;
        // painted already at this strength or more: one pass is one step;
        // on a tie the lower symmetry image (the stroke itself) keeps it
        if (const auto done = stroke.paintedStrength(x, y);
            done && (*done > strength || (*done == strength && *stroke.paintedImage(x, y) <= d.image)))
            continue;
        if (const auto c = paintCell(stroke, x, y, strength))
            stroke.set(x, y, *c, strength);
    }
}

std::optional<Cell> Brush::paintCell(const Stroke&, int, int, double) const { return std::nullopt; }

Stroke::Stroke(const Canvas& canvas, const Brush& brush, BrushSettings settings, StrokeOptions options)
    : canvas_(canvas), brush_(brush), settings_(settings), options_(std::move(options)) {
    settings_.size = std::clamp(settings_.size, 1, kMaxBrushSize);
    settings_.strength = std::clamp(settings_.strength, 0.0, 1.0);
}

void Stroke::addDab(const Dab& d, bool join) {
    const std::vector<Image> ims = dabImages(options_.symmetry, d, canvas_.width(), canvas_.height());
    lasts_.resize(ims.size());
    // Each image's stamps for this dab (its joining line's, or the one),
    // stamped in turn: point k of every image, then point k + 1, so a brush
    // that judges a dab's images together sees each point's images at once.
    // A joining line is walked from its lesser end (by row, then column).
    // A mirror or the half turn walks its image from the image of the
    // stroke's own lesser end instead of its own: Line takes the same steps
    // whichever way its signs run, so the image's cells are the exact
    // reflections of the stroke's (#125). A turn that is rounded walks its
    // own line from its own lesser end.
    const auto fromLast = [](const std::optional<std::pair<long long, long long>>& last, long long x, long long y) {
        return last && std::pair{last->second, last->first} < std::pair{y, x};
    };
    const bool ownFromLast = fromLast(lasts_[0], ims[0].x, ims[0].y);
    std::vector<std::vector<Dab>> stamps(ims.size());
    size_t most = 0;
    for (size_t i = 0; i < ims.size(); ++i) {
        const bool reflection = ims[i].flipX || ims[i].flipY;
        addImage(ims[i].x, ims[i].y, imageDab(d, ims[i], int(i)), join,
                 i == 0 || reflection ? ownFromLast : fromLast(lasts_[i], ims[i].x, ims[i].y), lasts_[i], stamps[i]);
        most = std::max(most, stamps[i].size());
    }
    for (size_t k = 0; k < most; ++k)
        for (const auto& image : stamps)
            if (k < image.size())
                stamp(image[k]);
}

void Stroke::addImage(long long x, long long y, const Dab& d, bool join, bool fromLast,
                      std::optional<std::pair<long long, long long>>& last, std::vector<Dab>& out) {
    const auto at = [&](long long cx, long long cy) {
        Dab out = d;
        out.x = int(cx);
        out.y = int(cy);
        return out;
    };
    if (!last || !join || *last == std::pair{x, y}) {
        // a new segment, or the pen pressed again where it is; an image
        // beyond the ends of int can't reach the canvas
        if (fitsInt(x) && fitsInt(y))
            out.push_back(at(x, y));
    } else {
        // Only the part of the line within reach of the canvas: the cells of
        // the whole line that are, in order, so the stroke paints as if every
        // dab of it were stamped. A dab further off than the brush's half
        // width (plus one) can't touch the canvas. The ends stay in 64 bits,
        // so an image beyond int keeps the line's slope.
        // The line is walked from the end addDab picks (the lesser one, by
        // row then column, of the stroke's own line) and stamped from the
        // last point, so a line back over the same two points covers the
        // same cells: a scrub stamps no new ones.
        const long long m = settings_.size / 2 + 1;
        const auto [fx, fy] = *last;
        if (fromLast) {
            walk4(fx, fy, x, y, -m, -m, canvas_.width() - 1LL + m, canvas_.height() - 1LL + m,
                  [&](long long cx, long long cy) {
                      if (cx != fx || cy != fy)
                          out.push_back(at(cx, cy));
                  });
        } else {
            std::vector<std::pair<long long, long long>> cells;
            walk4(x, y, fx, fy, -m, -m, canvas_.width() - 1LL + m, canvas_.height() - 1LL + m,
                  [&](long long cx, long long cy) {
                      if (cx != fx || cy != fy)
                          cells.emplace_back(cx, cy);
                  });
            for (auto it = cells.rbegin(); it != cells.rend(); ++it)
                out.push_back(at(it->first, it->second));
        }
    }
    last = std::pair{x, y};
}

void Stroke::stamp(const Dab& d) {
    ++stamps_;
    current_ = d;
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
    return it != painted_.end() ? it->second.cell : canvas_.at(x, y);
}

std::optional<double> Stroke::paintedStrength(int x, int y) const {
    const auto it = painted_.find({y, x});
    return it != painted_.end() ? std::optional<double>(it->second.strength) : std::nullopt;
}

std::optional<int> Stroke::paintedImage(int x, int y) const {
    const auto it = painted_.find({y, x});
    return it != painted_.end() ? std::optional<int>(it->second.image) : std::nullopt;
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

std::vector<std::pair<int, int>> Stroke::paintedCells() const {
    std::vector<std::pair<int, int>> out;
    out.reserve(painted_.size());
    for (const auto& [yx, p] : painted_)
        out.emplace_back(yx.second, yx.first);
    return out;
}

bool Stroke::set(int x, int y, const Cell& c, double strength, int image) {
    if (!paintable(x, y))
        return false;
    // The alpha lock freezes what is opaque: a write may not clear an opaque
    // cell (the glyph brush's erase) or, on a pixel layer, change which
    // pixels are set.
    if (options_.lockTransparent) {
        const Cell& before = canvas_.at(x, y);
        if (options_.pixelLayer) {
            const LayerPixels was = layerPixels(before), now = layerPixels(c);
            if (bool(was.top) != bool(now.top) || bool(was.bottom) != bool(now.bottom))
                return false;
        } else if (c.transparent && !before.transparent) {
            return false;
        }
    }
    painted_[{y, x}] = Painted{c, strength, image < 0 ? current_.image : image};
    return true;
}

Canvas Stroke::preview() const {
    Canvas out = canvas_;
    for (const auto& [yx, p] : painted_)
        out.at(yx.second, yx.first) = p.cell;
    return out;
}

CellEdit Stroke::edit() const {
    CellEdit e;
    for (const auto& [yx, p] : painted_) {
        const Cell& before = canvas_.at(yx.second, yx.first);
        if (!identical(before, p.cell))
            e.changes.push_back({yx.second, yx.first, before, p.cell});
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
