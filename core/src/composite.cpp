#include "blockwright/composite.h"

#include "blockwright/ansi.h"
#include "blockwright/ramp.h"

#include <vector>

namespace bw {

namespace {

// Perceived brightness of a palette colour, for ▒'s darker half.
int luma(std::uint8_t i) {
    const Rgb& c = kPalette[i & 15];
    return 299 * c.r + 587 * c.g + 114 * c.b;
}

bool composites(const Layer& l) {
    return l.props.visible && l.props.kind != LayerKind::Generator && l.props.kind != LayerKind::Reference;
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
    // blink only while the foreground is the half under: the pixel has no
    // blink of its own.
    acc.attrs = shown.attrs;
    if (acc.ch != (bottom ? kUpperHalf : kLowerHalf))
        acc.attrs &= std::uint8_t(~attr::Blink);
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
    if (rect.empty())
        return;
    for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x)
            out.at(x, y) = options.ground;
    std::vector<char> text(static_cast<std::size_t>(rect.width) * rect.height, 0);
    for (const Layer& l : stack.layers()) {
        if (!composites(l))
            continue;
        const CellRect r = intersect(rect, l.bounds());
        for (int y = r.y; y < r.y + r.height; ++y) {
            const int ly = y - l.props.y;
            for (int x = r.x; x < r.x + r.width; ++x) {
                bool t = text[static_cast<std::size_t>(y - rect.y) * rect.width + (x - rect.x)];
                over(out.at(x, y), t, l.props, l.cells.at(x - l.props.x, ly), options.ice);
                text[static_cast<std::size_t>(y - rect.y) * rect.width + (x - rect.x)] = t;
            }
        }
    }
}

Canvas composite(const LayerStack& stack, const CompositeOptions& options) {
    Canvas out(stack.width(), stack.height());
    recomposite(stack, out, stack.canvasRect(), options);
    return out;
}

Cell compositeCell(const LayerStack& stack, int x, int y, const CompositeOptions& options) {
    Cell acc = options.ground;
    if (!stack.canvasRect().contains(x, y))
        return acc;
    bool text = false;
    for (const Layer& l : stack.layers())
        if (composites(l))
            if (const Cell* c = l.cellAt(x, y))
                over(acc, text, l.props, *c, options.ice);
    return acc;
}

} // namespace bw
