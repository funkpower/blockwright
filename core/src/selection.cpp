#include "blockwright/selection.h"

#include "blockwright/edit.h"
#include "blockwright/halfblock.h"
#include "blockwright/layer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace bw {

namespace {

// Collects the cells an operation sets, over the canvas as it stood, and
// turns them into one CellEdit: each cell once, in row-major order, with
// its value before the operation. Cells off the canvas are dropped.
class EditBuilder {
public:
    explicit EditBuilder(const Canvas& canvas) : canvas_(canvas) {}

    void set(int x, int y, const Cell& c) {
        if (canvas_.contains(x, y))
            after_[{y, x}] = c;
    }
    bool has(int x, int y) const { return after_.count({y, x}) != 0; }

    // With `keepPairs`, a continuation set unchanged beside a lead set
    // changed stays, so the settle keeps that pair whole (PLAN §16), and so
    // does a lead set unchanged whose continuation is set to something else
    // (a narrow cluster typed over its own base).
    CellEdit finish(bool keepPairs = false) const {
        CellEdit e;
        const auto changedAt = [&](int x, int y) {
            const auto it = after_.find({y, x});
            return it != after_.end() && !identical(canvas_.at(x, y), it->second);
        };
        for (const auto& [at, c] : after_) {
            const auto [y, x] = at;
            const Cell& before = canvas_.at(x, y);
            const bool kept = keepPairs && c.ch.isContinuation() && changedAt(x - 1, y) &&
                              !after_.at({y, x - 1}).ch.isContinuation();
            const auto right = after_.find({y, x + 1});
            const bool spaced = keepPairs && !c.ch.isContinuation() && !before.ch.isContinuation() &&
                                right != after_.end() && canvas_.at(x + 1, y).ch.isContinuation() &&
                                !right->second.ch.isContinuation();
            if (kept || spaced || !identical(before, c))
                e.changes.push_back({x, y, before, c});
        }
        return e;
    }

private:
    const Canvas& canvas_;
    std::map<std::pair<int, int>, Cell> after_;  // (y, x) -> cell
};

void clearInto(EditBuilder& b, const Canvas& canvas, const Selection& sel, const Cell& blank) {
    const CellRect r = sel.clipped(canvas).bounds();
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x)
            if (sel.contains(x, y))
                b.set(x, y, blank);
}

// Pastes over the canvas as `b` already has it, so a paste after a clear
// sees the cleared cells (Under mode writes into them).
// A wide pair (PLAN §16) is pasted whole or not at all: a half whose other
// half isn't written (clipped at an edge, or kept out by the paste mode)
// is written as a space in its own colours.
void pasteInto(EditBuilder& b, const Canvas& canvas, const CellBlock& block, int x0, int y0, PasteMode mode,
               const Selection* cleared, const Cell& blank, PastePixels pixels = {}) {
    std::vector<std::optional<Cell>> row(static_cast<std::size_t>(block.width));
    for (int by = 0; by < block.height; ++by) {
        const long long ly = static_cast<long long>(y0) + by;
        if (ly < 0 || ly >= canvas.height())
            continue;
        const int y = static_cast<int>(ly);
        for (int bx = 0; bx < block.width; ++bx) {
            auto& out = row[static_cast<std::size_t>(bx)];
            out.reset();
            const long long lx = static_cast<long long>(x0) + bx;
            if (lx < 0 || lx >= canvas.width())
                continue;
            const int x = static_cast<int>(lx);
            const Cell under = cleared && cleared->contains(x, y) ? blank : canvas.at(x, y);
            if (const auto c = pasteCell(block.at(bx, by), under, mode, pixels, block.pixelLayer))
                out = writtenOver(under, *c);
        }
        for (int bx = 0; bx < block.width; ++bx) {
            auto& out = row[static_cast<std::size_t>(bx)];
            if (!out)
                continue;
            const bool lead = bx + 1 < block.width && !block.at(bx, by).ch.isContinuation() &&
                              block.at(bx + 1, by).ch.isContinuation();
            const bool cont = block.at(bx, by).ch.isContinuation();
            const bool whole = lead   ? row[static_cast<std::size_t>(bx + 1)].has_value()
                               : cont ? bx > 0 && !block.at(bx - 1, by).ch.isContinuation() &&
                                            row[static_cast<std::size_t>(bx - 1)].has_value()
                                      : true;
            if (!whole)
                out->ch = U' ';
            b.set(x0 + bx, y, *out);
        }
    }
}

// Whether (x, y) of `c` is the lead of a pair, or its continuation.
bool leadAt(const Canvas& c, int x, int y) {
    return c.contains(x + 1, y) && c.at(x + 1, y).ch.isContinuation() && !c.at(x, y).ch.isContinuation();
}
bool pairedContinuationAt(const Canvas& c, int x, int y) {
    return c.at(x, y).ch.isContinuation() && x > 0 && !c.at(x - 1, y).ch.isContinuation();
}

// Box-drawing glyphs by the arms they reach out with: up, right, down, left,
// each 0 (none), 1 (single) or 2 (double). CP437 has every corner, tee and
// cross for each mix of a horizontal and a vertical weight, so flips and
// rotations of these always land on another of them.
struct Box {
    char32_t ch;
    std::array<std::uint8_t, 4> arms;  // up, right, down, left
};

constexpr std::array<Box, 40> kBoxes = {{
    {U'│', {1, 0, 1, 0}}, {U'┤', {1, 0, 1, 1}}, {U'╡', {1, 0, 1, 2}}, {U'╢', {2, 0, 2, 1}},
    {U'╖', {0, 0, 2, 1}}, {U'╕', {0, 0, 1, 2}}, {U'╣', {2, 0, 2, 2}}, {U'║', {2, 0, 2, 0}},
    {U'╗', {0, 0, 2, 2}}, {U'╝', {2, 0, 0, 2}}, {U'╜', {2, 0, 0, 1}}, {U'╛', {1, 0, 0, 2}},
    {U'┐', {0, 0, 1, 1}}, {U'└', {1, 1, 0, 0}}, {U'┴', {1, 1, 0, 1}}, {U'┬', {0, 1, 1, 1}},
    {U'├', {1, 1, 1, 0}}, {U'─', {0, 1, 0, 1}}, {U'┼', {1, 1, 1, 1}}, {U'╞', {1, 2, 1, 0}},
    {U'╟', {2, 1, 2, 0}}, {U'╚', {2, 2, 0, 0}}, {U'╔', {0, 2, 2, 0}}, {U'╩', {2, 2, 0, 2}},
    {U'╦', {0, 2, 2, 2}}, {U'╠', {2, 2, 2, 0}}, {U'═', {0, 2, 0, 2}}, {U'╬', {2, 2, 2, 2}},
    {U'╧', {1, 2, 0, 2}}, {U'╨', {2, 1, 0, 1}}, {U'╤', {0, 2, 1, 2}}, {U'╥', {0, 1, 2, 1}},
    {U'╙', {2, 1, 0, 0}}, {U'╘', {1, 2, 0, 0}}, {U'╒', {0, 2, 1, 0}}, {U'╓', {0, 1, 2, 0}},
    {U'╫', {2, 1, 2, 1}}, {U'╪', {1, 2, 1, 2}}, {U'┘', {1, 0, 0, 1}}, {U'┌', {0, 1, 1, 0}},
}};

// The box glyph with these arms, or `fallback` if CP437 has none.
char32_t boxWith(const std::array<std::uint8_t, 4>& arms, char32_t fallback) {
    for (const Box& b : kBoxes)
        if (b.arms == arms)
            return b.ch;
    return fallback;
}

const Box* findBox(char32_t ch) {
    for (const Box& b : kBoxes)
        if (b.ch == ch)
            return &b;
    return nullptr;
}

// Glyph pairs a flip swaps (either way round).
constexpr std::array<std::pair<char32_t, char32_t>, 11> kFlipH = {{
    {U'▌', U'▐'}, {U'/', U'\\'}, {U'(', U')'}, {U'[', U']'}, {U'{', U'}'}, {U'<', U'>'},
    {U'≤', U'≥'}, {U'►', U'◄'}, {U'→', U'←'}, {U'⌐', U'¬'}, {U'«', U'»'},
}};
constexpr std::array<std::pair<char32_t, char32_t>, 5> kFlipV = {{
    {U'▀', U'▄'}, {U'/', U'\\'}, {U'▲', U'▼'}, {U'↑', U'↓'}, {U'⌠', U'⌡'},
}};

// Glyph cycles a quarter turn clockwise walks (a 2-cycle is its own
// counter-clockwise turn). Two quarter turns must be both flips, for every
// glyph. CP437 has no turned bracket, so the pairs a flip swaps and no turn
// can show are put two by two into 4-cycles, each pair's two glyphs opposite:
// a half turn swaps them as both flips do, and a quarter turn shows the other
// pair. ¬ turns to ⌡ and ⌠ to ¬ as drawn; their other two steps are the
// nearest CP437 has.
constexpr std::array<std::array<char32_t, 4>, 7> kTurn4 = {{
    {U'▀', U'▐', U'▄', U'▌'},
    {U'▲', U'►', U'▼', U'◄'},
    {U'↑', U'→', U'↓', U'←'},
    {U'¬', U'⌡', U'⌐', U'⌠'},
    {U'(', U'[', U')', U']'},
    {U'<', U'«', U'>', U'»'},
    {U'{', U'≤', U'}', U'≥'},
}};
constexpr std::array<std::pair<char32_t, char32_t>, 3> kTurn2 = {{
    {U'/', U'\\'}, {U'-', U'|'}, {U'↔', U'↕'},
}};

template <size_t N>
char32_t swapPair(const std::array<std::pair<char32_t, char32_t>, N>& pairs, char32_t ch) {
    for (const auto& [a, b] : pairs) {
        if (ch == a)
            return b;
        if (ch == b)
            return a;
    }
    return ch;
}

Cell flipCell(Cell c, Flip f) {
    c.ch = flipGlyph(c.ch, f);
    return c;
}

Cell rotateCell(Cell c, bool clockwise) {
    c.ch = rotateGlyph(c.ch, clockwise);
    return c;
}

} // namespace

CellRect CellRect::fromCorners(int x0, int y0, int x1, int y1) {
    return {std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0) + 1, std::abs(y1 - y0) + 1};
}

CellRect CellRect::intersected(const CellRect& o) const {
    if (empty() || o.empty())
        return {};
    const long long l = std::max(x, o.x), t = std::max(y, o.y);
    const long long r = std::min(static_cast<long long>(x) + width, static_cast<long long>(o.x) + o.width);
    const long long b = std::min(static_cast<long long>(y) + height, static_cast<long long>(o.y) + o.height);
    if (r <= l || b <= t)
        return {};
    return {static_cast<int>(l), static_cast<int>(t), static_cast<int>(r - l), static_cast<int>(b - t)};
}

Selection Selection::clipped(const Canvas& canvas) const {
    return Selection(rect_.intersected({0, 0, canvas.width(), canvas.height()}));
}

bool isBlank(const Cell& c) {
    if (c.transparent)
        return true;
    const bool space = c.ch == U' ' || c.ch == U'\0' || c.ch == U' ';
    const bool black = c.bg.isIndexed() ? c.bg.index == Black : c.bg.rgb == Rgb{};
    return space && black && !(c.attrs & attr::Blink) && !attr::showsOnBlank(c.attrs);
}

CellBlock copyBlock(const Canvas& canvas, const Selection& sel) {
    const CellRect r = sel.clipped(canvas).bounds();
    if (r.empty())
        return {};
    CellBlock b(r.width, r.height);
    for (int y = 0; y < r.height; ++y)
        for (int x = 0; x < r.width; ++x) {
            const int cx = r.x + x, cy = r.y + y;
            if (!sel.contains(cx, cy)) {
                b.at(x, y) = transparentCell();
                continue;
            }
            Cell& c = b.at(x, y) = canvas.at(cx, cy);
            // a cut half of a pair (PLAN §16): a space in the pair's colours
            if (leadAt(canvas, cx, cy) && !sel.contains(cx + 1, cy)) {
                c.ch = U' ';
            } else if (c.ch.isContinuation()) {
                if (!pairedContinuationAt(canvas, cx, cy))
                    c.ch = U' ';
                else if (!sel.contains(cx - 1, cy)) {
                    c = canvas.at(cx - 1, cy);
                    c.ch = U' ';
                }
            }
        }
    return b;
}

CellEdit clearSelection(const Canvas& canvas, const Selection& sel, const Cell& blank) {
    EditBuilder b(canvas);
    clearInto(b, canvas, sel, blank);
    return b.finish();
}

std::optional<Cell> pasteCell(const Cell& cell, const Cell& under, PasteMode mode, PastePixels pixels,
                              bool pixelCell) {
    if (cell.transparent) {
        const LayerPixels p = layerPixels(cell);
        if (!p.top && !p.bottom)
            return std::nullopt;
    }
    Cell pasted = cell;
    if (cell.transparent)
        pasted = halfPixelOver(cell, under, pixels.pixelLayer, pixels.ice);
    else if (pixelCell && !pixels.pixelLayer)
        pasted = pixelCellOnCells(cell, pixels.ice);
    switch (mode) {
    case PasteMode::Normal: return pasted;
    case PasteMode::Transparent: return isBlank(cell) ? std::nullopt : std::optional<Cell>(pasted);
    case PasteMode::Under: return isBlank(under) ? std::optional<Cell>(pasted) : std::nullopt;
    }
    return std::nullopt;
}

CellEdit pasteBlock(const Canvas& canvas, const CellBlock& block, int x, int y, PasteMode mode,
                    PastePixels pixels) {
    return stampBlock(canvas, block, x, y, mode, nullptr, Cell{}, pixels);
}

CellEdit stampBlock(const Canvas& canvas, const CellBlock& block, int x, int y, PasteMode mode,
                    const Selection* liftedFrom, const Cell& blank, PastePixels pixels) {
    EditBuilder b(canvas);
    if (liftedFrom)
        clearInto(b, canvas, *liftedFrom, blank);
    pasteInto(b, canvas, block, x, y, mode, liftedFrom, blank, pixels);
    // a pair landing on a continuation it leaves as it was stays whole
    CellEdit e = b.finish(true);
    e.moves = true;
    return e;
}

char32_t flipGlyph(char32_t ch, Flip flip) {
    if (const Box* box = findBox(ch)) {
        auto a = box->arms;
        if (flip == Flip::Horizontal)
            std::swap(a[1], a[3]);
        else
            std::swap(a[0], a[2]);
        return boxWith(a, ch);
    }
    return flip == Flip::Horizontal ? swapPair(kFlipH, ch) : swapPair(kFlipV, ch);
}

Glyph flipGlyph(Glyph g, Flip flip) {
    const std::optional<char32_t> cp = g.codePoint();
    return cp ? Glyph(flipGlyph(*cp, flip)) : g;
}

Glyph rotateGlyph(Glyph g, bool clockwise) {
    const std::optional<char32_t> cp = g.codePoint();
    return cp ? Glyph(rotateGlyph(*cp, clockwise)) : g;
}

char32_t rotateGlyph(char32_t ch, bool clockwise) {
    if (const Box* box = findBox(ch)) {
        const auto& a = box->arms;
        // Clockwise, what reached up now reaches right, and so on round.
        const std::array<std::uint8_t, 4> turned =
            clockwise ? std::array<std::uint8_t, 4>{a[3], a[0], a[1], a[2]}
                      : std::array<std::uint8_t, 4>{a[1], a[2], a[3], a[0]};
        return boxWith(turned, ch);
    }
    for (const auto& cycle : kTurn4)
        for (int i = 0; i < 4; ++i)
            if (cycle[i] == ch)
                return cycle[(i + (clockwise ? 1 : 3)) % 4];
    return swapPair(kTurn2, ch);
}

CellBlock transformed(const CellBlock& block, Transform t) {
    const int w = block.width, h = block.height;
    if (block.empty())
        return {};
    // a pair turned would stand on end (PLAN §16): no rotation
    if ((t == Transform::RotateClockwise || t == Transform::RotateCounterClockwise) && blockHoldsPair(block))
        return block;
    switch (t) {
    case Transform::FlipHorizontal: {
        CellBlock out(w, h);
        out.pixelLayer = block.pixelLayer;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                // a pair keeps its lead on the left: (lead, continuation)
                // lands as itself, not as (continuation, lead)
                if (x + 1 < w && !block.at(x, y).ch.isContinuation() && block.at(x + 1, y).ch.isContinuation()) {
                    out.at(w - 2 - x, y) = flipCell(block.at(x, y), Flip::Horizontal);
                    out.at(w - 1 - x, y) = block.at(x + 1, y);
                    ++x;
                    continue;
                }
                out.at(w - 1 - x, y) = flipCell(block.at(x, y), Flip::Horizontal);
            }
        return out;
    }
    case Transform::FlipVertical: {
        CellBlock out(w, h);
        out.pixelLayer = block.pixelLayer;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.at(x, h - 1 - y) = flipCell(block.at(x, y), Flip::Vertical);
        return out;
    }
    case Transform::RotateClockwise: {
        CellBlock out(h, w);
        out.pixelLayer = block.pixelLayer;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.at(h - 1 - y, x) = rotateCell(block.at(x, y), true);
        return out;
    }
    case Transform::RotateCounterClockwise: {
        CellBlock out(h, w);
        out.pixelLayer = block.pixelLayer;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.at(y, w - 1 - x) = rotateCell(block.at(x, y), false);
        return out;
    }
    }
    return {};
}

SelectionEdit transformSelection(const Canvas& canvas, const Selection& sel, Transform t, const Cell& blank) {
    const Selection from = sel.clipped(canvas);
    if (from.empty())
        return {{}, from};
    if ((t == Transform::RotateClockwise || t == Transform::RotateCounterClockwise) && selectionHoldsPair(canvas, from))
        return {{}, sel};
    const CellBlock block = transformed(copyBlock(canvas, from), t);
    const CellRect& r = from.bounds();
    EditBuilder b(canvas);
    clearInto(b, canvas, from, blank);
    pasteInto(b, canvas, block, r.x, r.y, PasteMode::Normal, &from, blank);
    CellEdit e = b.finish(true);
    e.moves = true;
    return {e, Selection(CellRect{r.x, r.y, block.width, block.height}).clipped(canvas)};
}

SelectionEdit moveSelection(const Canvas& canvas, const Selection& sel, int dx, int dy, PasteMode mode,
                            const Cell& blank) {
    const Selection from = sel.clipped(canvas);
    if (from.empty())
        return {{}, from};
    const CellRect& r = from.bounds();
    const long long nx = static_cast<long long>(r.x) + dx, ny = static_cast<long long>(r.y) + dy;
    // Far enough off the canvas that nothing lands: just the clear.
    const long long w = canvas.width(), h = canvas.height();
    if (nx >= w || ny >= h || nx + r.width <= 0 || ny + r.height <= 0)
        return {clearSelection(canvas, from, blank), {}};
    const CellRect to{static_cast<int>(nx), static_cast<int>(ny), r.width, r.height};
    return {stampBlock(canvas, copyBlock(canvas, from), to.x, to.y, mode, &from, blank), Selection(to).clipped(canvas)};
}

int mirrorColumn(int width, int x) { return width - 1 - x; }

bool selectionHoldsPair(const Canvas& canvas, const Selection& sel) {
    const CellRect r = sel.clipped(canvas).bounds();
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x)
            if (sel.contains(x, y) && (leadAt(canvas, x, y) || pairedContinuationAt(canvas, x, y)))
                return true;
    return false;
}

bool blockHoldsPair(const CellBlock& block) {
    for (int y = 0; y < block.height; ++y)
        for (int x = 1; x < block.width; ++x)
            if (block.at(x, y).ch.isContinuation() && !block.at(x - 1, y).ch.isContinuation())
                return true;
    return false;
}

CellEdit mirrorEdit(const Canvas& canvas, const CellEdit& edit, int* pairsUnmirrored) {
    if (pairsUnmirrored)
        *pairsUnmirrored = 0;
    // Each cell the edit asks for is mirrored with the pair it is a half of
    // once the edit lands (EditedCells over the settled edit, so a lead
    // written over leaves its old continuation out of it, and over the
    // canvas, so an unchanged half the edit leaves out is still in it), or
    // alone. Only those units and their images reserve cells: the settle's
    // cleanup is provisional, and what comes back is the edit as asked and
    // the images of its units, unsettled, so the settle runs once, after
    // fitLayerEdit, and a pair fitting refuses takes none of the cleanup it
    // would have caused with it (PLAN §16, Editing).
    const CellEdit settled = settleWide(canvas, edit);
    const EditedCells after(canvas, settled);
    std::map<std::pair<int, int>, bool> units;  // (y, x) of each unit's first cell -> a pair
    std::set<std::pair<int, int>> owned;  // (y, x): the cells of every unit
    for (const CellChange& c : edit.changes) {
        if (!canvas.contains(c.x, c.y))
            continue;
        const std::optional<int> lead = after.pairLead(c.x, c.y);
        const int x = lead.value_or(c.x);
        units[{c.y, x}] = lead.has_value();
        owned.insert({c.y, x});
        if (lead)
            owned.insert({c.y, x + 1});
    }
    EditBuilder all(canvas);
    for (const CellChange& c : edit.changes)
        all.set(c.x, c.y, c.after);
    const int w = canvas.width();
    // a drawn image is settled with the rest; a moved one is pasted
    const auto put = [&](int mx, int y, const Cell& image) {
        all.set(mx, y, edit.moves && canvas.contains(mx, y) ? writtenOver(canvas.at(mx, y), image) : image);
    };
    for (const auto& [yx, pair] : units) {
        const auto [y, x] = yx;
        const Cell& c = *after.at(x, y);
        if (!pair) {
            const int mx = mirrorColumn(w, x);
            if (!owned.contains({y, mx}))
                put(mx, y, flipCell(c, Flip::Horizontal));
            continue;
        }
        // a pair (x, x + 1): its image's lead at mirrorColumn(x + 1)
        const int ml = mirrorColumn(w, x + 1), mr = mirrorColumn(w, x);
        if (ml == x)
            continue;  // its own image (an even width, across the axis)
        if (owned.contains({y, ml}) || owned.contains({y, mr})) {
            // over its own image (an odd width) gets no copy; one over
            // another unit keeps that unit
            if (pairsUnmirrored && (ml == x + 1 || mr == x))
                ++*pairsUnmirrored;
            continue;
        }
        put(ml, y, flipCell(c, Flip::Horizontal));
        put(mr, y, *after.at(x + 1, y));
    }
    CellEdit e = all.finish(true);
    e.moves = edit.moves;
    e.typed = edit.typed;
    return e;
}

} // namespace bw
