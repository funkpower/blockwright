#include "blockwright/brushes/drawing.h"

#include "blockwright/halfblock.h"
#include "blockwright/ramp.h"
#include "blockwright/selection.h"

#include <algorithm>
#include <array>

namespace bw {

std::optional<int> shadeLevel(Glyph g) {
    const std::optional<char32_t> ch = g.codePoint();
    if (!ch)
        return std::nullopt;
    switch (*ch) {
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
    const auto cell = stroke.originalAt(x, y);
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
    out.attrs = cell->attrs & attr::Formatting;  // shading keeps the formatting (#360)
    return out;
}

char32_t dabGlyph(char32_t ch, const Dab& d) {
    if (d.flipX)
        ch = flipGlyph(ch, Flip::Horizontal);
    if (d.flipY)
        ch = flipGlyph(ch, Flip::Vertical);
    for (int i = 0; i < d.turns; ++i)
        ch = rotateGlyph(ch, true);
    return ch;
}

bool pixelsHoldGlyph(char32_t ch) {
    Cell c;
    c.ch = ch;
    return halfBlocks(c).has_value();
}

std::vector<char32_t> pixelsUnheldGlyphs(char32_t ch, const Symmetry& symmetry, int width, int height) {
    if (!pixelsHoldGlyph(ch))
        return {ch};
    std::vector<char32_t> out;
    for (const Dab& d : symmetryDabs(symmetry, Dab{width / 2, height / 2}, width, height)) {
        const char32_t g = dabGlyph(ch, d);
        if (!pixelsHoldGlyph(g) && std::find(out.begin(), out.end(), g) == out.end())
            out.push_back(g);
    }
    return out;
}

std::optional<Cell> GlyphBrush::paintCell(const Stroke& stroke, int, int, double) const {
    // a mirrored image of the stroke writes the glyph mirrored, and a
    // radial image a quarter turn round writes it turned
    Cell c = cell_;
    if (const std::optional<char32_t> cp = c.ch.codePoint())  // a brush holds a narrow glyph
        c.ch = dabGlyph(*cp, stroke.currentDab());
    return c;
}

std::optional<Cell> ColorizeBrush::paintCell(const Stroke& stroke, int x, int y, double) const {
    auto cell = stroke.originalAt(x, y);
    if (!cell)
        return std::nullopt;
    if (fg_)
        cell->fg = *fg_;
    if (bg_)
        cell->bg = *bg_;
    return cell;
}

} // namespace bw
