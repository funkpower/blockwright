#include "shade.h"

#include "read_file.h"

#include "blockwright/brush.h"

#include <algorithm>
#include <memory>

namespace bwcli {

Shaded shadeDocument(bw::AnsiDocument& doc, const ShadeOptions& o) {
    bw::Canvas& c = doc.canvas;
    Shaded r;
    r.box = o.box.value_or(bw::lint::Box{0, 0, c.width() - 1, c.height() - 1});
    if (r.box.x0 >= c.width() || r.box.y0 >= c.height())
        throw Failure{"the box " + std::to_string(r.box.x0) + "," + std::to_string(r.box.y0) + "," +
                      std::to_string(r.box.x1) + "," + std::to_string(r.box.y1) + " is off the " +
                      std::to_string(c.width()) + "x" + std::to_string(c.height()) + " canvas"};
    r.box.x1 = std::min(r.box.x1, c.width() - 1);
    r.box.y1 = std::min(r.box.y1, c.height() - 1);

    // A piece that was written without loss and is lossy once shaded only
    // can be because --ice turned iCE colours on, where blink can't be kept.
    const bw::AnsiOutput before = bw::writeAnsi(doc);
    const bool flag = doc.ice;
    bw::openAsEditor(doc);
    if (o.ice)
        doc.ice = true;
    const bw::RampShadeBrush brush(o.ramp, o.direction, o.steps);
    bw::StrokeOptions so;
    so.selection = std::make_shared<const bw::Mask>(
        bw::Mask::rect(c.width(), c.height(), r.box.x0, r.box.y0, r.box.x1, r.box.y1));
    so.ice = doc.ice;
    r.edit = bw::settleLostBlink(bw::paintRegion(c, brush, {}, so));
    r.edit.apply(c);
    r.blinkDropped = bw::lostBlinkCount(doc);
    r.iceTurnedOn = !flag && doc.ice;
    if (!before.lossy()) {
        const int n = bw::writeAnsi(doc).blinkDropped;
        if (n)
            r.why = "--ice turns iCE colours on, and " + std::to_string(n) +
                    (n == 1 ? " blinking cell would lose its blink" : " blinking cells would lose their blink");
    }
    return r;
}

} // namespace bwcli
