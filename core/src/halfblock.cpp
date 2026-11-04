#include "blockwright/halfblock.h"

#include "blockwright/ansi.h"
#include "blockwright/layer.h"
#include "blockwright/ramp.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <deque>

namespace bw {

namespace {

// The cell row of a pixel row and whether it is the bottom half. An
// arithmetic shift (C++20), so a negative row floors.
int cellRow(int py) { return py >> 1; }
bool bottomHalf(int py) { return (py & 1) != 0; }

std::uint8_t halfOf(HalfPair p, bool bottom) { return bottom ? p.bottom : p.top; }

// The cell written for `pair` over `from`: the attributes the colours don't
// use are kept (iceCell drops Blink where it is the bright background), and
// the cell is opaque, since it was painted.
Cell written(const Cell& from, HalfPair pair, bool ice) {
    Cell c = halfBlockCell(pair, ice);
    c.attrs = iceCell(from, ice).attrs;
    return c;
}

} // namespace

std::optional<HalfPair> halfBlocks(const Cell& cell, bool ice) {
    const Cell shown = iceCell(cell, ice);
    const std::uint8_t fg = shown.fg.paletteIndex(), bg = shown.bg.paletteIndex();
    switch (shown.ch.codePointOr(kNoCodePoint)) {
    case U' ':
    case U'\0':
    case U'\u00a0': return HalfPair{bg, bg};
    case kFullBlock: return HalfPair{fg, fg};
    case kUpperHalf: return HalfPair{fg, bg};
    case kLowerHalf: return HalfPair{bg, fg};
    default: return std::nullopt;
    }
}

Cell halfBlockCell(HalfPair p, bool ice) {
    // iCE off can't show two bright colours in one cell: the bottom goes dark
    if (!ice && p.top >= 8 && p.bottom >= 8 && p.top != p.bottom)
        p.bottom &= 7;
    Cell c;
    if (p.top == p.bottom) {
        if (p.top < 8) {
            c.bg = Color::indexed(p.top);
        } else {
            c.ch = kFullBlock;
            c.fg = Color::indexed(p.top);
        }
    } else if (p.bottom < 8 || p.top >= 8) {
        c.ch = kUpperHalf;
        c.fg = Color::indexed(p.top);
        c.bg = Color::indexed(p.bottom);
    } else {
        c.ch = kLowerHalf;
        c.fg = Color::indexed(p.bottom);
        c.bg = Color::indexed(p.top);
    }
    return c;
}

Cell setHalfBlock(const Cell& cell, bool bottom, std::uint8_t colour, bool ice) {
    colour &= 15;
    auto pair = halfBlocks(cell, ice);
    if (pair && halfOf(*pair, bottom) == colour && !cell.transparent)
        return cell;
    if (!pair) {
        const std::uint8_t bg = iceCell(cell, ice).bg.paletteIndex();
        pair = HalfPair{bg, bg};
    }
    // With iCE off the half not painted keeps the colour it shows: a second
    // bright colour can only be the dark background.
    const std::uint8_t other = halfOf(*pair, !bottom);
    if (!ice && colour >= 8 && other >= 8 && other != colour)
        colour &= 7;
    (bottom ? pair->bottom : pair->top) = colour;
    return written(cell, *pair, ice);
}

Cell setHalfBlocks(const Cell& cell, std::uint8_t colour, bool ice) {
    colour &= 15;
    if (halfBlocks(cell, ice) == HalfPair{colour, colour} && !cell.transparent)
        return cell;
    return written(cell, HalfPair{colour, colour}, ice);
}

std::optional<Color> pickColour(const Canvas& canvas, int x, int py, bool background, bool ice) {
    const int y = cellRow(py);
    if (!canvas.contains(x, y))
        return std::nullopt;
    // a continuation shows its pair's colours, which are its lead's
    const bool cont = canvas.at(x, y).ch.isContinuation() && x > 0 && !canvas.at(x - 1, y).ch.isContinuation();
    const Cell& c = canvas.at(cont ? x - 1 : x, y);
    if (const auto pair = halfBlocks(c, ice))
        return Color::indexed(halfOf(*pair, bottomHalf(py)));
    return background ? iceCell(c, ice).bg : c.fg;
}

namespace {

Symmetry symmetryOf(const StrokeOptions& options, bool mirror) {
    if (options.symmetry.mode == SymmetryMode::None && mirror)
        return Symmetry{SymmetryMode::MirrorX, 4, std::nullopt};
    return options.symmetry;
}

} // namespace

PixelPainter::PixelPainter(const Canvas& canvas, std::uint8_t colour, StrokeOptions options, bool mirror)
    : canvas_(canvas), colour_(colour & 15), options_(std::move(options)), symmetry_(symmetryOf(options_, mirror)) {}

bool PixelPainter::paintable(int x, int y) const {
    if (!canvas_.contains(x, y))
        return false;
    if (options_.selection && !options_.selection->contains(x, y))
        return false;
    if (!options_.lockTransparent)
        return true;
    if (options_.pixelLayer) {
        const LayerPixels p = layerPixels(canvas_.at(x, y));
        return p.top || p.bottom;
    }
    return !canvas_.at(x, y).transparent;
}

bool PixelPainter::pixelPaintable(int x, int py) const {
    const int y = cellRow(py);
    if (!paintable(x, y))
        return false;
    if (!options_.pixelLayer || !options_.lockTransparent)
        return true;
    const LayerPixels p = layerPixels(canvas_.at(x, y));
    return bool(bottomHalf(py) ? p.bottom : p.top);
}

bool PixelPainter::plot(int x, int py) {
    // a pixel off the canvas has images too: each is clipped on its own
    if (symmetry_.mode == SymmetryMode::None)
        return plotOne(x, py);
    bool any = false;
    for (const auto& [ix, ipy] : symmetryPixels(symmetry_, x, py, canvas_.width(), canvas_.height()))
        if (ix >= 0 && ix < canvas_.width() && ipy >= 0 && ipy < 2LL * canvas_.height())
            any = plotOne(int(ix), int(ipy)) || any;
    return any;
}

bool PixelPainter::plotOne(int x, int py) {
    const int y = cellRow(py);
    if (!pixelPaintable(x, py))
        return false;
    cover(x, y);
    halves_[static_cast<size_t>(y - wy_) * ww_ + (x - wx_)] |= bottomHalf(py) ? 2 : 1;
    return true;
}

void PixelPainter::cover(int x, int y) {
    const bool first = halves_.empty();
    bx0_ = first ? x : std::min(bx0_, x), bx1_ = first ? x : std::max(bx1_, x);
    by0_ = first ? y : std::min(by0_, y), by1_ = first ? y : std::max(by1_, y);
    if (!first && x >= wx_ && x < wx_ + ww_ && y >= wy_ && y < wy_ + wh_)
        return;
    // The plotted cells' bounding box with half its span spare on each side,
    // clamped to the canvas: at most twice the span each way. The box only
    // grows, so the new window holds the old one; and a cell outside it adds
    // more than half the span on its axis, so a shape plotted in any order
    // reallocates a logarithmic number of times. In 64 bits, as the sums
    // pass INT_MAX on a canvas over about 1.07e9 cells wide (no test: that
    // canvas is too big to allocate).
    const auto window = [](int b0, int b1, int size, int& lo, int& hi) {
        const long long margin = (static_cast<long long>(b1) - b0 + 1) / 2;
        lo = static_cast<int>(std::max(0LL, b0 - margin));
        hi = static_cast<int>(std::min(size - 1LL, b1 + margin));
    };
    int x0, x1, y0, y1;
    window(bx0_, bx1_, canvas_.width(), x0, x1);
    window(by0_, by1_, canvas_.height(), y0, y1);
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<std::uint8_t> grown(static_cast<size_t>(w) * h);
    for (int r = 0; r < wh_; ++r)
        std::copy_n(halves_.begin() + static_cast<std::ptrdiff_t>(r) * ww_, ww_,
                    grown.begin() + static_cast<std::ptrdiff_t>(wy_ + r - y0) * w + (wx_ - x0));
    halves_ = std::move(grown);
    wx_ = x0, wy_ = y0, ww_ = w, wh_ = h;
}

Cell PixelPainter::painted(int x, int y, std::uint8_t halves) const {
    const Cell& c = canvas_.at(x, y);
    if (options_.pixelLayer) {
        Cell out = c;
        if (halves & 1)
            out = setLayerPixel(out, false, colour_);
        if (halves & 2)
            out = setLayerPixel(out, true, colour_);
        return out;
    }
    if (halves == 3)
        return setHalfBlocks(c, colour_, options_.ice);
    return setHalfBlock(c, halves == 2, colour_, options_.ice);
}

std::optional<Cell> PixelPainter::cellAt(int x, int y) const {
    if (!canvas_.contains(x, y))
        return std::nullopt;
    const bool in = x >= wx_ && x < wx_ + ww_ && y >= wy_ && y < wy_ + wh_;
    const std::uint8_t h = in ? halves_[static_cast<size_t>(y - wy_) * ww_ + (x - wx_)] : 0;
    return h ? painted(x, y, h) : canvas_.at(x, y);
}

Canvas PixelPainter::preview() const {
    Canvas out = canvas_;
    for (const CellChange& c : edit().changes)
        out.at(c.x, c.y) = c.after;
    return out;
}

CellEdit PixelPainter::edit() const {
    CellEdit e;
    for (int y = wy_; y < wy_ + wh_; ++y) {
        const std::uint8_t* row = halves_.data() + static_cast<size_t>(y - wy_) * ww_;
        for (int x = wx_; x < wx_ + ww_; ++x) {
            const std::uint8_t h = row[x - wx_];
            if (!h)
                continue;
            const Cell& before = canvas_.at(x, y);
            const Cell after = painted(x, y, h);
            if (!identical(before, after))
                e.changes.push_back({x, y, before, after});
        }
    }
    return e;
}

CellEdit plotPixels(const Canvas& canvas, const std::vector<std::pair<int, int>>& pixels, std::uint8_t colour,
                    const StrokeOptions& options, bool mirror) {
    PixelPainter p(canvas, colour, options, mirror);
    for (const auto& [x, py] : pixels)
        p.plot(x, py);
    return p.edit();
}

std::vector<std::pair<int, int>> line8(long long x0, long long y0, long long x1, long long y1, long long cx0,
                                       long long cy0, long long cx1, long long cy1) {
    std::vector<std::pair<int, int>> out;
    // only cells within int come out
    cx0 = std::max<long long>(cx0, INT_MIN), cy0 = std::max<long long>(cy0, INT_MIN);
    cx1 = std::min<long long>(cx1, INT_MAX), cy1 = std::min<long long>(cy1, INT_MAX);
    const long long dx = x1 - x0, dy = y1 - y0;
    const long long n = std::max(std::llabs(dx), std::llabs(dy));
    if (n == 0) {
        if (x0 >= cx0 && x0 <= cx1 && y0 >= cy0 && y0 <= cy1)
            out.emplace_back(static_cast<int>(x0), static_cast<int>(y0));
        return out;
    }
    // the point at step i: the minor axis is d * i / n rounded, halves
    // away from the start. The ends are images of int points, within 2^34
    // of the canvas, so |d| and i are below 2^35 and d * i can pass 2^63:
    // divided in two parts of i, its bits above 16 then the low 16.
    const auto along = [n](long long a0, long long d, long long i) {
        const long long m = std::llabs(d), hi = m * (i >> 16), lo = m * (i & 0xffff);
        const long long rest = (hi % n) * 65536 + lo;
        const long long q = (hi / n) * 65536 + rest / n, r = q + (2 * (rest % n) >= n ? 1 : 0);
        return a0 + (d < 0 ? -r : r);
    };
    // the steps whose major coordinate is in the box
    const bool xMajor = std::llabs(dx) >= std::llabs(dy);
    const long long a0 = xMajor ? x0 : y0, da = xMajor ? dx : dy;
    const long long lo = xMajor ? cx0 : cy0, hi = xMajor ? cx1 : cy1;
    long long i0 = 0, i1 = n;
    if (da > 0)
        i0 = std::max(i0, lo - a0), i1 = std::min(i1, hi - a0);
    else
        i0 = std::max(i0, a0 - hi), i1 = std::min(i1, a0 - lo);
    for (long long i = i0; i <= i1; ++i) {
        const long long x = along(x0, dx, i), y = along(y0, dy, i);
        if (x >= cx0 && x <= cx1 && y >= cy0 && y <= cy1)
            out.emplace_back(static_cast<int>(x), static_cast<int>(y));
    }
    return out;
}

HalfBlockStroke::HalfBlockStroke(const Canvas& canvas, std::uint8_t colour, StrokeOptions options, bool mirror,
                                 Connectivity connectivity)
    : painter_(canvas, colour, options), symmetry_(symmetryOf(options, mirror)), connectivity_(connectivity) {}

void HalfBlockStroke::addPoint(int x, int py, bool join) {
    const Canvas& c = painter_.canvas();
    if (symmetry_.mode == SymmetryMode::None) {
        lasts_.resize(1);
        addImage(x, py, join, lasts_[0]);
        return;
    }
    const auto images = symmetryPixels(symmetry_, x, py, c.width(), c.height());
    lasts_.resize(images.size());
    for (size_t i = 0; i < images.size(); ++i)
        addImage(images[i].first, images[i].second, join, lasts_[i]);
}

void HalfBlockStroke::addImage(long long x, long long py, bool join,
                               std::optional<std::pair<long long, long long>>& last) {
    if (last && join && *last != std::pair{x, py}) {
        const Canvas& c = painter_.canvas();
        const long long cx1 = c.width() - 1LL, cy1 = 2LL * c.height() - 1;
        for (const auto& [lx, ly] : connectivity_ == Connectivity::Eight
                                        ? line8(last->first, last->second, x, py, 0, 0, cx1, cy1)
                                        : line4(last->first, last->second, x, py, 0, 0, cx1, cy1))
            painter_.plotOne(lx, ly);
    } else if (x >= INT_MIN && x <= INT_MAX && py >= INT_MIN && py <= INT_MAX) {
        // an image beyond int can't be on the canvas
        painter_.plotOne(static_cast<int>(x), static_cast<int>(py));
    }
    last = std::pair{x, py};
}

std::vector<std::pair<int, int>> floodPixels(const Canvas& canvas, int x, int py, const StrokeOptions& options) {
    const PixelPainter area(canvas, 0, options);
    const int w = canvas.width(), h = 2 * canvas.height();
    // the colour at a pixel, if it is a paintable pixel; on a pixel layer a
    // transparent pixel is -1, a colour of its own
    auto colourAt = [&](int px, int ppy) -> std::optional<int> {
        if (px < 0 || ppy < 0 || px >= w || ppy >= h || !area.pixelPaintable(px, ppy))
            return std::nullopt;
        if (options.pixelLayer) {
            const LayerPixels p = layerPixels(canvas.at(px, cellRow(ppy)));
            const auto& half = bottomHalf(ppy) ? p.bottom : p.top;
            return half ? int(*half) : -1;
        }
        const auto pair = halfBlocks(canvas.at(px, cellRow(ppy)), options.ice);
        if (!pair)
            return std::nullopt;
        return halfOf(*pair, bottomHalf(ppy));
    };
    std::vector<std::pair<int, int>> out;
    const auto target = colourAt(x, py);
    if (!target)
        return out;
    std::vector<bool> seen(static_cast<size_t>(w) * h);
    std::deque<std::pair<int, int>> todo{{x, py}};
    seen[static_cast<size_t>(py) * w + x] = true;
    while (!todo.empty()) {
        const auto [px, ppy] = todo.front();
        todo.pop_front();
        out.emplace_back(px, ppy);
        for (const auto& [nx, ny] : {std::pair{px - 1, ppy}, {px + 1, ppy}, {px, ppy - 1}, {px, ppy + 1}}) {
            if (colourAt(nx, ny) != target || seen[static_cast<size_t>(ny) * w + nx])
                continue;
            seen[static_cast<size_t>(ny) * w + nx] = true;
            todo.emplace_back(nx, ny);
        }
    }
    return out;
}

CellEdit fillPixels(const Canvas& canvas, int x, int py, std::uint8_t colour, const StrokeOptions& options,
                    bool mirror) {
    return plotPixels(canvas, floodPixels(canvas, x, py, options), colour, options, mirror);
}

} // namespace bw
