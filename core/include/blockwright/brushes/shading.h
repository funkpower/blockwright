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
// With `ice` off the cell is read as it shows (a bright background is its
// dark colour, blinking: off the ramp), and a rung that needs a bright
// background either way round (a shade of two bright neighbours) is never
// written: the step stops at the last rung short of the target that
// doesn't, and is empty if there is none.
std::optional<Cell> stepRung(const Ramp& ramp, const Cell& cell, int steps, bool ice = true);

enum class ShadeDirection { Lighten, Darken };

// Ramp shade brush: each pass lightens or darkens every cell under it by one
// rung (or `steps` rungs) along the ramp. Strength (pressure, with
// PressureTo::Strength) scales the step count, rounded and never below one
// rung, so it changes nothing at the default of one step: there is no
// randomness or partial step. Cells off the ramp are left alone. With the
// stroke's iCE colours off it writes no cell that needs a bright background
// (stepRung).
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
