#include "blockwright/brushes/shading.h"

#include "blockwright/ansi.h"

#include <algorithm>
#include <cmath>

namespace bw {

std::optional<Cell> stepRung(const Ramp& ramp, const Cell& cell, int steps, bool ice) {
    // with iCE off a bright background shows as its dark colour, blinking:
    // off the ladder
    const Cell shown = ice ? cell : iceCell(cell, false);
    const auto rung = rungOf(ramp, shown);
    if (!rung)
        return std::nullopt;
    // saturates: a huge step count goes to the end of the ramp
    const int target =
        static_cast<int>(std::clamp(static_cast<long long>(*rung) + steps, 0LL, (long long)ramp.topRung()));
    // keep the way round the artist wrote the shade, where the ramp allows
    const ShadeForm form = shadeForm(ramp, shown).value_or(ShadeForm::LightOnTop);
    // With iCE off a rung that needs a bright background either way round
    // (two bright neighbours) can't be written: stop at the last one short
    // of the target that can, or leave the cell.
    const int back = target > *rung ? -1 : 1;
    for (int next = target; next != *rung; next += back) {
        const Cell out = rungCell(ramp, next, form);
        if (ice || out.bg.paletteIndex() < 8)
            return out;
    }
    return std::nullopt;
}

RampShadeBrush::RampShadeBrush(Ramp ramp, ShadeDirection direction, int steps)
    : ramp_(std::move(ramp)), direction_(direction), steps_(std::max(1, steps)) {}

std::optional<Cell> RampShadeBrush::paintCell(const Stroke& stroke, int x, int y, double strength) const {
    const auto cell = stroke.originalAt(x, y);
    if (!cell)
        return std::nullopt;
    const int n = std::max(1, static_cast<int>(std::lround(steps_ * strength)));
    return stepRung(ramp_, *cell, direction_ == ShadeDirection::Lighten ? n : -n, stroke.options().ice);
}

} // namespace bw
