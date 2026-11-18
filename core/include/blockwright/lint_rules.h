#pragma once
// The whole lint (PLAN §3, §10): the ladder check (lint.h) under each ramp,
// the pixel noise score, and the craft rules that
// a program can judge (stray cells, same-colour merges, dotted diagonals,
// flat areas, what IRC drops, glyphs an .ans saves as '?'). The lint panel and
// `blockwright-cli lint` both run lintCanvas, so they agree.
#include "blockwright/lint.h"
#include "blockwright/target.h"

#include <algorithm>
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
        NoCp437,         // a glyph an .ans saves as '?': no CP437 code, or a control byte there
        WidthVaries,     // a glyph whose columns a client or terminal may count differently
        LoneMark,        // a zero-width code point in a cell of its own, or a lone wide cell
        BidiControl,     // a bidi control, which reorders the rest of the line in some clients
        AnsLoss,         // IRC target: what an .ans save would drop, one finding per kind
        WastedFormat,    // bold, italic or underline that draws nothing but costs IRC bytes
        IrcFold,         // IRC in 16 colours: cells in mIRC 16-98 sent as the nearest of 0-15
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
// Whether the kind is a note: reported, but no fault, so it doesn't make a
// report unclean or `lint` exit 4. AnsLoss is one: the art is fine for IRC.
// IrcFold is another: the fold is what 16 colours asks for.
bool noteKind(Finding::Kind kind);

struct RuleOptions {
    bool ice = true;  // read pixels by the colours they show (halfBlocks)
    // The subject: Flat is judged only here (detail over --box). With
    // none, Flat isn't judged: a ground or backdrop may be flat by intent.
    std::optional<Box> box;
    // Flat areas of fewer cells than this aren't reported.
    int minFlatCells = 48;
    // The document's target: under IRC and Micron, NoCp437 is off and
    // AnsLoss runs; under Micron the rules about what IRC drops and
    // WastedFormat are off too.
    Target target = Target::Ansi;
    // The colours IRC text carries (sentIrcColours of the document's
    // target and setting): with 16, IrcFold runs.
    IrcColours colours = IrcColours::Mirc99;
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
//   no break. Not along the canvas's edge either, where a cell across the
//   line is off the canvas: the line may be thicker past it (a column of
//   dither down the edge). Across a row or down a column.
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
//   TrailingBlack and BlankEdgeRows are off under Micron: a page keeps
//   those cells (PLAN §17).
// - NoCp437: a glyph an .ans saves as '?': one with no CP437 code, or one
//   whose CP437 byte .ans reads as a control code (ansControlByte: ○ ◙ ♪ → ←),
//   which XBIN and BIN keep. A cluster is named by its code points.
//   ANSI target only.
// - WidthVaries (PLAN §16, Lint): a cell holding a code point of width 0
//   or 2 whose width varies since widthVariesFloor() (widthVaries), a
//   joiner (U+200D) in a cluster, VS16 (U+FE0F) on a narrow base, or a
//   skin tone modifier; and two regional indicators in a row (a flag), from
//   the first to the second. What a terminal that joins graphemes, or a
//   client on an older width table, draws in other columns than the cells.
//   One finding a cell, its reasons joined by "; ".
// - LoneMark: a cell whose glyph (or cluster base) is zero-width, or a
//   two-column glyph with no continuation right of it (a lone wide cell).
// - BidiControl: a cell holding U+061C, U+200E-200F, U+202A-202E or
//   U+2066-2069, alone or in a cluster.
// - AnsLoss: IRC and Micron targets only, where the art is fine for them and an .ans is
//   the lossy save. One finding per kind of loss, at the first cell it
//   covers, counting the cells: glyphs a CP437 .ans saves as '?' (the cells
//   NoCp437 would report), 24-bit colours saved as the nearest of the 16
//   (quantisedColourCount) and cells with bold, italic or underline, which
//   an .ans leaves out (formattedCellCount).
// - WastedFormat: under ANSI and IRC (not Micron, which has no byte
//   budget), a cell whose formatting draws nothing
//   and still costs its toggles in IRC text: an attribute without which the
//   cell draws the same pixels (formatRows) at 8 and 9 px, such as bold on
//   a blank, ▀ or █, or underline on █. Italic shears a block off its
//   background, so it draws. Only glyphs the VGA font has are judged. One
//   finding a cell, naming the attributes wasted.
// - IrcFold: with options.colours 16 only (#496). Read from the cells as
//   IRC sends them (irc::sentCell under IRC with options.ice). One finding,
//   at the first cell it counts, counting the cells (not transparent, a
//   pair at its lead) that show a colour IRC text in 99 colours sends as
//   one of mIRC 16-98, which 16 colours send as the nearest of 0-15
//   (irc::fold16): a foreground on a glyph that isn't blank, a background
//   under one that isn't a full block, as ans-loss reads them. A second, at
//   the first such cell, counting those whose glyph the fold hides: a
//   foreground and background that differ in 99 colours and fold to the
//   same one. Both notes.
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
    Target target = Target::Ansi;  // RuleOptions::target
    IrcColours colours = IrcColours::Mirc99;  // RuleOptions::colours
};

struct LintReport {
    std::vector<Finding> findings;  // by y, then x, then kind
    double noise = 0.0;             // pixel noise over the whole canvas
    bool ice = true;
    // No finding but notes (noteKind).
    bool clean() const {
        return std::all_of(findings.begin(), findings.end(), [](const Finding& f) { return noteKind(f.kind); });
    }
};

// Everything: the ladder check of the whole canvas under each ramp (with
// fallback specks on), the noise score, and checkRules. A finding of the
// same kind at the same cells under two ramps is reported once, under the
// first. A shade of two colours that one of the ramps has as neighbours is
// no fault, whichever ramps don't: the lint can't know which ramp the artist
// shaded on. With craftPairs, neither is a shade of a craftPair. Under the
// IRC or Micron target a ladder finding at an RGB cell says it was judged by the
// nearest of the 16. Throws LintError for a bad ramp (rampProblem).
LintReport lintCanvas(const Canvas& canvas, const LintOptions& options = {});

// The report in a line: "clean" or "N findings (n kind, ...)" by kind in the
// order the kinds are declared, the notes apart as ", N notes (n kind)", then
// the noise and whether iCE is on, as "3 findings (1 speckle, 2 gap), noise
// 0.0071, iCE on" or "clean, 1 note (1 ans-loss), noise 0.0000, iCE on".
std::string summary(const LintReport& report);

} // namespace bw::lint
