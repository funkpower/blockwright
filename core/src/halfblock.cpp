#include "blockwright/halfblock.h"

#include "blockwright/ansi.h"
#include "blockwright/ramp.h"

#include <algorithm>
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
    switch (shown.ch) {
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
    const Cell& c = canvas.at(x, y);
    if (const auto pair = halfBlocks(c, ice))
        return Color::indexed(halfOf(*pair, bottomHalf(py)));
    return background ? iceCell(c, ice).bg : c.fg;
}

PixelPainter::PixelPainter(const Canvas& canvas, std::uint8_t colour, StrokeOptions options, bool mirror)
    : canvas_(canvas), colour_(colour & 15), options_(std::move(options)), mirror_(mirror) {}

bool PixelPainter::paintable(int x, int y) const {
    if (!canvas_.contains(x, y))
        return false;
    if (options_.selection && !options_.selection->contains(x, y))
        return false;
    return !(options_.lockTransparent && canvas_.at(x, y).transparent);
}

bool PixelPainter::plot(int x, int py) {
    bool any = plotOne(x, py);
    if (mirror_ && x >= 0 && x < canvas_.width())
        any = plotOne(canvas_.width() - 1 - x, py) || any;
    return any;
}

bool PixelPainter::plotOne(int x, int py) {
    const int y = cellRow(py);
    if (!paintable(x, y))
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
            if (!(before == after))
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

HalfBlockStroke::HalfBlockStroke(const Canvas& canvas, std::uint8_t colour, StrokeOptions options, bool mirror)
    : painter_(canvas, colour, std::move(options), mirror) {}

void HalfBlockStroke::addPoint(int x, int py, bool join) {
    if (last_ && join && *last_ != std::pair{x, py}) {
        const Canvas& c = painter_.canvas();
        for (const auto& [lx, ly] : line4(last_->first, last_->second, x, py, 0, 0, c.width() - 1LL,
                                          2LL * c.height() - 1))
            painter_.plot(lx, ly);
    } else {
        painter_.plot(x, py);
    }
    last_ = std::pair{x, py};
}

std::vector<std::pair<int, int>> floodPixels(const Canvas& canvas, int x, int py, const StrokeOptions& options) {
    const PixelPainter area(canvas, 0, options);
    const int w = canvas.width(), h = 2 * canvas.height();
    // the colour at a pixel, if it is a paintable pixel
    auto colourAt = [&](int px, int ppy) -> std::optional<std::uint8_t> {
        if (px < 0 || ppy < 0 || px >= w || ppy >= h || !area.paintable(px, cellRow(ppy)))
            return std::nullopt;
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
