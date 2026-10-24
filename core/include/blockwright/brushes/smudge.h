#pragma once
// Smudge brush (PLAN §10, "Shading", blend/smudge): pulls ramp values along
// the stroke and lays them back on the ladder.

#include "blockwright/brush.h"
#include "blockwright/ramp.h"

namespace bw {

// What a smudge lays on a cell at `rung` while it carries `carried` (a
// rung, not rounded): `strength` (0-1) of the way from the cell to what the
// brush carries. It is also what the brush carries on.
double smudgeMix(int rung, double carried, double strength);
// The ladder rung for a smudged value, `from` the cell's rung: the nearest,
// and halfway between two the one further from `from` (the way the value
// was pulled), so a half-strength smudge moves a cell a rung toward a
// neighbour a rung off it.
int smudgeRung(double value, int from);

// Smudge brush: the tip picks up the rungs under it and drags them along
// the stroke. Each cell of the tip carries a value of its own (the tip's
// cells keyed by their place in it), so a wide tip drags a patch, not one
// colour. At every stamp of the stroke, joining lines included, a tip cell
// over a cell of the ramp lays smudgeMix of the two, rounded to a rung
// (smudgeRung) and written with stepRung, and carries that mix on: at full
// strength it drags its value unchanged, at half it fades toward the cells
// it crosses, at none it changes nothing. A tip cell starts by picking up
// the cell under it, laying nothing. Cells are read as the stroke has left
// them (a smudge builds up: going over a stroke again pulls further), with
// iCE colours off as they show (a bright background is off the ladder).
// Black (ink, a seam), cells off the ramp, transparent cells (they show the
// layer beneath), cells the stroke may not paint (outside the selection)
// and the canvas edge stop the smear: the tip cell over one drops what it
// carried and the cell is left, so a smear never crosses an outline.
//
// Under a mirror (X, Y or both) each stamp's images are smudged together
// from the same cells, each image with a tip of its own, a cell several
// images reach taking the mean of what they lay, so a stroke on a canvas
// symmetric about the mirror stays symmetric. A radial image stamps on its
// own, in turn, with its own tip.
//
// The stroke's ladder lint guard (StrokeLint) judges each stamp: a stamp
// whose cells would add a finding the stroke's start didn't have (a skip,
// a wide rung, a speck, a canvas left in one glyph, noise over its limit)
// gives way a cell at a time (with its mirror images), each cell a rung at
// a time back toward where it was, in rows from the top. So the canvas
// after any stamp adds no finding to the stroke's start, the preview is
// what is committed, and what a stamp can't lay cleanly it leaves: a smear
// across a fold whose planes are more than a rung apart (one cell between
// them would be a skip) or into a ladder a rung a cell (the front of the
// pull would skip) lays nothing there. What the tip carries is the mix
// whether the guard took it or not.
class SmudgeBrush : public Brush {
public:
    explicit SmudgeBrush(Ramp ramp);

    std::string_view name() const override { return "smudge"; }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override;

    const Ramp& ramp() const { return ramp_; }

private:
    Ramp ramp_;
};

} // namespace bw
