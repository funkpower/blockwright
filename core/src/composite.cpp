#include "blockwright/composite.h"

#include "blockwright/ansi.h"
#include "blockwright/ramp.h"
#include "blockwright/style_draw.h"

#include <algorithm>
#include <deque>

#include <utility>
#include <vector>

namespace bw {

namespace {

// Perceived brightness of a palette colour, for ▒'s darker half.
int luma(std::uint8_t i) {
    const Rgb& c = kPalette[i & 15];
    return 299 * c.r + 587 * c.g + 114 * c.b;
}

bool composites(const Layer& l) {
    return l.props.visible && l.props.kind != LayerKind::Reference;
}

// Lays one layer cell over the cell composited so far (`acc`, `text` whether
// it is text).
void over(Cell& acc, bool& text, const LayerProperties& props, const Cell& lc, bool ice) {
    if (props.kind != LayerKind::Pixels) {
        if (lc.transparent)
            return;
        const bool isText = props.kind == LayerKind::Text;
        switch (props.mode) {
        case LayerMode::Normal:
            acc = lc;
            text = isText;
            break;
        case LayerMode::CharsOnly:
            // the glyph is this layer's now, and so is whether it is text
            acc.ch = lc.ch;
            text = isText;
            break;
        case LayerMode::ColoursOnly:
            acc.fg = lc.fg;
            acc.bg = lc.bg;
            acc.attrs = lc.attrs;
            break;
        }
        acc.transparent = false;
        return;
    }
    // A pixel cell may be transparent and still hold one half (layer.h).
    if (text)
        return;
    const LayerPixels p = layerPixels(lc);
    if (p.top && p.bottom) {
        acc = halfBlockCell(HalfPair{*p.top, *p.bottom}, ice);
        return;
    }
    if (!p.top && !p.bottom)
        return;
    HalfPair pair = halvesUnder(acc, ice);
    const bool bottom = !p.top;
    const std::uint8_t colour = bottom ? *p.bottom : *p.top;
    std::uint8_t& other = bottom ? pair.top : pair.bottom;
    const Cell shown = iceCell(acc, ice);
    // With iCE off the foreground blinks: the half under was blinking if it
    // showed the foreground, and the blink showed if that differs from the
    // background.
    const std::uint8_t shownFg = shown.fg.paletteIndex();
    const bool blinking = (shown.attrs & attr::Blink) && other == shownFg && shownFg != shown.bg.paletteIndex();
    const std::uint8_t under = other;
    // iCE off fits one bright colour, and the pixel, on top, keeps its own:
    // the half under shows dark. Under a shade (or any cell not pixels) that
    // is its background, which iCE off always shows dark; under a pixel
    // cell, the half's dark counterpart.
    if (!ice && colour >= 8 && other >= 8 && colour != other) {
        if (halfBlocks(acc, ice))
            other &= 7;
        else
            other = shown.bg.paletteIndex() & 7;
    }
    (bottom ? pair.bottom : pair.top) = colour;
    acc = halfBlockCell(pair, ice);
    // The attributes the colours don't use stay (Blink with iCE off), but the
    // blink only on a blinking half under, kept as it was: the pixel has no
    // blink of its own. That half stays the foreground where the pixel's
    // colour can be the background (dark, with iCE off).
    acc.attrs = shown.attrs & std::uint8_t(~attr::Blink);
    const char32_t underFg = bottom ? kUpperHalf : kLowerHalf;
    if (blinking && other == under && colour != under && (ice || colour < 8)) {
        if (acc.ch != underFg) {
            acc.ch = underFg;
            std::swap(acc.fg, acc.bg);
        }
        acc.attrs |= attr::Blink;
    }
}

// How a layer whose style the compositor draws composites: its styled cells
// as a cells layer's, a pixel layer's in the normal mode, which it reads
// whatever its own (§5).
LayerProperties styledProps(const LayerProperties& props) {
    LayerProperties p = props;
    if (p.kind == LayerKind::Pixels)
        p.mode = LayerMode::Normal;
    p.kind = LayerKind::Cells;
    return p;
}

// One styled cell over the cell composited so far: a pixel layer's styled
// cells, like its pixels, never cover a text cell.
void overStyled(Cell& acc, bool& text, const LayerProperties& props, const Cell& lc, bool ice) {
    if (props.kind == LayerKind::Pixels && text)
        return;
    over(acc, text, styledProps(props), lc, ice);
}

// A styled layer's cells for compositeCellBelow, which composites a cell at
// a time: the layers last styled on this thread, one entry a layer id and
// iCE setting, each kept with the properties and cell revision it was drawn
// from, so a cell at a time over several overlapping styled layers styles
// each of them once and checks each entry in constant time.
const Canvas& cachedStyledCells(const Layer& layer, bool ice) {
    struct Entry {
        LayerProperties props;
        std::uint64_t revision = 0;
        bool ice = true;
        Canvas cells{0, 0};
    };
    constexpr std::size_t kEntries = 8;
    // deque: a push or a pop at either end leaves the other entries in place
    thread_local std::deque<Entry> cache;
    const auto it = std::find_if(cache.begin(), cache.end(),
                                 [&](const Entry& e) { return e.props.id == layer.props.id && e.ice == ice; });
    const std::uint64_t revision = layer.cells.revision();
    if (it != cache.end()) {
        if (it->revision == revision && it->props == layer.props)
            return it->cells;
        cache.erase(it);
    }
    if (cache.size() == kEntries)
        cache.pop_back();
    cache.push_front(Entry{layer.props, revision, ice, styledCells(layer, ice)});
    return cache.front().cells;
}

// Composites the cells of `rect` (on the canvas, not empty) into `out`,
// canvas cell (x, y) at (x - ox, y - oy).
void compositeInto(const LayerStack& stack, CellRect rect, Canvas& out, int ox, int oy,
                   const CompositeOptions& options) {
    for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x)
            out.at(x - ox, y - oy) = options.ground;
    std::vector<char> text(static_cast<std::size_t>(rect.width) * rect.height, 0);
    for (const Layer& l : stack.layers()) {
        if (!composites(l))
            continue;
        const CellRect r = intersect(rect, l.bounds());
        if (r.empty())
            continue;
        // a style the compositor draws is drawn once for the whole layer
        const bool styled = styledAtComposite(l.props);
        const Canvas styledLayer = styled ? styledCells(l, options.ice) : Canvas{0, 0};
        const Canvas& cells = styled ? styledLayer : l.cells;
        for (int y = r.y; y < r.y + r.height; ++y) {
            const int ly = y - l.props.y;
            for (int x = r.x; x < r.x + r.width; ++x) {
                char& t = text[static_cast<std::size_t>(y - rect.y) * rect.width + (x - rect.x)];
                bool isText = t;
                if (styled)
                    overStyled(out.at(x - ox, y - oy), isText, l.props, cells.at(x - l.props.x, ly), options.ice);
                else
                    over(out.at(x - ox, y - oy), isText, l.props, cells.at(x - l.props.x, ly), options.ice);
                t = isText;
            }
        }
    }
}

} // namespace

HalfPair halvesUnder(const Cell& below, bool ice) {
    if (const auto pair = halfBlocks(below, ice))
        return *pair;
    const Cell shown = iceCell(below, ice);
    const std::uint8_t fg = shown.fg.paletteIndex(), bg = shown.bg.paletteIndex();
    switch (shown.ch) {
    case kShadeLight: return {bg, bg};
    case kShadeDark: return {fg, fg};
    case kShadeMedium:
        return luma(fg) < luma(bg) ? HalfPair{fg, bg} : HalfPair{bg, fg};
    default: return {bg, bg};
    }
}

void recomposite(const LayerStack& stack, Canvas& out, CellRect rect, const CompositeOptions& options) {
    if (out.width() != stack.width() || out.height() != stack.height())
        throw LayerError("recomposite: the canvas is not the stack's size");
    rect = intersect(rect, stack.canvasRect());
    if (!rect.empty())
        compositeInto(stack, rect, out, 0, 0, options);
}

Canvas compositeRect(const LayerStack& stack, CellRect rect, const CompositeOptions& options) {
    rect = intersect(rect, stack.canvasRect());
    Canvas out(rect.width, rect.height);
    if (!rect.empty())
        compositeInto(stack, rect, out, rect.x, rect.y, options);
    return out;
}

Canvas composite(const LayerStack& stack, const CompositeOptions& options) {
    Canvas out(stack.width(), stack.height());
    recomposite(stack, out, stack.canvasRect(), options);
    return out;
}

Cell compositeCell(const LayerStack& stack, int x, int y, const CompositeOptions& options) {
    return compositeCellBelow(stack, stack.size(), x, y, options);
}

Cell compositeCellBelow(const LayerStack& stack, std::size_t index, int x, int y, const CompositeOptions& options) {
    Cell acc = options.ground;
    if (!stack.canvasRect().contains(x, y))
        return acc;
    bool text = false;
    const auto& layers = stack.layers();
    for (std::size_t i = 0; i < std::min(index, layers.size()); ++i)
        if (composites(layers[i]))
            if (const Cell* c = layers[i].cellAt(x, y)) {
                if (styledAtComposite(layers[i].props)) {
                    const Canvas& cells = cachedStyledCells(layers[i], options.ice);
                    overStyled(acc, text, layers[i].props, cells.at(x - layers[i].props.x, y - layers[i].props.y),
                               options.ice);
                } else {
                    over(acc, text, layers[i].props, *c, options.ice);
                }
            }
    return acc;
}

} // namespace bw
