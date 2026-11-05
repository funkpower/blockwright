#include "blockwright/stamp_slice.h"

#include "blockwright/ansi.h"
#include "blockwright/frame.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace bw {

namespace {

// Why insets `lead` and `trail` can't cut `size` cells along one axis, or
// empty. `resizes` says whether the slice resizes along it; `what` names
// the axis, `across` its insets, `unit` and `extent` its cells.
std::string axisProblem(int size, int lead, int trail, bool resizes, const char* what, const char* across,
                        const char* unit, const char* extent) {
    if (lead < 0 || trail < 0)
        return "an inset is negative";
    if (!resizes)
        return lead || trail ? std::string("a stamp that doesn't resize in ") + what + " has no " + across + " insets"
                             : std::string();
    // each is checked against the size before the two are added, so nothing
    // overflows
    if (lead >= size || trail >= size || lead + trail >= size)
        return std::string("the ") + across + " insets leave no " + unit + " between them to repeat (the stamp is " +
               std::to_string(size) + " cells " + extent + ")";
    return {};
}

} // namespace

std::string sliceProblem(const CellBlock& block, const StampSlice& slice) {
    if (block.empty())
        return "the stamp is empty";
    if (!slice.width && !slice.height)
        return "the slice resizes in neither width nor height";
    if (std::string why = axisProblem(block.width, slice.left, slice.right, slice.width, "width", "left and right", "column", "wide");
        !why.empty())
        return why;
    if (std::string why = axisProblem(block.height, slice.top, slice.bottom, slice.height, "height", "top and bottom", "row", "tall");
        !why.empty())
        return why;
    if (blockHoldsPair(block))
        return "a resizable stamp can't hold a wide character, which a repeat could cut in two";
    return {};
}

int sliceMinimumWidth(const CellBlock& block, const StampSlice& slice) {
    return slice.width ? std::max(1, slice.left + slice.right) : block.width;
}

int sliceMinimumHeight(const CellBlock& block, const StampSlice& slice) {
    return slice.height ? std::max(1, slice.top + slice.bottom) : block.height;
}

int sliceMaximumWidth(const CellBlock& block, const StampSlice& slice) {
    return slice.width ? kMaxColumns : block.width;
}

int sliceMaximumHeight(const CellBlock& block, const StampSlice& slice) {
    return slice.height ? kMaxRows : block.height;
}

namespace {

bool quarterTurn(Transform t) { return t == Transform::RotateClockwise || t == Transform::RotateCounterClockwise; }

// The limits above for a block `w` x `h` cells. A canvas's limits hold on
// an axis that doesn't resize too: a band turned on its side may be longer
// across than a canvas is wide.
bool sizeFits(int w, int h, const StampSlice& slice, int width, int height) {
    const int wMin = slice.width ? std::max(1, slice.left + slice.right) : w;
    const int hMin = slice.height ? std::max(1, slice.top + slice.bottom) : h;
    const int wMax = slice.width ? kMaxColumns : w, hMax = slice.height ? kMaxRows : h;
    return width >= wMin && height >= hMin && width <= wMax && height <= hMax && width <= kMaxColumns &&
           height <= kMaxRows;
}

// `block` cut through `slice` to width x height, unchecked: drawFrame's cut
// with the middle drawn, but with no frame style's source limit, since a
// stamp may be as large as a canvas. An axis that doesn't resize repeats its
// whole length once.
CellBlock cut(const CellBlock& block, const StampSlice& slice, int width, int height) {
    const int midW = block.width - slice.left - slice.right, midH = block.height - slice.top - slice.bottom;
    CellBlock out(width, height);
    out.pixelLayer = block.pixelLayer;
    for (int y = 0; y < height; ++y) {
        const int sy = frameSourceIndex(y, height, slice.top, midH, slice.bottom);
        for (int x = 0; x < width; ++x)
            out.at(x, y) = block.at(frameSourceIndex(x, width, slice.left, midW, slice.right), sy);
    }
    return out;
}

} // namespace

std::optional<CellBlock> resizeStamp(const CellBlock& block, const StampSlice& slice, int width, int height) {
    if (!sliceProblem(block, slice).empty() || !sizeFits(block.width, block.height, slice, width, height) ||
        block.width > kMaxColumns || block.height > kMaxRows)
        return std::nullopt;
    return cut(block, slice, width, height);
}

std::optional<CellBlock> resizeTurnedStamp(const CellBlock& block, const StampSlice& slice,
                                           const std::vector<Transform>& turns, int width, int height) {
    if (!sliceProblem(block, slice).empty() || block.width > kMaxColumns || block.height > kMaxRows)
        return std::nullopt;
    StampSlice shown = slice;
    bool side = false;
    for (const Transform t : turns) {
        shown = transformedSlice(shown, t);
        side ^= quarterTurn(t);
    }
    // the limits on the size as it shows; the cut before the turns may be
    // taller than a canvas is wide, or wider than one is tall
    if (!sizeFits(side ? block.height : block.width, side ? block.width : block.height, shown, width, height))
        return std::nullopt;
    CellBlock out = cut(block, slice, side ? height : width, side ? width : height);
    for (const Transform t : turns)
        out = transformed(out, t);
    return out;
}

namespace {

// What a list of turns does, which is all a replay of it depends on: where
// the cells go, one of the eight orientations (`r` quarter turns clockwise,
// then a horizontal flip when `f`), and how far round its 4-cycle a bracket
// glyph has stepped (`z`). The glyphs keep the dihedral relations the cells
// do (FH FH, FV FV, R R R R and R L are none, FH FV = FV FH = R R, FH R =
// L FH) but for the brackets, guillemets and ≤ ≥: CP437 has no turned
// bracket, so selection.cpp walks ( [ ) ], < « > » and { ≤ } ≥ a step
// round per quarter turn and two per horizontal flip, and leaves them
// under a vertical one. That is `z`; test_stamp_slice.cpp checks both on
// all 256 glyphs and that every state followed by every turn lands on the
// state addTurn keeps.
struct TurnState {
    int r = 0, z = 0;
    bool f = false;
    int index() const { return (r * 2 + int(f)) * 4 + z; }
};

TurnState after(TurnState s, Transform t) {
    switch (t) {
    case Transform::FlipHorizontal: s.f = !s.f; s.z += 2; break;
    case Transform::FlipVertical: s.r += 2; s.f = !s.f; break;  // FV = R R FH
    case Transform::RotateClockwise: s.r += s.f ? 3 : 1; s.z += 1; break;
    case Transform::RotateCounterClockwise: s.r += s.f ? 1 : 3; s.z += 3; break;
    }
    s.r %= 4;
    s.z %= 4;
    return s;
}

// The shortest list of turns for each state, found breadth first: sixteen
// are reachable, none takes more than kMaxTurns.
const std::array<std::optional<std::vector<Transform>>, 32>& shortestTurns() {
    static const auto table = [] {
        std::array<std::optional<std::vector<Transform>>, 32> out;
        std::vector<TurnState> queue = {TurnState{}};
        out[0] = std::vector<Transform>{};
        for (size_t i = 0; i < queue.size(); ++i)
            for (const Transform t : {Transform::FlipHorizontal, Transform::FlipVertical, Transform::RotateClockwise,
                                      Transform::RotateCounterClockwise}) {
                const TurnState next = after(queue[i], t);
                if (out[size_t(next.index())])
                    continue;
                std::vector<Transform> turns = *out[size_t(queue[i].index())];
                turns.push_back(t);
                out[size_t(next.index())] = std::move(turns);
                queue.push_back(next);
            }
        return out;
    }();
    return table;
}

} // namespace

void addTurn(std::vector<Transform>& turns, Transform t) {
    TurnState s;
    for (const Transform u : turns)
        s = after(s, u);
    turns = *shortestTurns()[size_t(after(s, t).index())];
}

StampSlice transformedSlice(const StampSlice& s, Transform t) {
    StampSlice r = s;
    switch (t) {
    case Transform::FlipHorizontal: std::swap(r.left, r.right); break;
    case Transform::FlipVertical: std::swap(r.top, r.bottom); break;
    case Transform::RotateClockwise:
        // the top edge turns to the right, the left to the top
        r = {s.bottom, s.top, s.left, s.right, s.height, s.width};
        break;
    case Transform::RotateCounterClockwise:
        // the top edge turns to the left, the right to the top
        r = {s.top, s.bottom, s.right, s.left, s.height, s.width};
        break;
    }
    return r;
}

} // namespace bw
