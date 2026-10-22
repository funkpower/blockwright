#pragma once
// The speckle and ladder checks of clean block shading, with the common
// measures of pixel noise and detail.

#include "blockwright/canvas.h"
#include "blockwright/ramp.h"

#include <optional>
#include <string>
#include <vector>

namespace bw::lint {

// A rectangle of cells, corners inclusive, as --box x0,r0,x1,r1.
struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The canvas as pixels: two pixels per cell (top, bottom) from
// █ ▀ ▄ and spaces; shade and text cells are empty. Cropped to the box.
std::vector<std::vector<std::optional<std::uint8_t>>> pixels(const Canvas& canvas, std::optional<Box> box = {});

// Noise: the share of drawn, non-black pixels unlike all four
// neighbours, over pixels away from the edge whose neighbours are all
// pixels. As in the written file, each row's trailing black
// spaces aren't pixels. Shade cells aren't pixels either, so shade speckle
// doesn't show here (LadderFinding::Speckle catches it). Speckle reads as
// noise; smaug-on-the-hoard went from 0.033 to 0.016 with a despeckle pass.
double noise(const Canvas& canvas, std::optional<Box> box = {});

struct LadderOptions {
    // The widest a shade rung may be along the gradient, in cells: each rung
    // has each rung 1-3 cells wide.
    int maxRung = 3;
    // The most noise a clean area has.
    double maxNoise = 0.02;
};

struct LadderFinding {
    enum class Kind {
        NonNeighbourShade,  // a ░▒▓ of two colours of the ramp that aren't neighbours
        WideRung,           // a step of a gradient wider than maxRung along it
        RungSkip,           // a gradient jumping more than one rung
        Speckle,            // a lone cell lighter or darker than all its neighbours
        OneGlyph,           // the shading uses one of ░▒▓ only
        Noise,              // pixel noise over maxNoise
    };
    Kind kind;
    int x = 0, y = 0;  // canvas cell (the box's corner for OneGlyph and Noise)
    std::string message;
};

struct LadderReport {
    int cells = 0;         // cells checked
    int ladderCells = 0;   // cells on the ramp's ladder
    int shadeCells = 0;    // of those, ░▒▓ shades
    double noise = 0.0;
    std::vector<LadderFinding> findings;

    bool clean() const { return findings.empty(); }
};

// Checks the box (the whole canvas when empty) against the ramp's ladder:
// shades only between ramp neighbours, rungs no wider than maxRung, no
// skipped rungs, no specks, more than one shade glyph, and noise no higher
// than maxNoise.
//
// Rows and columns are read as gradients. Only a step of one is judged for
// width: a run of a shade rung with a lower rung at one end and a higher one
// at the other. Its width is the shorter of that run and the rung's longest
// run the other way, in cells, so a rung lying along the contour of a ball
// isn't counted wide, and a flat field isn't judged at all. A jump of more
// than one rung is a skip where three or more levels climb or fall
// together; an outline that turns back, two planes meeting (two levels and
// no third) and black against anything (ink, seams, the silhouette) aren't
// skips, nor is a one-cell end level in the outline brush's colour, the one
// next below the darker colour beside it. A speck is a cell with three or more ladder
// neighbours that is lighter than all of them or darker than all of them;
// black (ink) and the ramp's top rung (a glint) are never specks. Cells off
// the ramp (text, half blocks, RGB and other ramps' colours) aren't judged.
LadderReport checkLadder(const Canvas& canvas, const Ramp& ramp, std::optional<Box> box = {},
                         const LadderOptions& options = {});

} // namespace bw::lint
