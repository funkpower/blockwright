#pragma once
// Shading brushes (PLAN §10, "Shading"). They walk a named ramp along the
// ladder A → B░A → B▒A → B▓A → B and never write a shade of two colours that
// aren't ramp neighbours.

#include "blockwright/brush.h"
#include "blockwright/ramp.h"

namespace bw {

// The cell `steps` rungs up the ramp (down for a negative count), clamped to
// the ramp's ends (any int, INT_MIN and INT_MAX included). A shade keeps the
// way round it was written unless that would make the background bright
// where the other way wouldn't (see rungCell). Empty when the cell isn't on the ramp (text, half blocks,
// colours the ramp doesn't have, a shade of two non-neighbours) or the step
// changes nothing.
std::optional<Cell> stepRung(const Ramp& ramp, const Cell& cell, int steps);

enum class ShadeDirection { Lighten, Darken };

// Ramp shade brush: each pass lightens or darkens every cell under it by one
// rung (or `steps` rungs) along the ramp. Strength (pressure, with
// PressureTo::Strength) scales the step count, rounded and never below one
// rung, so it changes nothing at the default of one step: there is no
// randomness or partial step. Cells off the ramp are left alone.
class RampShadeBrush : public Brush {
public:
    RampShadeBrush(Ramp ramp, ShadeDirection direction, int steps = 1);

    std::string_view name() const override { return "ramp-shade"; }
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

    const Ramp& ramp() const { return ramp_; }

private:
    Ramp ramp_;
    ShadeDirection direction_;
    int steps_;
};

} // namespace bw
