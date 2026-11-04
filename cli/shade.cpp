#include "shade.h"

#include "read_file.h"

#include "blockwright/brush.h"
#include "blockwright/brushes/despeckle.h"
#include "blockwright/brushes/detail.h"
#include "blockwright/selection.h"

#include <algorithm>
#include <memory>

namespace bwcli {

namespace {

// The widest odd tip: an odd square tip centres on its dab.
constexpr int kWidestOddTip = bw::kMaxBrushSize % 2 ? bw::kMaxBrushSize : bw::kMaxBrushSize - 1;

// One full-strength stroke of a square tip over the box: a dab sized to
// cover it, or a dab a kWidestOddTip square, rows from the top, when it is
// wider or taller than that.
bw::CellEdit coverBox(const bw::Canvas& c, const bw::Brush& brush, const bw::lint::Box& box, bw::Compass light,
                      const bw::StrokeOptions& so) {
    const int bw_ = box.x1 - box.x0 + 1, bh = box.y1 - box.y0 + 1;
    int size = std::min(std::max(bw_, bh) | 1, kWidestOddTip);
    bw::BrushSettings s{size, bw::Tip::Square, 1.0, bw::PressureTo::Nothing, light};
    bw::Stroke stroke(c, brush, s, so);
    const int half = size / 2;
    for (int y = box.y0; y <= box.y1; y += size)
        for (int x = box.x0; x <= box.x1; x += size)
            // centred on the square's middle where the box is smaller
            stroke.addDab(bw::Dab{x + std::min(half, (box.x1 - x) / 2), y + std::min(half, (box.y1 - y) / 2)}, false);
    return stroke.edit();
}

} // namespace

bw::Mask maskOf(const bw::Canvas& c) {
    bw::Mask m(c.width(), c.height());
    for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width(); ++x)
            if (!bw::isBlank(c.at(x, y)))
                m.set(x, y);
    return m;
}

Shaded shadeDocument(bw::AnsiDocument& doc, const ShadeOptions& o) {
    bw::Canvas& c = doc.canvas;
    Shaded r;
    if (o.mask && (o.mask->width() != c.width() || o.mask->height() != c.height()))
        throw Failure{"the mask is " + std::to_string(o.mask->width()) + "x" + std::to_string(o.mask->height()) +
                      ", the canvas " + std::to_string(c.width()) + "x" + std::to_string(c.height())};
    r.box = o.box.value_or(bw::lint::Box{0, 0, c.width() - 1, c.height() - 1});
    if (r.box.x0 >= c.width() || r.box.y0 >= c.height())
        throw Failure{"the box " + std::to_string(r.box.x0) + "," + std::to_string(r.box.y0) + "," +
                      std::to_string(r.box.x1) + "," + std::to_string(r.box.y1) + " is off the " +
                      std::to_string(c.width()) + "x" + std::to_string(c.height()) + " canvas"};
    r.box.x1 = std::min(r.box.x1, c.width() - 1);
    r.box.y1 = std::min(r.box.y1, c.height() - 1);

    // The selection: the box, and the mask's cells in it. Its bounds are the
    // box the pass covers and the check reads.
    auto sel = std::make_shared<bw::Mask>(
        bw::Mask::rect(c.width(), c.height(), r.box.x0, r.box.y0, r.box.x1, r.box.y1));
    if (o.mask) {
        bw::lint::Box bounds{c.width(), c.height(), -1, -1};
        for (int y = r.box.y0; y <= r.box.y1; ++y)
            for (int x = r.box.x0; x <= r.box.x1; ++x) {
                const bool in = o.mask->contains(x, y);
                sel->set(x, y, in);
                if (in) {
                    bounds.x0 = std::min(bounds.x0, x), bounds.y0 = std::min(bounds.y0, y);
                    bounds.x1 = std::max(bounds.x1, x), bounds.y1 = std::max(bounds.y1, y);
                }
            }
        if (bounds.x1 >= 0)
            r.box = bounds;
    }

    // A piece that was written without loss and is lossy once shaded only
    // can be because --ice turned iCE colours on, where blink can't be kept.
    const bw::AnsiOutput before = bw::writeAnsi(doc, o.encoding);
    const bool flag = doc.ice;
    bw::openAsEditor(doc);
    if (o.ice)
        doc.ice = true;
    bw::StrokeOptions so;
    so.selection = sel;
    so.ice = doc.ice;
    bw::CellEdit edit;
    switch (o.mode) {
    case ShadeMode::Step:
        edit = bw::paintRegion(c, bw::RampShadeBrush(o.ramp, o.direction, o.steps), {}, so);
        break;
    case ShadeMode::Form:
        edit = coverBox(c, bw::FormShadeBrush(o.ramp, bw::FormOptions{o.form, false, o.glint}), r.box, o.light, so);
        break;
    case ShadeMode::Despeckle:
        edit = coverBox(c, bw::DespeckleBrush(o.ramp), r.box, o.light, so);
        break;
    case ShadeMode::Rim:
        edit = coverBox(c, bw::RimLightBrush(o.ramp, o.light), r.box, o.light, so);
        break;
    }
    r.edit = bw::settleMarks(c, bw::settleWide(c, std::move(edit)));
    r.edit.apply(c);
    r.blinkDropped = bw::lostBlinkCount(doc);
    r.iceTurnedOn = !flag && doc.ice;
    if (before.unwritable.empty() && !before.blinkDropped) {
        const int n = bw::writeAnsi(doc, o.encoding).blinkDropped;
        if (n)
            r.why = "--ice turns iCE colours on, and " + std::to_string(n) +
                    (n == 1 ? " blinking cell would lose its blink" : " blinking cells would lose their blink");
    }
    return r;
}

} // namespace bwcli
