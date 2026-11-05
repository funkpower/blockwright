#include "blockwright/irc_render.h"

#include <algorithm>

namespace bw {

QImage renderIrcView(const irc::View& view, int glyphWidth, int scale) {
    QImage img = renderCanvas(view.canvas, glyphWidth, scale);
    const int gh = vgaFont().height * scale, gw = glyphWidth * scale;
    // a quarter of a glyph wide, at least one pixel
    const int bar = std::max(1, gw / 4);
    for (int y = 0; y < static_cast<int>(view.rows.size()) && y < view.canvas.height(); ++y) {
        const irc::ViewRow& r = view.rows[std::size_t(y)];
        if (!r.cut())
            continue;
        // at the first lost cell, or at the right edge when every cell the
        // view shows is sent and the cells lost lie past its width (#364)
        const int x0 = r.shown < view.canvas.width() ? r.shown * gw : img.width() - bar;
        for (int py = y * gh; py < (y + 1) * gh; ++py) {
            auto* line = reinterpret_cast<QRgb*>(img.scanLine(py));
            std::fill(line + x0, line + std::min(img.width(), x0 + bar), kIrcCutMark);
        }
    }
    return img;
}

} // namespace bw
