#pragma once
// Detail brushes (PLAN §10, "Detail"): the rim-light brush. It works on a
// shape: the cells on the brush's ramp other than its ground (its first
// colour when that is black), read as they show under the stroke's iCE
// setting, and the half blocks whose halves are ramp colours, or one a ramp
// colour and the other black (the fringe the shape tools and the pencil
// leave). Everything else on the canvas is outside: black, text, RGB cells,
// other ramps' colours. A fringe is always on the shape's edge, and a full
// cell is when one of its eight neighbours is outside or a fringe (a 3x3
// neighbourhood, so the edge has no notches and
// runs 4-connected along a diagonal). Off the canvas is not outside: a
// shape running off the frame has no rim there. The rim light never lights
// a fringe (a half block has one colour a half, and a rung is a shade glyph
// between two colours), so the rim falls on the full cells in from it,
// where their own normal faces the light and the neighbour rule allows it
// (RimLightBrush, below). The canvas is read as it was
// before the stroke, so one stroke lights one cell deep however often it
// passes. It ignores strength and pressure.

#include "blockwright/brush.h"
#include "blockwright/ramp.h"

#include <optional>

namespace bw {

// Whether (x, y) is a shape cell on the shape's edge (above).
bool onEdge(const Canvas& canvas, const Ramp& ramp, int x, int y, bool ice = true);

// The outward normal of the shape's edge at (x, y), on screen, unit length;
// empty off the edge, or where the outside cancels out (a stem with the
// outside on both sides). It is taken on a grid of 2 x 2 sub-pixels a cell
// (▀ and ▄ split a cell top and bottom, ▌ and ▐ left and right), a
// sub-pixel half a column wide and a column tall on screen: the sum of the
// offsets from the cell's centre to the outside sub-pixels of the cells
// within three columns' distance (three columns either way on the cell's
// row, two on the rows above and below), each weighted a half. A sub-pixel
// is outside when its cell is outside, or it is in a fringe's black half; a
// fringe's coloured half is inside; off the canvas is not outside. The
// window is round on screen, so the normals of a round shape spread evenly
// and a light lights an arc centred on its direction, and a shape of full
// cells has the normals it would have taken a cell at a time. Because off
// the canvas is not outside, a cell on the canvas's first or last three
// columns can have another normal than the same cell of the same shape
// further in, where its window finds outside cells (a stem a column wide on
// column 1 or 2 faces E down its length); up and down the window reaches a
// row, so only a full cell on the first or last row can. A ▌/▐ fringe's
// coloured half is inside, so a ▌/▐ column is not an empty column to the
// normal.
std::optional<std::pair<double, double>> edgeNormal(const Canvas& canvas, const Ramp& ramp, int x, int y,
                                                    bool ice = true);

// Rim-light brush: the full edge cells under it that face the light go one
// rung lighter along the ramp (stepRung, so iCE off writes no bright
// background); fringes stay as they are. A cell faces the light when its
// edge normal, on screen, is within about 73° of the light's direction on
// screen: the normal's dot product with the light's unit vector over 0.3,
// as the rim light. Nor is a cell lit when the neighbour a step
// towards the light (from NE, the one up and to the right) is a full shape
// cell while the one a step away is outside or a fringe, whatever its
// normal sums to (the column of a narrow shape that faces a gap can see
// more outside on the shape's far side, within three columns, than in the
// gap). A neighbour off the canvas is neither. The rule checks only that
// one pair of neighbours, along the light's own step, not whether the cell
// has an open side towards the light. The direction is the Brush dock's
// light direction (BrushSettings::lightDirection), the way the light comes
// from (NW: from the top left). It is an accent, not held to the ladder
// lint: a lit cell can make a rung skip or a speck, which the lint reports.
//
// Known limits (PLAN §10; follow-up #54):
// - canvas edges: a shape within three columns of the left or right edge,
//   or with full cells on the first or last row, can light differently
//   from the same shape further in (edgeNormal, above);
// - ▌/▐ gaps: the top and bottom cells beside a ▌/▐ column can light where
//   across an empty column they don't;
// - counters: in a gap closed above and below (a letter's counter), the
//   side walls' top and bottom inner cells can be lit from the far
//   diagonal, where the neighbour a step away is the counter's top or
//   bottom stroke;
// - thin bars split by a black half-pixel row: the interior of the full
//   row of a bar three pixels tall with another shape a black pixel row
//   under it, such as (10,2), faces up and is lit from above, since the
//   neighbour towards the light is a fringe; the row's end cells, such as
//   (2,2) with a dot of about 0.24 with N, stay unlit from above;
// - grazing cells on rotated ellipses: a cell whose normal faces the light
//   just over the threshold can be left unlit when the neighbour pair
//   along the light's step is full and outside or a fringe.
class RimLightBrush : public Brush {
public:
    RimLightBrush(Ramp ramp, Compass light);

    std::string_view name() const override { return "rim-light"; }
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

    // Whether an edge normal faces a light from `light`.
    static bool facesLight(std::pair<double, double> normal, Compass light);

private:
    Ramp ramp_;
    Compass light_;
};

} // namespace bw
