#pragma once
// Despeckle brush (PLAN §10, "Shading"): a majority filter that clears the
// specks the lint's speckle rule finds (lint::checkLadder).

#include "blockwright/brush.h"
#include "blockwright/ramp.h"

#include <functional>
#include <optional>
#include <vector>

namespace bw {

// The rung a speck at x, y would take, or empty if it isn't one. `rungAt`
// gives a cell's rung on the ramp, empty off the ladder or off the canvas.
// A speck is the lint's: a ladder cell, not black (ink) and not the ramp's
// top rung (a glint), with three or more neighbours on the ladder and
// lighter than every one of them or darker than every one. It takes the
// rung most of those neighbours share; on a tie the nearest to its own, and
// of two as near the darker.
std::optional<int> speckFix(const Ramp& ramp, int x, int y,
                            const std::function<std::optional<int>(int, int)>& rungAt);
// Every rung a speck could take, its neighbours' rungs in that order (the
// most shared first, then the nearest, then the darker), past the first
// leaving out the far end of theirs (the highest for a dark speck, the
// lowest for a light one) when only one neighbour holds it (it would leave
// a hole or a spike); empty if it isn't a speck. speckFix is the first.
std::vector<int> speckFixes(const Ramp& ramp, int x, int y,
                            const std::function<std::optional<int>(int, int)>& rungAt);

// Despeckle brush: each speck under it takes the rung of the neighbours
// round it, copied from the neighbour that has it (below first, then above,
// left and right), so a lone light or dark cell, or a stray shade, joins
// the field it sits in. Cells are read as the stroke has left them, by rows
// from the top and left to right, so a cluster clears from the first speck
// in; a flipped tip reads them, and picks its neighbours, turned over.
// Under a mirror a dab's images are cleaned together: a cell and its images
// are judged from the same cells and set together, each image read as the
// mirror of the dab, so a stroke on a canvas symmetric about the mirror
// stays symmetric. Each dab goes over its cells again until none of them is
// a speck, so one pass leaves no speck under the brush but those whose fix
// the guard refuses: a fix that would add a lint finding the stroke's
// start didn't have (StrokeLint: a skip or a wide rung the speck's new
// rung makes, a canvas left in one glyph, noise over the limit) gives way
// to the speck's next rung (speckFixes), and is left undone when every one
// would. Nothing else changes: cells off the ramp, black, the top rung,
// and cells that aren't specks are left alone. With iCE colours off a cell is read as it shows (a
// bright background is off the ladder).
class DespeckleBrush : public Brush {
public:
    explicit DespeckleBrush(Ramp ramp);

    std::string_view name() const override { return "despeckle"; }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override;

    const Ramp& ramp() const { return ramp_; }

private:
    Ramp ramp_;
};

} // namespace bw
