#include "blockwright/brushes/shading.h"

#include <algorithm>
#include <cmath>

namespace bw {

std::optional<Cell> stepRung(const Ramp& ramp, const Cell& cell, int steps) {
    const auto rung = rungOf(ramp, cell);
    if (!rung)
        return std::nullopt;
    // saturates: a huge step count goes to the end of the ramp
    const int next = static_cast<int>(std::clamp(static_cast<long long>(*rung) + steps, 0LL, (long long)ramp.topRung()));
    if (next == *rung)
        return std::nullopt;
    // keep the way round the artist wrote the shade, where the ramp allows
    return rungCell(ramp, next, shadeForm(ramp, cell).value_or(ShadeForm::LightOnTop));
}

RampShadeBrush::RampShadeBrush(Ramp ramp, ShadeDirection direction, int steps)
    : ramp_(std::move(ramp)), direction_(direction), steps_(std::max(1, steps)) {}

std::optional<Cell> RampShadeBrush::paintCell(const Stroke& stroke, int x, int y, double strength) const {
    const auto cell = stroke.cellAt(x, y);
    if (!cell)
        return std::nullopt;
    const int n = std::max(1, static_cast<int>(std::lround(steps_ * strength)));
    return stepRung(ramp_, *cell, direction_ == ShadeDirection::Lighten ? n : -n);
}

} // namespace bw
