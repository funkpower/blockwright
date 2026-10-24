#include "blockwright/selection.h"

#include "blockwright/halfblock.h"
#include "blockwright/layer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <utility>

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

    CellEdit finish() const {
        CellEdit e;
        for (const auto& [at, c] : after_) {
            const Cell& before = canvas_.at(at.second, at.first);
            if (!identical(before, c))
                e.changes.push_back({at.second, at.first, before, c});
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
void pasteInto(EditBuilder& b, const Canvas& canvas, const CellBlock& block, int x0, int y0, PasteMode mode,
               const Selection* cleared, const Cell& blank, PastePixels pixels = {}) {
    for (int by = 0; by < block.height; ++by)
        for (int bx = 0; bx < block.width; ++bx) {
            const long long lx = static_cast<long long>(x0) + bx, ly = static_cast<long long>(y0) + by;
            if (lx < 0 || ly < 0 || lx >= canvas.width() || ly >= canvas.height())
                continue;
            const int x = static_cast<int>(lx), y = static_cast<int>(ly);
            const Cell under = cleared && cleared->contains(x, y) ? blank : canvas.at(x, y);
            if (const auto c = pasteCell(block.at(bx, by), under, mode, pixels, block.pixelLayer))
                b.set(x, y, writtenOver(under, *c));
        }
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
    return space && black && !(c.attrs & attr::Blink);
}

CellBlock copyBlock(const Canvas& canvas, const Selection& sel) {
    const CellRect r = sel.clipped(canvas).bounds();
    if (r.empty())
        return {};
    CellBlock b(r.width, r.height);
    for (int y = 0; y < r.height; ++y)
        for (int x = 0; x < r.width; ++x) {
            if (sel.contains(r.x + x, r.y + y))
                b.at(x, y) = canvas.at(r.x + x, r.y + y);
            else
                b.at(x, y) = transparentCell();
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
    CellEdit e = b.finish();
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
    switch (t) {
    case Transform::FlipHorizontal: {
        CellBlock out(w, h);
        out.pixelLayer = block.pixelLayer;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                out.at(w - 1 - x, y) = flipCell(block.at(x, y), Flip::Horizontal);
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
    const CellBlock block = transformed(copyBlock(canvas, from), t);
    const CellRect& r = from.bounds();
    EditBuilder b(canvas);
    clearInto(b, canvas, from, blank);
    pasteInto(b, canvas, block, r.x, r.y, PasteMode::Normal, &from, blank);
    CellEdit e = b.finish();
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

CellEdit mirrorEdit(const Canvas& canvas, const CellEdit& edit) {
    EditBuilder own(canvas);
    for (const CellChange& c : edit.changes)
        own.set(c.x, c.y, c.after);
    EditBuilder all = own;
    for (const CellChange& c : edit.changes) {
        const int mx = mirrorColumn(canvas.width(), c.x);
        if (!canvas.contains(c.x, c.y) || own.has(mx, c.y))
            continue;
        // a drawn image is settled with the rest; a moved one is pasted
        const Cell image = flipCell(c.after, Flip::Horizontal);
        all.set(mx, c.y, edit.moves && canvas.contains(mx, c.y) ? writtenOver(canvas.at(mx, c.y), image) : image);
    }
    CellEdit e = all.finish();
    e.moves = edit.moves;
    return e;
}

} // namespace bw
