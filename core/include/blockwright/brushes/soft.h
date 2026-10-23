#pragma once
// Soft shading brush (PLAN §10, "Shading"): the ramp shade brush with a
// falloff, banded along the ladder rather than dithered.

#include "blockwright/brush.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/ramp.h"

#include <array>
#include <utility>
#include <vector>

namespace bw {

// The rungs a soft dab moves the cells of its footprint, as (cell, rungs)
// pairs in the footprint's order. `wanted` rungs is what the middle moves:
// its strength's share of a whole colour (four rungs), as the ramp shade
// brush, never less than one. From the tip's own edge in, the cells move a
// rung, then two, and so on up to the depth: each band a column in from
// either end of the row and a row in from the top or bottom of the column,
// so a band is a cell wide wherever it runs and a neighbour is never more
// than a band away. Each band's corner cell drops to the band outside it,
// so the outermost band leaves its corners out (they are not listed),
// except where the tip runs three cells or fewer one way, and a tip that
// is all corners (two by two) keeps them. No cell moves
// further than all four of its neighbours (that would be a speck). Hardness
// (0-1) is the share of the bands the falloff doesn't use: a soft tip
// spreads all of them, a harder one stops its depth short of `wanted`
// sooner, and a hard tip (1) moves every cell of the footprint `wanted`
// rungs.
std::vector<std::pair<std::pair<int, int>, int>> softFalloff(const Dab& d, int size, Tip tip, int width,
                                                             int height, double strength, double hardness);

// Soft brush: lightens (or darkens) the cells under it along the ramp like
// the ramp shade brush, the most in the middle of the stroke and less
// towards its edge. The falloff is the one softFalloff gives a tip, taken
// over the whole stroke so far: each cell wants the depth of the strongest
// dab that covered it, and moves no further than its band in the stroke's
// own shape, so overlapping dabs and bends band as one tip would. After
// every dab the stroke is shaded again round the box of its footprint: the
// cells within 9 of it take their wants and the 4 beyond settle, judged
// with 13 more cells of context, so the solve's work is bounded by the
// dab's size, not the stroke's or the canvas's; each mirror image gets a
// grid of its own, and no dab copies the canvas. A stroke never takes paint
// off: lightening, no cell is left darker than it has been in the stroke
// (darkening, no lighter), and a dab that wants nothing deeper than the
// stroke already does is skipped, so going over a stroke again at the same
// or a lighter press changes nothing. So whichever of two overlapping dabs
// lands first keeps its paint, and dabs played in another order can leave
// other tones. Under a mirror a dab's images are shaded in one solve and a
// cell that gives way takes its images with it, so a stroke on a canvas
// symmetric about the mirror stays symmetric.
//
// After any dab the canvas has no ladder lint finding that the stroke's
// start didn't have (StrokeLint, the lint's own rule for what a change
// adds: by kind and place, a wide rung unless the start had one it shares
// cells with, one glyph and noise over its limit as the whole canvas
// reads). The guard reads every row and column through the cells the dab
// would change, through one lint index kept over the stroke. Neighbours
// on the stroke are never more than a rung apart (so a pressure change
// ramps, even on a hard tip), and a cell left in a new finding, by its tones as they stand,
// gives way a rung at a time, the one that moved more first. A dab whose
// cells can't settle without a new finding gives way entirely: every cell
// stays where the last dab left it (a size 1 dab on a flat field, which
// would be a speck; two cells on a gradient a rung a column). The brush
// gives way cell by cell from the whole footprint and doesn't search for
// the largest clean part of it, so it can leave a dab blank that a smaller
// change would have taken. Cells off the ramp are left alone; with the
// stroke's iCE colours off a cell moves only as far as the rungs it can
// write (no bright background) run unbroken from its own, so it never
// jumps one.
class SoftShadeBrush : public Brush {
public:
    SoftShadeBrush(Ramp ramp, ShadeDirection direction, double hardness = 0.0);

    std::string_view name() const override { return "soft-shade"; }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override;

    const Ramp& ramp() const { return ramp_; }
    double hardness() const { return hardness_; }

    // Shade the whole stroke again after every dab rather than the cells
    // round it: slower, and the same result but for rare cells at the edge
    // of those the near solve settles (a test pins the known ones). Only
    // tests use it, as the reference for the near solve.
    void reshadeWhole(bool on) { whole_ = on; }
    // The cells soft dabs have read or written on this thread (grid copies,
    // band and ramp passes, solve passes and rung reads; the guard's lint
    // reads count in lint::cellsRead()), to bound a dab's work in tests.
    static long long cellsVisited();

private:
    struct State;
    // How many of `want` rungs the cell can move.
    int reach(const Cell& cell, int want, bool ice) const;
    // Shades the stroke round the boxes (x0, y0, x1, y1) of a dab's
    // footprints, one for each of its images under a mirror, or the whole
    // stroke when `whole`; false when it changes no cell.
    bool shade(Stroke& stroke, State& state, const std::vector<std::array<int, 4>>& around, bool whole) const;

    Ramp ramp_;
    ShadeDirection direction_;
    double hardness_;
    bool whole_ = false;
};

} // namespace bw
