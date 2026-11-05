#pragma once
// Form shading brush (PLAN §10, "Shading"): shades a shape as a dome, a
// cylinder or a flat plane, lit from the light direction (BrushSettings::
// lightDirection, north-west by default), along a ramp's
// ladder.

#include "blockwright/brush.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"

#include <map>
#include <optional>
#include <vector>

namespace bw {

class StrokeLint;

enum class FormShape { Dome, Cylinder, Plane };

struct FormOptions {
    FormShape shape = FormShape::Dome;
    // A rung of reflected light along the edge that faces away from the
    // light.
    bool rim = false;
    // A glint of the ramp's top rung, two cells at the brightest point,
    // two columns (a row) clear of the edge.
    bool specular = true;

    bool operator==(const FormOptions&) const = default;
};

// The form's tone for each cell of the shape, lit from `light`: a rung of
// the ramp per cell of a shape.width() × shape.height() grid, row-major, -1
// off the shape. `highest` is the lightest rung it may use (-1: the ramp's
// top).
//
// Each cell of a dome or a cylinder gets a normal from its distance to the
// shape's edge (edgeDistance): a dome is a ball whose surface meets the
// shape's edge, a cylinder is round across the shape's shorter side on
// screen and straight along the longer one; a plane tilts towards the
// light. A cylinder lit along its axis (the light's part across it under
// 0.3 of it) is lit from the diagonal on the side the light leans to
// across it, west of a pillar and north of a log when it leans to neither.
// Each cell is as far from the brightest point as the angle between its
// normal and the light's whole direction (across the canvas and out of it)
// times the form's radius; on a plane, as far along the light.
//
// The form is a broad lit plane, solid, on the cells nearest the light (the
// top rung, or with `specular` the highest solid rung below it); a fall
// crowded at the turn from the light, every rung from the one below the
// plane down to the first above the darkest colour, each one to five
// columns of the arc wide; and past it the darkest colour, black on a ramp
// that starts with it. A cylinder too thin for the ladder to come down to
// black still has black on its shadow edge (its silhouette across the axis
// facing away from the light), unless `rim` or the ramp doesn't start with
// black. A dome or a cylinder with `specular` gets a glint,
// the top rung on the most lit cell clear of the edge with room for it and
// its more lit neighbour in the row (a cylinder's ties go to the middle of
// its length), falling to the plane a rung a cell or a rung a column on
// screen: a 2-5 cell spot ringed once by the ladder. It has room where every cell it lifts above the
// plane, and each of that cell's neighbours, is on the shape at the
// plane's rung: a spot on the lit side that never reaches the fall, the
// shadow or the edge. A plane has none. `rim` lifts the edge facing away
// from the light a rung. The tones are then kept a ladder: neighbours at
// most a rung apart (the dark side comes up where the form turns faster
// than the fall), and no cell lighter or darker than all its neighbours on
// the shape but the glint.
//
// The fall is tried at the turn, a little either side of it, and just past
// the share of the form the light lights as brightly as the plane,
// narrowest bands first, with the glint (its falloff none or a column a
// rung) and then without it. A try is in bounds where its black
// (the darkest rung) is no more than the light's shadow and ten points and
// its lit plane (the plane's rung and above) no less than the light's lit
// share (formLight); a try out of bounds is never the form while one in
// bounds is clean. The first try in bounds that clears the bar is
// the form: lint-clean at its cells (cells off the shape are black), the
// lit plane there, no rung of the turn wider than four cells, two shade
// glyphs or more, no seam (a cell lighter than both its neighbours in the
// row three rows running), and where the form has whole 4x2 windows, detail
// 3.6 or more and under 15% flat, each the mean over the 8 alignments of
// the windows on its box (lint::detail). When none clears it, the clean try
// in bounds that comes nearest is the form (the bar's parts weighed in that
// order, the glint before flatness), then the clean try that comes nearest;
// a large dome lit with a glint can miss the detail under the bounds, its
// lit plane a third of it. When no try is clean, every tone is -1 and nothing is laid. A
// form big enough that every try would be slow gets the first tries that
// cover a couple of million cells, the narrowest bands. The same shape,
// options and `highest` always give the same tones.
std::vector<int> formRungs(const Ramp& ramp, const Mask& shape, const FormOptions& options = {},
                          Compass light = Compass::NW, int highest = -1);

// The light's shares of a form, from its normals and the light (with a
// cylinder's tilt): the shadow, the cells turned from the light, and the
// lit share for a plane at rung `plane`, the cells it lights as brightly
// as that rung or more (the rungs from black to the lightest taken as the
// light from none to full).
struct FormLight {
    double shadow = 0;
    double lit = 0;
};
FormLight formLight(const Ramp& ramp, const Mask& shape, const FormOptions& options, Compass light, int plane,
                    int highest = -1);

// How the form formRungs lays reads.
struct FormReading {
    // whether there is a form (a clean try), whether it clears the bar,
    // and whether it is in bounds
    bool laid = false, clears = false, bounded = false;
    // the lit plane's rung
    int plane = 0;
    // detail and percent flat over the box, the mean over the 8 alignments
    // of the windows; and at the box's corner, the usual reading of a piece
    double detail = 0, flatPercent = 0;
    lint::Detail corner;
    // shares of the form: black (the darkest rung) and the lit plane (its
    // rung and above); and the light's (formLight)
    double black = 0, lit = 0;
    double shadow = 0, litFloor = 0;
};

struct FormResult {
    std::vector<int> tones;
    FormReading reading;
};

// formRungs with how its form reads.
FormResult formShade(const Ramp& ramp, const Mask& shape, const FormOptions& options = {},
                     Compass light = Compass::NW, int highest = -1);

// Form brush: paints the cells under it with their tone of the form, lit
// from the stroke's light direction (BrushSettings::lightDirection).
// The form is the selection's shape (the selected cells the stroke may
// paint) or, with nothing selected, the cells the stroke has covered so
// far, the form worked out near each dab that adds to it: over the covered
// cells within two dab widths and six columns either side of its
// footprint, a dab's height and three rows above and below, so the stroke
// sees the shape grow. Under the stroke's symmetry each image is a form of
// its own, the cells that image's dabs have covered, lit from the same
// light; where images cover the same cell the strongest dab paints it, the
// lower image on a tie (StrokeOptions::symmetry). Every dab that covers a
// cell or strengthens one lays the covered cells near it again from the
// canvas as the stroke began: within a dab's width and two columns of its
// footprint, half its height and a row above and below, or every cell the
// form was worked out over when that holds every cell the image has
// covered. A cell further off keeps what the stroke left it, so a stamp's
// work is bounded by its size, not the stroke's length. A dab that covers
// nothing new or stronger paints nothing, so replaying a stroke's dabs
// paints what playing them once does; another order can paint otherwise.
// The preview is what is committed. At full strength a cell takes its tone; weaker (or a lighter
// press, with pressure to strength) moves it that share of the ramp's
// rungs towards it, never less than a rung. Cells off the ramp take their
// tone outright. A shade keeps the way round it was written where it can
// (rungCell). With the stroke's iCE colours off the form's lightest rung is
// the last of the ones it can write (no bright background) running
// unbroken up from the darkest, so the ladder never jumps a rung; a weaker
// press that lands on one it can't write takes the nearest one below or
// above that it can. The cells are laid only as they add no lint finding
// to the canvas as the stroke began (StrokeLint, one glyph and noise
// included): the changed cells at and next to a new finding are taken back
// to what the stroke had left them until none is left (all of them when a
// finding is nowhere near one), so a dab that can lay nothing leaves the
// stroke as it was.
class FormShadeBrush : public Brush {
public:
    FormShadeBrush(Ramp ramp, FormOptions options = {});

    std::string_view name() const override { return "form-shade"; }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override;

    const Ramp& ramp() const { return ramp_; }
    const FormOptions& options() const { return options_; }

    // The cells every form brush has worked through so far, each try's box
    // and each lay counted: a measure of its work.
    static long long cellsVisited();
    // The steps every form brush's passes over a shape have taken so far: each
    // cell a pass reads (the distance transform's included, edgeDistance)
    // and each step of a loop the tones decide the length of. It doesn't
    // depend on the machine: a pass that grew with the shape's square would
    // show here, where cellsVisited counts only boxes. Only in builds with
    // the tests (BW_TEST_HOOKS).
#ifdef BW_TEST_HOOKS
    static long long operations();
#endif

private:
    struct Paint {
        int x = 0, y = 0;
        Cell cell;
        double strength = 1.0;
        int image = 0;
    };
    // The cell a tone paints at a strength; empty off the form.
    std::optional<Cell> toneCell(const Stroke& stroke, const std::vector<int>& tones, int x, int y,
                                 double strength) const;
    // Sets the paints that add no lint finding to the canvas as the stroke
    // began, and the rest to what the stroke had left them, judged by the
    // stroke's guard over `next`, the guard's canvas.
    void lay(Stroke& stroke, StrokeLint& guard, Canvas& next, std::vector<Paint> paints) const;
    // The lightest rung the stroke's form may use.
    int highest(const Stroke& stroke) const;

    Ramp ramp_;
    FormOptions options_;
    // The selection's form, checked at a stroke's first dab (the shape it
    // is made from holds for the stroke) and worked out again when the
    // shape has changed. A brush lives for one drag
    // (BrushTool makes one per stroke), so this is a cache, not stroke
    // state: keyed on the shape's cells, the lightest rung and the light,
    // the only things the tones are made from besides the brush's own ramp
    // and options.
    struct Cached {
        Mask shape;
        int highest = 0;
        Compass light = Compass::NW;
        std::vector<int> tones;
    };
    mutable std::optional<Cached> cached_;
};

} // namespace bw
