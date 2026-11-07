#include "blockwright/canvas_size.h"

#include "blockwright/ansi.h"
#include "blockwright/project.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>

namespace bw {

namespace {

constexpr std::array<std::string_view, 9> kAnchorNames = {
    "top-left", "top", "top-right", "left", "centre", "right", "bottom-left", "bottom", "bottom-right"};

int shiftFor(int from, int to, int column) {
    const int change = to - from;
    return column == 0 ? 0 : column == 2 ? change : change / 2;
}

int movePlace(int at, long long by) {
    return int(std::clamp<long long>(at + by, -ReferenceImage::kMaxPlace, ReferenceImage::kMaxPlace));
}

// The layer's cells on the new canvas, moved by `shift`: the cells it gains
// are `blank`, and a pair the new edges cut keeps its half on the canvas as
// a space in that half's colours.
Canvas followCanvas(const Canvas& old, int width, int height, CellShift shift, const Cell& blank) {
    Canvas next(width, height);
    for (int y = 0; y < height; ++y) {
        const int sy = y - shift.dy;
        for (int x = 0; x < width; ++x) {
            const int sx = x - shift.dx;
            next.at(x, y) = old.contains(sx, sy) ? old.at(sx, sy) : blank;
        }
        if (sy < 0 || sy >= old.height())
            continue;
        // a continuation whose lead is off the new left edge
        if (Cell& first = next.at(0, y); first.ch.isContinuation())
            first.ch = U' ';
        // a lead whose continuation is off the new right edge
        const int sx = width - 1 - shift.dx;
        if (old.contains(sx, sy) && old.contains(sx + 1, sy) && old.at(sx + 1, sy).ch.isContinuation() &&
            !old.at(sx, sy).ch.isContinuation())
            next.at(width - 1, y).ch = U' ';
    }
    return next;
}

} // namespace

std::string_view anchorName(Anchor anchor) { return kAnchorNames[std::size_t(anchor)]; }

std::optional<Anchor> anchorFromName(std::string_view name) {
    for (std::size_t i = 0; i < kAnchorNames.size(); ++i)
        if (kAnchorNames[i] == name)
            return Anchor(i);
    return std::nullopt;
}

CellShift anchorShift(int oldWidth, int oldHeight, int width, int height, Anchor anchor) {
    const int i = int(anchor);
    return {shiftFor(oldWidth, width, i % 3), shiftFor(oldHeight, height, i / 3)};
}

std::optional<std::string> canvasSizeProblem(const LayerStack& stack, int width, int height) {
    if (width < 1 || height < 1 || width > kMaxColumns || height > kMaxRows)
        return "a canvas is 1 to " + std::to_string(kMaxColumns) + " columns wide and 1 to " +
               std::to_string(kMaxRows) + " rows high";
    std::uint64_t cells = 0;
    const CellRect canvas = stack.canvasRect();
    for (const Layer& l : stack.layers()) {
        const bool follows = (l.props.kind == LayerKind::Cells || l.props.kind == LayerKind::Pixels) &&
                             l.bounds() == canvas;
        cells += follows ? std::uint64_t(width) * std::uint64_t(height)
                         : std::uint64_t(l.width()) * std::uint64_t(l.height());
    }
    if (cells > project::kMaxTotalCells)
        return "the layers would hold over " + std::to_string(project::kMaxTotalCells) + " cells";
    return std::nullopt;
}

LayerStack resizeCanvas(const LayerStack& stack, int width, int height, CellShift shift, int cellWidth) {
    LayerStack out = stack;
    const CellRect canvas = stack.canvasRect();
    out.resize(width, height);
    for (std::size_t i = 0; i < out.size(); ++i) {
        Layer& l = out[i];
        const bool follows =
            (l.props.kind == LayerKind::Cells || l.props.kind == LayerKind::Pixels) && l.bounds() == canvas;
        if (follows) {
            const Cell blank = i == 0 && l.props.kind == LayerKind::Cells ? Cell{} : transparentCell();
            l.cells = followCanvas(l.cells, width, height, shift, blank);
            continue;
        }
        // a reference layer is placed by its picture, in canvas pixels
        if (l.props.kind != LayerKind::Reference) {
            l.props.x = int(std::clamp<long long>(1LL * l.props.x + shift.dx, INT_MIN, INT_MAX));
            l.props.y = int(std::clamp<long long>(1LL * l.props.y + shift.dy, INT_MIN, INT_MAX));
        }
        if (l.props.reference) {
            l.props.reference->x = movePlace(l.props.reference->x, 1LL * shift.dx * cellWidth);
            l.props.reference->y = movePlace(l.props.reference->y, 1LL * shift.dy * 16);
        }
    }
    return out;
}

int canvasCellWidth(const std::optional<Sauce>& sauce) { return sauce && sauce->letterSpacing() == 2 ? 9 : 8; }

Selection resizedSelection(const Selection& selection, CellShift shift, int width, int height) {
    if (selection.empty())
        return {};
    const CellRect& b = selection.bounds();
    const CellRect kept = intersect(CellRect{b.x + shift.dx, b.y + shift.dy, b.width, b.height}, {0, 0, width, height});
    return kept.empty() ? Selection() : Selection(kept);
}

} // namespace bw
