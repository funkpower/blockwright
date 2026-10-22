#include "blockwright/brushes/drawing.h"

#include "blockwright/ramp.h"

#include <algorithm>
#include <array>

namespace bw {

std::optional<int> shadeLevel(char32_t ch) {
    switch (ch) {
    case U' ':
    case U'\0':
    case U' ': return 0;
    case kShadeLight: return 1;
    case kShadeMedium: return 2;
    case kShadeDark: return 3;
    case kFullBlock: return 4;
    default: return std::nullopt;
    }
}

std::optional<Cell> ShadeBrush::paintCell(const Stroke& stroke, int x, int y, double) const {
    static constexpr std::array<char32_t, 5> kLevels = {U' ', kShadeLight, kShadeMedium, kShadeDark, kFullBlock};
    const auto cell = stroke.cellAt(x, y);
    if (!cell)
        return std::nullopt;
    // Only the ladder steps: any other glyph (text, half-block pixels) is
    // left as it is, so a pass doesn't wipe what it can't step.
    const auto level = shadeLevel(cell->ch);
    if (!level || (reduce_ && *level == 0))
        return std::nullopt;
    const int next = reduce_ ? *level - 1 : std::min(4, *level + 1);
    Cell out;
    out.ch = kLevels[static_cast<size_t>(next)];
    out.fg = pen_.fg;
    out.bg = pen_.bg;
    return out;
}

std::optional<Cell> GlyphBrush::paintCell(const Stroke&, int, int, double) const { return cell_; }

std::optional<Cell> ColorizeBrush::paintCell(const Stroke& stroke, int x, int y, double) const {
    auto cell = stroke.cellAt(x, y);
    if (!cell)
        return std::nullopt;
    if (fg_)
        cell->fg = *fg_;
    if (bg_)
        cell->bg = *bg_;
    return cell;
}

} // namespace bw
