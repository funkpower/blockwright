#pragma once
// Detail brushes (PLAN §10, "Detail"): the outline and rim-light brushes. It works on a
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

// Whether the cell is in the ramp's shape (above), as it shows with iCE
// `ice`.
bool inShape(const Ramp& ramp, const Cell& cell, bool ice = true);

// How many cells the detail brushes have read on this thread so far, the
// cells they copied and the lint's reads (lint::cellsRead) included: a work
// counter, for tests of what a dab costs.
long long detailCellsRead();

// The cells an outline may read (detailCellsRead) before a careful pass
// starts no more (OutlineBrush, below), on this thread: 5e7 unless a test
// sets another. Returns the one it replaces.
long long setCarefulReads(long long reads);

// How many cells the outline read settling the last dab on this thread
// (detailCellsRead), the count the careful-pass budget is on: for tests.
long long outlineCellsRead();

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

// The normal the rim light reads (RimLightBrush, below): edgeNormal's, over
// only the outside in the window that reaches the cell. That is each
// region of outside sub-pixels in the window, joined by a side or a corner,
// that touches the cell's own sub-pixels; where none does (a full cell whose
// only outside lies past a fringe's coloured half, as under a row of ▄
// fringes), each region with a sub-pixel in one of its eight neighbours.
// Outside beyond the shape doesn't turn it: a narrow shape's column beside
// a gap faces the gap, a counter's inner wall faces the counter, and the
// full row of a bar three pixels tall, two columns or more in from the
// bar's ends, faces the black pixel row under it however much outside lies
// past the fringes above it. Nearer the ends that outside joins the outside
// below round the bar's end within the window, so it reaches the cell (a
// known limit, RimLightBrush below).
std::optional<std::pair<double, double>> rimNormal(const Canvas& canvas, const Ramp& ramp, int x, int y,
                                                   bool ice = true);

// Outline brush (PLAN §10): the edge cells under it become the line, the
// outline colour (lint::outlineRung) of the level inside them: the darkest
// interior cell (not on the edge, not a half block) at the shortest walk
// through the shape, read once a stroke from the whole stroke-start canvas,
// so a thin part takes the line of the nearest body however far along it.
// A shape with no interior is left alone. Where the colour below would be
// the ground the line is the darkest colour above the ground. A cell darker
// than the line is kept, but for a shade into the ground (k░K) on the edge,
// which takes the line. The line over the cells the stroke has covered is
// settled against checkLadder: where it would add a finding the cells to
// blame take the next lighter candidate, down to the cell as it was, so the
// line is the darkest the lint accepts. Each finding is blamed on its own:
// on the line cells along its own row or column (a wide run's width, where
// nothing along it is, on the runs across it; a speck on the speck, or what
// is round it); noise on the changed cells whose own change raises it; one
// glyph on the cells that change a shade glyph. A blamed cell beside a
// darker one waits for it. Where no step clears noise, the one finding, the
// line goes back on the index a cell at a time, the cells that raise the
// noise least alone first, each kept where the noise stays as it was or
// within its limit; where no step clears another finding, only the fewest
// line cells nearest it are left as they were, not the rest of the line. An
// image's first settle (a click, a stroke's start) that writes nothing from
// three or more covered cells settles again carefully, while its outline
// has read under 5e7 cells (detailCellsRead): a skip between two
// changed cells is first the cell more findings involve, and cells left as
// they were are tried a step darker, alone or with a stepped neighbour. Repairs are on
// covered cells only: a fade cell the line leaves a speck takes the darkest
// colour above the ground, and so on inward. Lone fragments (under three
// line cells beside covered edge cells the line leaves, or one cell a step
// lighter with no line beside it) and corners of pieces of four or more (the
// end of a lined side where the side round it stays unlined) are left off
// unless the lint needs them. Every write is judged by lint::LadderIndex
// with the guard identity (place; wide rungs by overlap; noise only over its
// limit and higher), so a dab adds no finding anywhere on the canvas, and a
// dab that softens a finding or changes only its value is allowed. Each dab
// settles only round its own new cells: the covered cells within three of
// it that the stroke has left as they were, against the line the stroke has
// drawn so far, settled again until it is its own fixed point. A cell the
// stroke has written stays for the rest of the stroke, unless a strictly
// stronger dab covers it; a dab that would add a finding adds nothing. A
// dab's work is flat in the stroke's length and O(W + H) per changed cell.
// Each symmetry image (Dab::image) is outlined on its own, a mirror image in
// its own frame (the canvas mirrored back). The stroke stamps each point's
// images in turn, and their composite is judged on the shown canvas as a
// whole: where it adds a finding, the cells to blame go back, a mirror orbit
// at a time and in the images too, until it adds nothing; where nothing to
// blame clears it, every image's cells are refused, so a Mirror X click can
// write nothing where the stroke alone would write a line. Where images overlap, a cell shows the line of the strongest dab
// that changed it; on a tie the image whose frame has the cell in the
// canvas's first half (across, then down) wins, then the lower image, so a
// mirrored canvas gets a mirrored line. Its
// dabs again in the same stroke change nothing. Limits: a cell written a step
// lighter before the line beside it was drawn stays for the stroke, so a
// second stroke over the result can take it darker (it adds no finding);
// where the careful budget ran out, so can a second click (baud-dudes-all,
// 80 x 2312, skin, square 161 at (60,1541): 42 cells after 305). A larger
// click is not sure to write more of the line where the noise is over its
// limit: psyop-radar's steel at (20,13) writes 127 cells at size 73 and 109
// at 75, cells the noise fallback keeps apart being pruned as fragments. A
// fringe keeps its shape half
// and takes the line in its black half, outside the shape, when the line is
// darker than that half; with iCE off it is written the way round that keeps
// the background dark, or left when it can't be. Where an outside cell lies
// beside the fringe along its half (left or right of a ▀/▄, above or below a
// ▌/▐), the shape half would still meet the ground past that line, so the
// whole cell takes the line first and the half is the next step lighter.
// With OutlineShape::Selection the shape is only the ramp's cells in the
// stroke's selection: the cells outside it are outside the shape, so where
// the selected shape touches another on the same ramp its edge runs along
// the seam and takes the line on its own side, as against black (#51). With
// no selection, or one over the whole canvas, it is the outline as ever. The
// seam is settled against the lint like the rest of the line: beside a shape
// that climbs away from it the darkest line can be a skip (w between W and
// W▒w, W▓w after it), and the seam takes a lighter rung there.
enum class OutlineShape {
    Ramp,       // every cell on the ramp but the ground; the selection only clips
    Selection,  // those cells in the selection
};

class OutlineBrush : public Brush {
public:
    explicit OutlineBrush(Ramp ramp, OutlineShape shape = OutlineShape::Ramp);

    std::string_view name() const override { return "outline"; }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override;

    // The stroke's state: the cells it has covered and their line.
    struct Plan;

private:
    void settleImage(Stroke& stroke, Plan& plan, const Dab& d, int size, double strength) const;
    void composite(Stroke& stroke, Plan& plan) const;

    Ramp ramp_;
    OutlineShape shape_;
};

// Rim-light brush: the full edge cells under it that face the light go one
// rung lighter along the ramp (stepRung, so iCE off writes no bright
// background); fringes stay as they are. A cell faces the light when its
// rim normal (rimNormal, above: the normal over the outside that reaches
// it), on screen, is within about 73° of the light's direction on screen:
// the normal's dot product with the light's unit vector over 0.3. A cell is lit only past outside beside it on the
// light's side: an outside sub-pixel in one of its eight neighbours whose
// offset from the cell's centre, on screen, has a positive dot product with
// the light's direction, however the outside that reaches it sums. Nor is a
// cell lit when the neighbour a step towards the light (from NE, the one up
// and to the right) is a full shape cell while the one a step away is
// outside or a fringe, whatever its normal sums to. A neighbour off the
// canvas is neither, and off the canvas is not outside. The direction is
// Tool Options' light direction (BrushSettings::lightDirection), the way
// the light comes from (NW: from the top left). By default it is an
// accent, not held to the ladder lint: a lit cell can make a rung skip or
// a speck, which the lint reports. With keepLint (Tool Options' "Keep the
// lint clean") each stamp is judged by the stroke's lint guard (StrokeLint,
// guard.h), taken at its first dab: a cell whose step would add a finding
// to the stroke's start stays as it is, blamed cell by cell as the form
// brush takes cells back, and is tried again only by a later stamp that
// would light a cell beside it (a cell that would be a speck alone can be
// lit with the next one along the rim); a stamp looks up only its own
// cells' eight neighbours among the cells refused so far. Under a mirror
// (X, Y or both) a cell lights only with its whole orbit, every image the
// mirror makes of it: each image unpainted, selected and lit by the
// rim-light rule from the same (unmirrored) light. An orbit with an image
// outside the selection or out of the light stays as it is, so a symmetric
// canvas stays symmetric on every axis of the mirror.
//
// Known limits (PLAN §10):
// - canvas edges: a shape within three columns of the left or right edge,
//   or with full cells on the first or last row, can light differently
//   from the same shape further in (edgeNormal, above);
// - ▌/▐ gaps: the top and bottom cells beside a ▌/▐ column can light where
//   across an empty column they don't;
// - grazing cells on rotated ellipses: a cell whose normal faces the light
//   just over the threshold can be left unlit when the neighbour pair
//   along the light's step is full and outside or a fringe;
// - thin bars by their ends: the full row of a bar three pixels tall under
//   ▄ fringes, at the bar's end or a column in from it, can face up or
//   sideways (rimNormal, above), so it can be lit from N, NE or NW and not
//   from S;
// - keepLint on diagonal rims: a staircase rim lights one cell a row, each
//   a speck, so it stays unlit; a flat █ shape with no ░▒▓ lights only the
//   few cells the one-glyph rule allows;
// - keepLint under radial symmetry is judged a stamp at a time, not as
//   orbits (under a mirror every stamp's images are the images' own
//   stamps, so it lights only cells the accent's stroke reaches);
// - keepLint under a mirror whose axis the light crosses (N under mirror
//   Y) lights only cells whose images face the light too: a flat top row
//   under mirror Y lights nothing from N;
// - keepLint lights all it safely can only among the cells a stamp tries:
//   after each stamp, every cell it tried (its own, and those left unlit
//   before beside them; with their images under a mirror) is lit or would
//   add a finding lit alone, but not every cell of the canvas. A change
//   further off that makes a cell left unlit safe (a stamp elsewhere that
//   brings in a second shade glyph) doesn't light it; it waits for a later
//   stamp beside it.
class RimLightBrush : public Brush {
public:
    RimLightBrush(Ramp ramp, Compass light, bool keepLint = false);

    std::string_view name() const override { return "rim-light"; }
    // Brush::dab, or with keepLint the stamp held to the lint guard.
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override;
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

    // How many refused cells the keepLint retry has looked up on this thread
    // so far: a work counter, for tests of what a stamp costs.
    static long long refusalLookups();

    // Whether an edge normal faces a light from `light`.
    static bool facesLight(std::pair<double, double> normal, Compass light);

private:
    Ramp ramp_;
    Compass light_;
    bool keepLint_;
};

} // namespace bw
