#pragma once
// Ramp gradient (PLAN §10, "Shading"): a linear or radial fill along a
// ramp's ladder, cell-banded or with a 4×4 Bayer dither along its band
// edges, which the speck rule leaves out (lint::ditherExempt).

#include "blockwright/brush.h"
#include "blockwright/edit.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"

#include <vector>

namespace bw {

enum class GradientShape { Linear, Radial };
// How the rungs blend: each a band of whole cells, or the same bands with
// their edges mixed in a 4×4 Bayer order (kBayer4).
enum class GradientBanding { Cells, Bayer };

struct Gradient {
    GradientShape shape = GradientShape::Linear;
    // Cells: a linear gradient runs from the start cell to the end cell, a
    // radial one from the centre (the start) out to the end cell. A row
    // counts as two columns, so a radial gradient is round on screen.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    // The rungs at the start and at the end, either way round, clamped to
    // the ramp.
    int fromRung = 0, toRung = 0;
    GradientBanding banding = GradientBanding::Cells;
};

// The 4×4 Bayer order: cell (x, y) of the canvas takes kBayer4[y % 4][x % 4].
inline constexpr int kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
// Where a solid rung's plateau meets a neighbouring rung, the Bayer banding
// gives the plateau's cell next to it the neighbour's rung where its order is
// below kDitherNear, the cell beyond that where it is below kDitherFar, and
// the neighbour's cell next to the plateau the plateau's rung where its order
// is below kDitherBand: across the edge the neighbour's share runs 2, 6, 12
// and 16 sixteenths.
inline constexpr int kDitherFar = 2, kDitherNear = 6, kDitherBand = 4;

// Each ░▒▓ rung of a gradient is this many columns wide along it (a row
// counting as two): two cells across a row, a row down a column.
inline constexpr double kGradientBand = 2.0;

// The rung each cell of a width × height canvas takes (row-major), or empty
// when the start and the end are the same cell, or with `ice` off when the
// first rung can't be written. A cell's place is its distance along the
// gradient (its projection on the start-to-end line for a linear one, from
// the centre for a radial one), mapped onto the ladder from fromRung to
// toRung with every rung in between in order. Each ░▒▓ rung is kGradientBand
// wide and the solid rungs share the rest of the span as plateaus (as hand
// shading does: terrace a wide gradient, the turn squeezed to 1-3 cells), or
// with no solid rung between the ends the end rungs take it as planes, so
// no rung is skipped and no shade is wider than the lint allows. A radial
// gradient's plateaus take whole bands, so each ring ends on an even radius,
// and what that leaves over goes to the last rung. A span too short for the
// ladder at that width runs the ladder on past the end cell rather than skip
// a rung. Before the start and past the ladder's end the end rungs go on as
// planes. With `ice` off the ladder stops at the last rung short of one that
// needs a bright background (a shade of two bright colours), as stepRung
// does. Where the ladder's shades would all show one glyph (steel's 3 to 5:
// each shade written the way round that keeps its background dark, both ▓)
// one more rung goes on past the end, or else before the start. A cell left
// a speck (all four neighbours on one side of it, the first ring of a radial
// gradient centred a column off the canvas) takes its nearest neighbour's
// rung. The ends may be anywhere an int reaches. With the Bayer banding the
// band edges are dithered as rampGradient dithers them.
std::vector<int> gradientRungs(const Ramp& ramp, const Gradient& g, int width, int height, bool ice = true);

// The gradient over every cell the options let a stroke paint (the
// selection, the alpha lock), each written with rungCell, as one edit; cells
// that come out as they were are left out. The options' symmetry isn't
// read: the view mirrors the edit (CanvasView::mirrored). Where the fill
// meets cells it doesn't paint, it is fitted to them as the lint reads
// them: a field further along the ramp beside the plane of the last rung
// (or the first) carries the ladder on to the next solid rung or the
// field's rung, whichever is nearer; a jump into a field that would be a
// skip (the fill going on the same way) pulls the fill toward the field's
// rung, a rung a cell in from the edge; and a speck against the cells
// round it takes its nearest neighbour's rung. In an area too narrow for
// the climb the climb is all the fill shows. What that can't clear is left:
// cells between two fields too far apart in rung for both pulls, a run of
// the field itself that the fill makes a step, with iCE off a field the
// ladder can't reach without a bright background, and a band or ring of the
// gradient's own that meets the climb at a slant to the edge (a radial fill
// the edge cuts, a linear one at an angle to it) can join the climb's run of
// the same rung in an L that the lint reads as a wide rung: rare in a small
// box, common along a long edge. A linear fill square to the edges makes
// none.
//
// With the Bayer banding the cell-banded fill's band edges are then mixed:
// where a painted cell of a solid rung (not black) has a painted cell of the
// next rung up or down one or two cells off along its row or column (only
// its own rung between), it takes that rung where its Bayer order is below
// kDitherNear (one off) or kDitherFar (two off), and a painted shade cell
// beside a solid rung's cell (not black) takes the solid where it is below
// kDitherBand. A cell keeps its new rung only where every neighbour it has on
// the ladder is one rung from it (no skip), it can be written (iCE off), and
// the ladder lint finds nothing the cell-banded fill didn't have but the
// dither's own specks (StrokeLint, the dither marked): a finding sends back
// every dithered cell within a cell of it, until none is left. The dithered
// cells carry the dither mark (Cell::dithered), every other cell the fill
// writes none, and the edit is CellEdit::dithers. So a dithered fill shows
// the lint no specks but those of its dither (lint::ditherExempt).
//
// With `added`, it is set to what the edit adds to the lint's findings
// on the canvas (StrokeLint's judgement), empty when clean.
CellEdit rampGradient(const Canvas& canvas, const Ramp& ramp, const Gradient& g, const StrokeOptions& options = {},
                      std::vector<lint::LadderFinding>* added = nullptr);

// How many cells the gradient's passes have visited on this thread so far
// (each cell of each pass over the cells it lays out, and each step of a
// walk along a line): a work counter, for tests of what a fill costs.
long long gradientCellsVisited();

} // namespace bw
