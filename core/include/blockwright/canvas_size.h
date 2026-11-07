#pragma once
// Canvas Size (PLAN §5, #397): the canvas made another size, the old canvas
// placed in the new one by an anchor. Crop to Selection and Trim (#397's
// later slices) are this with a shift they work out themselves.
#include "blockwright/layer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bw {

// Where the old canvas sits in the new one: a corner, the middle of a side
// or the centre. Row by row, top-left first.
enum class Anchor : std::uint8_t { TopLeft, Top, TopRight, Left, Centre, Right, BottomLeft, Bottom, BottomRight };

// "top-left", "top", ... "bottom-right" ("centre" for the middle), and back;
// empty for any other name.
std::string_view anchorName(Anchor anchor);
std::optional<Anchor> anchorFromName(std::string_view name);

// Where the old canvas's top-left cell lands on the new canvas, in cells:
// 0 at a left (top) anchor, the whole change at a right (bottom) one, and
// at the centre half of it, rounded toward 0, so the odd column goes on the
// right (the odd row at the bottom) whether the canvas grows or shrinks,
// and growing by n and shrinking by n at the centre give the old place back.
struct CellShift {
    int dx = 0, dy = 0;
    bool operator==(const CellShift&) const = default;
};
CellShift anchorShift(int oldWidth, int oldHeight, int width, int height, Anchor anchor);

// Why `stack` can't take a canvas of width by height, or empty when it can:
// a canvas is 1 to kMaxColumns wide and 1 to kMaxRows high (ansi.h, as
// File > New and the readers hold it), and the layers it leaves may hold no
// more cells together than a project holds (project::kMaxTotalCells).
std::optional<std::string> canvasSizeProblem(const LayerStack& stack, int width, int height);

// `stack` on a canvas of width by height, the old canvas's top-left at
// `shift` on it. A layer that covers the old canvas exactly (at (0, 0), the
// canvas's size) and is a cells or pixels layer follows the canvas: it is
// made the new canvas's size at (0, 0), its cells moved by `shift`, the
// cells it gains its blank cell (Cell{} on the bottom cells layer, the
// blank Document::blankCell gives there; transparentCell() on any other)
// and those past the new edges dropped. A pair cut there keeps the half on
// the canvas as a space in that half's colours (PLAN §16). Every other
// layer (smaller, moved or larger, and every text, generator and reference
// layer) keeps all its cells and moves by `shift`; a reference picture by
// `shift` in canvas pixels, a cell being `cellWidth` (8, or 9 with 9 px
// letter spacing) by 16, clamped to ReferenceImage::kMaxPlace. Ids, order,
// the next id and every other property stay as they are. `shift` may put
// the old canvas anywhere; nothing here checks the size (canvasSizeProblem).
LayerStack resizeCanvas(const LayerStack& stack, int width, int height, CellShift shift, int cellWidth);

} // namespace bw
