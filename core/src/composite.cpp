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

// What laying a layer cell over the composite wrote: its glyph, its colours
// (and attributes), both or neither.
constexpr std::uint8_t kWroteGlyph = 1, kWroteColours = 2, kWroteBoth = kWroteGlyph | kWroteColours;

// Lays one layer cell over the cell composited so far (`acc`, `text` whether
// it is text), and says what it wrote.
std::uint8_t over(Cell& acc, bool& text, const LayerProperties& props, const Cell& lc, bool ice) {
    if (props.kind != LayerKind::Pixels) {
        if (lc.transparent)
            return 0;
        std::uint8_t wrote = kWroteBoth;
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
            acc.dithered = false;  // a look neither layer's dither laid
            wrote = kWroteGlyph;
            break;
        case LayerMode::ColoursOnly:
            acc.fg = lc.fg;
            acc.bg = lc.bg;
            acc.attrs = lc.attrs;
            acc.dithered = false;
            wrote = kWroteColours;
            break;
        }
        acc.transparent = false;
        return wrote;
    }
    // A pixel cell may be transparent and still hold one half (layer.h).
    if (text)
        return 0;
    const LayerPixels p = layerPixels(lc);
    if (p.top && p.bottom) {
        acc = halfBlockCell(HalfPair{*p.top, *p.bottom}, ice);
        return kWroteBoth;
    }
    if (!p.top && !p.bottom)
        return 0;
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
    return kWroteBoth;
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
std::uint8_t overStyled(Cell& acc, bool& text, const LayerProperties& props, const Cell& lc, bool ice) {
    if (props.kind == LayerKind::Pixels && text)
        return 0;
    return over(acc, text, styledProps(props), lc, ice);
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

// Whether a layer cell is the right half of a wide pair (PLAN §16): an
// opaque continuation.
bool continuation(const Cell& c) {
    return !c.transparent && c.ch.isContinuation();
}

// A visible layer as the compositor reads it: its cells, or its styled cells
// for a layer whose style the compositor draws, with cell (0, 0) at canvas
// cell (x, y).
struct LayerView {
    const LayerProperties* props = nullptr;
    const Canvas* cells = nullptr;
    int x = 0, y = 0;
    bool styled = false;

    const Cell* at(int cx, int cy) const {
        const long lx = long(cx) - x, ly = long(cy) - y;
        return lx >= 0 && ly >= 0 && lx < cells->width() && ly < cells->height() ? &cells->at(int(lx), int(ly))
                                                                                  : nullptr;
    }
    CellRect bounds() const { return {x, y, cells->width(), cells->height()}; }
    std::uint8_t lay(Cell& acc, bool& text, const Cell& lc, bool ice) const {
        return styled ? overStyled(acc, text, *props, lc, ice) : over(acc, text, *props, lc, ice);
    }
};

// The topmost view (index + 1, 0 for the ground) whose cell wrote a cell's
// glyph, and the topmost that wrote its colours.
struct Source {
    std::uint32_t glyph = 0, colours = 0;
};

// Lays a row of one view's cells (`lc`, from canvas column x0) over the
// row composited so far (`acc`, `text` whether each cell is text). With
// `src`, records where each cell's glyph and colours came from (`id`).
// Says whether any of the cells is a continuation.
template <bool Styled, bool Track>
bool layRow(const LayerProperties& props, const Cell* lc, Cell* acc, char* text, Source* src, std::uint32_t id,
            int n, bool ice) {
    bool pairs = false;
    for (int i = 0; i < n; ++i) {
        pairs |= lc[i].ch.isContinuation();
        bool isText = text[i];
        const std::uint8_t wrote =
            Styled ? overStyled(acc[i], isText, props, lc[i], ice) : over(acc[i], isText, props, lc[i], ice);
        text[i] = isText;
        if constexpr (Track) {
            if (wrote & kWroteGlyph)
                src[i].glyph = id;
            if (wrote & kWroteColours)
                src[i].colours = id;
        }
    }
    return pairs;
}

// Composites each cell of `area` (on the canvas, not empty) on its own into
// `out`, canvas cell (x, y) at (x - ox, y - oy), recording in `src` (when
// given) where each cell's glyph and colours came from. Says whether any
// view holds a continuation in the area or just right of it, so that pairs
// may need settling.
bool compositeCells(const std::vector<LayerView>& views, CellRect area, Canvas& out, int ox, int oy,
                    const CompositeOptions& options, std::vector<Source>* src) {
    for (int y = area.y; y < area.y + area.height; ++y)
        std::fill_n(&out.at(area.x - ox, y - oy), area.width, options.ground);
    bool pairs = false;
    std::vector<char> text(static_cast<std::size_t>(area.width) * area.height, 0);
    const int right = area.x + area.width;
    for (std::size_t i = 0; i < views.size(); ++i) {
        const LayerView& v = views[i];
        const CellRect r = intersect(area, v.bounds());
        if (r.empty())
            continue;
        const auto row = v.styled ? (src ? layRow<true, true> : layRow<true, false>)
                                  : (src ? layRow<false, true> : layRow<false, false>);
        for (int y = r.y; y < r.y + r.height; ++y) {
            const std::size_t k = static_cast<std::size_t>(y - area.y) * area.width + (r.x - area.x);
            pairs |= row(*v.props, &v.cells->at(r.x - v.x, y - v.y), &out.at(r.x - ox, y - oy), &text[k],
                         src ? &(*src)[k] : nullptr, std::uint32_t(i + 1), r.width, options.ice);
            if (r.x + r.width == right)
                if (const Cell* c = v.at(right, y))
                    pairs |= continuation(*c);
        }
    }
    return pairs;
}

// The pair pass after per-cell compositing (PLAN §16), over the cells of
// `area` as compositeCells left them with `src`: a pair shows only where
// both its halves show, from the same layer; otherwise the half that shows
// is a space in its own colours. A pair that shows whole in two sets of
// colours (a colours-only layer over one half) takes the colours laid last,
// the lead's if both were laid by the same layer. Cells outside the area
// count as not showing, so only cells clear of its left and right edges, or
// at the canvas's, come out right.
void settlePairs(const std::vector<LayerView>& views, CellRect area, Canvas& out, int ox, int oy,
                 const std::vector<Source>& src) {
    struct Fix {
        int x, y;
        Cell c;
    };
    std::vector<Fix> fixes;
    const auto at = [&](int x, int y) -> const Source& {
        return src[static_cast<std::size_t>(y - area.y) * area.width + (x - area.x)];
    };
    const auto blank = [](Cell c) {
        c.ch = Glyph(U' ');
        return c;
    };
    const int right = area.x + area.width;
    for (int y = area.y; y < area.y + area.height; ++y)
        for (int x = area.x; x < right; ++x) {
            const Source& s = at(x, y);
            if (!s.glyph)
                continue;
            const LayerView& v = views[s.glyph - 1];
            const Cell& shown = out.at(x - ox, y - oy);
            if (continuation(*v.at(x, y))) {
                const Cell* lead = v.at(x - 1, y);
                const bool whole = x > area.x && at(x - 1, y).glyph == s.glyph && lead && !continuation(*lead);
                if (!whole)
                    fixes.push_back({x, y, blank(shown)});
                continue;
            }
            const Cell* next = v.at(x + 1, y);
            if (!next || !continuation(*next))
                continue;
            if (x + 1 >= right || at(x + 1, y).glyph != s.glyph) {
                fixes.push_back({x, y, blank(shown)});
                continue;
            }
            const Cell& cont = out.at(x + 1 - ox, y - oy);
            if (shown.fg == cont.fg && shown.bg == cont.bg && shown.attrs == cont.attrs)
                continue;
            const bool contWins = at(x + 1, y).colours > s.colours;
            const Cell& from = contWins ? cont : shown;
            Cell to = contWins ? shown : cont;
            to.fg = from.fg;
            to.bg = from.bg;
            to.attrs = from.attrs;
            fixes.push_back({contWins ? x : x + 1, y, to});
        }
    for (const Fix& f : fixes)
        out.at(f.x - ox, f.y - oy) = f.c;
}

// Composites `area` (on the canvas, not empty) into `out` and settles the
// pairs in it; only cells clear of the area's left and right edges, or at
// the canvas's, come out right.
void compositeArea(const std::vector<LayerView>& views, CellRect area, Canvas& out, int ox, int oy,
                   const CompositeOptions& options) {
    if (!compositeCells(views, area, out, ox, oy, options, nullptr))
        return;
    std::vector<Source> src(static_cast<std::size_t>(area.width) * area.height);
    compositeCells(views, area, out, ox, oy, options, &src);
    settlePairs(views, area, out, ox, oy, src);
}

// Composites the cells of `rect` (on the canvas, not empty) into `out`,
// canvas cell (x, y) at (x - ox, y - oy). A cell's neighbours on its row
// decide whether a pair there shows, so the cells are composited with a
// column more each side, where the canvas has one.
void compositeInto(const LayerStack& stack, CellRect rect, Canvas& out, int ox, int oy,
                   const CompositeOptions& options) {
    const CellRect outer = intersect({rect.x - 1, rect.y, rect.width + 2, rect.height}, stack.canvasRect());
    // a style the compositor draws is drawn once for the whole layer
    std::deque<Canvas> styled;
    std::vector<LayerView> views;
    for (const Layer& l : stack.layers()) {
        if (!composites(l) || intersect(outer, l.bounds()).empty())
            continue;
        const bool s = styledAtComposite(l.props);
        if (s)
            styled.push_back(styledCells(l, options.ice));
        views.push_back({&l.props, s ? &styled.back() : &l.cells, l.props.x, l.props.y, s});
    }
    if (outer == rect) {
        compositeArea(views, rect, out, ox, oy, options);
        return;
    }
    Canvas work(outer.width, outer.height);
    compositeArea(views, outer, work, outer.x, outer.y, options);
    for (int y = rect.y; y < rect.y + rect.height; ++y)
        for (int x = rect.x; x < rect.x + rect.width; ++x)
            out.at(x - ox, y - oy) = work.at(x - outer.x, y - outer.y);
}

} // namespace

HalfPair halvesUnder(const Cell& below, bool ice) {
    if (const auto pair = halfBlocks(below, ice))
        return *pair;
    const Cell shown = iceCell(below, ice);
    const std::uint8_t fg = shown.fg.paletteIndex(), bg = shown.bg.paletteIndex();
    switch (shown.ch.codePointOr(kNoCodePoint)) {
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
    // a change to one half of a pair shows in the other (PLAN §16)
    rect = intersect(rect, stack.canvasRect());
    if (!rect.empty())
        rect = intersect({rect.x - 1, rect.y, rect.width + 2, rect.height}, stack.canvasRect());
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

Coverage coverageAbove(const LayerStack& stack, std::size_t index, CellRect rect, const CompositeOptions& options,
                       ShownOffset shown, ShownCells pending) {
    Coverage out;
    out.rect = intersect(rect, stack.canvasRect());
    if (out.rect.empty()) {
        out.rect = {};
        return out;
    }
    const CellRect& r0 = out.rect;
    out.halves.assign(static_cast<std::size_t>(r0.width) * r0.height, 0);
    // whether each cell is text so far, as compositeInto tracks it
    std::vector<char> text(out.halves.size(), 0);
    // the pending layer's cells in progress, by cell of the rect
    std::vector<const Cell*> over;
    if (pending.id && pending.changes) {
        over.assign(out.halves.size(), nullptr);
        for (const CellChange& c : *pending.changes)
            if (r0.contains(c.x, c.y))
                over[static_cast<std::size_t>(c.y - r0.y) * r0.width + (c.x - r0.x)] = &c.after;
    }
    const auto& layers = stack.layers();
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const Layer& l = layers[i];
        if (!composites(l))
            continue;
        const bool moved = shown.id && l.props.id == shown.id;
        const int lx = moved ? shown.x : l.props.x, ly = moved ? shown.y : l.props.y;
        const CellRect r = intersect(r0, {lx, ly, l.width(), l.height()});
        if (r.empty())
            continue;
        const bool above = i > index;
        const bool styled = styledAtComposite(l.props);
        const Canvas& cells = styled ? cachedStyledCells(l, options.ice) : l.cells;
        // a styled layer composites as a cells layer (styledProps)
        const bool pixels = !styled && l.props.kind == LayerKind::Pixels;
        const bool blockedByText = l.props.kind == LayerKind::Pixels;
        const bool isText = !styled && l.props.kind == LayerKind::Text;
        const LayerMode mode = styled ? styledProps(l.props).mode : l.props.mode;
        const bool edited = !over.empty() && l.props.id == pending.id;
        for (int y = r.y; y < r.y + r.height; ++y)
            for (int x = r.x; x < r.x + r.width; ++x) {
                const std::size_t k = static_cast<std::size_t>(y - r0.y) * r0.width + (x - r0.x);
                const Cell& lc = edited && over[k] ? *over[k] : cells.at(x - lx, y - ly);
                if (blockedByText && text[k])
                    continue;
                if (pixels) {
                    // a pixel cell may be transparent and still hold one half
                    const LayerPixels p = layerPixels(lc);
                    if (above)
                        out.halves[k] |= (p.top ? Coverage::kTop : 0) | (p.bottom ? Coverage::kBottom : 0);
                    continue;
                }
                if (lc.transparent)
                    continue;
                if (above)
                    out.halves[k] = Coverage::kBoth;
                if (mode != LayerMode::ColoursOnly)
                    text[k] = isText;
            }
    }
    return out;
}

Cell compositeCell(const LayerStack& stack, int x, int y, const CompositeOptions& options) {
    return compositeCellBelow(stack, stack.size(), x, y, options);
}

Cell compositeCellBelow(const LayerStack& stack, std::size_t index, int x, int y, const CompositeOptions& options) {
    Cell acc = options.ground;
    if (!stack.canvasRect().contains(x, y))
        return acc;
    bool text = false, pairs = false;
    const auto& layers = stack.layers();
    const std::size_t n = std::min(index, layers.size());
    for (std::size_t i = 0; i < n; ++i) {
        const Layer& l = layers[i];
        if (!composites(l))
            continue;
        const bool styled = styledAtComposite(l.props);
        const LayerView v{&l.props, styled ? &cachedStyledCells(l, options.ice) : &l.cells, l.props.x, l.props.y,
                          styled};
        if (const Cell* c = v.at(x, y))
            v.lay(acc, text, *c, options.ice);
        for (int dx = -1; dx <= 1; ++dx)
            if (const Cell* c = v.at(x + dx, y))
                pairs |= continuation(*c);
    }
    if (!pairs)
        return acc;
    // A pair beside it: the three cells around it composited and settled.
    // A styled layer's cells there are copied, as styling the next layer may
    // drop this one's from the cache.
    const CellRect around{x - 1, y, 3, 1};
    std::deque<Canvas> copies;
    std::vector<LayerView> views;
    for (std::size_t i = 0; i < n; ++i) {
        const Layer& l = layers[i];
        const CellRect b = intersect(around, l.bounds());
        if (!composites(l) || b.empty())
            continue;
        if (!styledAtComposite(l.props)) {
            views.push_back({&l.props, &l.cells, l.props.x, l.props.y, false});
            continue;
        }
        const Canvas& cells = cachedStyledCells(l, options.ice);
        Canvas& copy = copies.emplace_back(b.width, 1);
        for (int cx = b.x; cx < b.x + b.width; ++cx)
            copy.at(cx - b.x, 0) = cells.at(cx - l.props.x, y - l.props.y);
        views.push_back({&l.props, &copy, b.x, y, true});
    }
    const CellRect area = intersect(around, stack.canvasRect());
    Canvas work(area.width, 1);
    compositeArea(views, area, work, area.x, area.y, options);
    return work.at(x - area.x, 0);
}

} // namespace bw
