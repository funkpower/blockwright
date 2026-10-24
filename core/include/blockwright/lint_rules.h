#pragma once
// The whole lint (PLAN §3, §10): the ladder check (lint.h) under each ramp,
// the pixel noise score, and the craft rules that
// a program can judge (stray cells, same-colour merges, dotted diagonals,
// flat areas, what IRC drops, glyphs with no CP437 code). The lint panel and
// `blockwright-cli lint` both run lintCanvas, so they agree.
#include "blockwright/lint.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bw::lint {

struct Finding {
    enum class Kind {
        // the ladder check, under one ramp (LadderFinding::Kind)
        NonNeighbourShade,
        WideRung,
        RungSkip,
        Speckle,
        FallbackSpeck,
        OneGlyph,
        // pixel noise over the limit
        Noise,
        // the craft rules (checkRules)
        LonePixel,       // a one-pixel glint alone on the form it lights
        Gap,             // a one-cell break in a line one cell thick
        Merge,           // a glyph in its own background colour
        DottedDiagonal,  // a 1 px line whose pixels touch only at the corners
        Flat,            // an area of one flat look in the box, 4x2-cell windows wide
        TrailingBlack,   // black cells at a row's end beside rows that reach the edge: IRC drops them
        BlankEdgeRows,   // blank rows above or below the art: IRC trims them
        NoCp437,         // a glyph CP437 has no code for: it saves as '?'
    };
    Kind kind = Kind::Noise;
    std::string ramp;  // the ramp a ladder finding came from; empty for the rest
    int x = 0, y = 0;  // the cell it is at
    int x2 = 0, y2 = 0;  // the far corner of what it covers, or its other cell; x, y for one cell
    std::string message;
    // The cells it is at when they don't fill the rectangle (a diagonal), in
    // reading order; empty when they do. The box keeps it by these.
    std::vector<std::pair<int, int>> cells;

    friend bool operator==(const Finding&, const Finding&) = default;
    // Same kind at the same cells, whatever the ramp.
    bool samePlace(const Finding& o) const {
        return kind == o.kind && x == o.x && y == o.y && x2 == o.x2 && y2 == o.y2;
    }
};

// The kind's name as the CLI prints it ("rung-skip", "lone-pixel", ...).
std::string_view kindName(Finding::Kind kind);
// Whether the kind comes from the ladder check (and so names a ramp).
bool ladderKind(Finding::Kind kind);

struct RuleOptions {
    bool ice = true;  // read pixels by the colours they show (halfBlocks)
    // The subject: Flat is judged only here (detail over --box). With
    // none, Flat isn't judged: a ground or backdrop may be flat by intent.
    std::optional<Box> box;
    // Flat areas of fewer cells than this aren't reported.
    int minFlatCells = 48;
};

// The craft rules over the whole canvas, in reading order (by y, then x).
// Each rule, as a program judges it:
// - LonePixel: a half-block pixel whose eight neighbours are all one other
//   colour, not black (on black it is a star), where the pixel is a lighter
//   rung of that colour on a named ramp: a glint, which wants a 2-5 px
//   cluster. A pixel of another hue (an LED, a jewel) is a detail, not this.
// - Gap: a cell between two cells of one look on each side, in a line one
//   cell thick (the cells across the line, beside the gap, look different),
//   where the gap shows what is across the line: the line is broken there.
//   Not where the line breaks the same way again 3-6 cells on, with the line
//   on both sides of that break too (a dashed line); past the line's end is
//   no break. Across a row or down a column.
// - Merge: a glyph other than a space or a block element in its own
//   background's colour: it can't be seen. (Figures that touch and fuse
//   need to know which cells are which figure, which a flat canvas doesn't.)
// - DottedDiagonal: four or more pixels of one colour joined only at their
//   corners, each with no side neighbour of its colour and at most two
//   corner ones (a checker has four): an 8-connected 1 px line, which
//   renders as dashes. From its top end to its bottom end, with its cells.
// - Flat: only in options.box. The cells of 4x2-cell windows of one look,
//   not black, at any offset (so where the box starts doesn't matter), joined
//   side by side into areas of one look of at least minFlatCells cells. The
//   finding covers an area's bounding box and counts its cells.
// - TrailingBlack: the black spaces at a row's end that the IRC encoding
//   drops, where the row above or below reaches the last column: there the
//   art is black to the edge (a plate, a band, a frame's rim) and IRC shows
//   the client's background instead.
// - BlankEdgeRows: blank rows above or below the art, which the export trims
//   (a margin needs a black-on-black █).
// - NoCp437: a glyph with no CP437 code, which an .ans saves as '?'.
std::vector<Finding> checkRules(const Canvas& canvas, const RuleOptions& options = {});

// Whether hand shading goes between the two colours, either way round,
// beyond the named ramps' neighbours: any colour but white straight into
// black (a saturated hero colour into black, "a one-hue ladder over black",
// warm skin's k n, two lights' k w, the dark object's k K) and the other
// steps of the measured ladders (pale skin K w W, warm skin n R w, cool
// cloth m B c w, the highlights W▓B and Y▓W, the pockets m░r and K▓m, the
// spills C░c and Y░r, hair Y░n, storm water K g and G▒K, the crest r▒K).
// White on black is a stroke, not a ladder.
bool craftPair(std::uint8_t a, std::uint8_t b);

struct LintOptions {
    std::vector<Ramp> ramps;  // each runs the ladder check; empty runs none
    bool craftPairs = false;  // a shade of a craftPair is no fault either (the default lint: every named ramp)
    std::optional<Box> box;   // only findings at a cell in the box, and Flat judged there; the lint still reads the whole canvas
    bool ice = true;
    double maxNoise = 0.02;
};

struct LintReport {
    std::vector<Finding> findings;  // by y, then x, then kind
    double noise = 0.0;             // pixel noise over the whole canvas
    bool ice = true;
};

// Everything: the ladder check of the whole canvas under each ramp (with
// fallback specks on), the noise score, and checkRules. A finding of the
// same kind at the same cells under two ramps is reported once, under the
// first. A shade of two colours that one of the ramps has as neighbours is
// no fault, whichever ramps don't: the lint can't know which ramp the artist
// shaded on. With craftPairs, neither is a shade of a craftPair. Throws LintError for a bad ramp (rampProblem).
LintReport lintCanvas(const Canvas& canvas, const LintOptions& options = {});

// The report in a line: "clean" or "N findings (n kind, ...)" by kind in the
// order the kinds are declared, then the noise and whether iCE is on, as
// "3 findings (1 speckle, 2 gap), noise 0.0071, iCE on".
std::string summary(const LintReport& report);

} // namespace bw::lint
