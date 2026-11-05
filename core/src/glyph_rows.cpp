#include "blockwright/glyph_rows.h"

#include "blockwright/cell.h"
#include "blockwright/pc_fonts.h"

#include <algorithm>
#include <cstddef>

namespace bw {

GlyphRows vgaRows(std::uint8_t g, int gw) {
    const auto font = ibmVga8x16();
    const bool lineDrawing = g >= 0xC0 && g <= 0xDF;
    GlyphRows rows{};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < gw; ++x)
            if (font[std::size_t(g) * 16 + std::size_t(y)] & (0x80 >> std::min(x, 7)) && (x < 8 || lineDrawing))
                rows[std::size_t(y)] |= std::uint16_t(1u << x);
    return rows;
}

GlyphRows formatRows(GlyphRows rows, std::uint8_t attrs, int gw, int gh) {
    gh = std::min(gh, 16);
    const std::uint16_t mask = std::uint16_t((1u << gw) - 1);
    for (int y = 0; y < gh; ++y) {
        std::uint16_t& row = rows[std::size_t(y)];
        if (attrs & attr::Bold)
            row = std::uint16_t(row | row << 1);
        if (attrs & attr::Italic)
            row = std::uint16_t(row << ((gh - 1 - y) / 4));
        row &= mask;
    }
    if (attrs & attr::Underline)
        rows[std::size_t(gh - 1)] = mask;
    return rows;
}

} // namespace bw
