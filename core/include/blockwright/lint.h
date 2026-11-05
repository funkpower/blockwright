#pragma once
// The speckle and ladder checks of clean block shading, with the common
// measures of pixel noise and detail.

#include "blockwright/canvas.h"
#include "blockwright/ramp.h"

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bw::lint {

// Options the lint can't judge with (a rung at most 0 cells wide), or a
// canvas an index doesn't hold.
class LintError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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

// Detail: distinct cell looks per 4x2-cell window (a space
// looks like its background, a full block like its foreground, any other
// cell like its glyph and both colours), over the windows aligned from the
// box's corner that have all eight cells and no more than four black, and
// the share of those windows that are one flat look. As in
// the written file, each row's trailing black spaces aren't there and nor
// are blank rows at the top and the bottom; the box's rows count from the
// first row left, as --box does. The
// bar for a modelled form is a mean of 3.6 or more and under 15% flat: below
// it the form reads as flat fills, not light.
struct Detail {
    double mean = 0.0;    // rounded to two places
    int flatPercent = 0;  // rounded to a whole percent
    int windows = 0;      // the windows judged
    int flat = 0;         // of those, one flat look
};
Detail detail(const Canvas& canvas, std::optional<Box> box = {});

struct LadderOptions {
    // The widest a shade rung may be along the gradient, in cells: each rung
    // is 1-3 cells wide, so a band reads as a turn, not a stripe. At least 1: checkLadder and LadderIndex
    // throw LintError below that.
    int maxRung = 3;
    // The most noise a clean area has.
    double maxNoise = 0.02;
    // Judge the box as a whole too (one glyph, noise). Off for a caller
    // that wants the findings at cells only: the noise reads every pixel.
    bool whole = true;
    // Judge fallback specks (#71). Off by default: the brushes' guards
    // don't place them yet, so they read what they always have.
    bool fallback = false;
    // The colours half blocks show, for fallback specks: with iCE off a
    // bright background shows dark (iceCell).
    bool ice = true;
};

struct LadderFinding {
    enum class Kind {
        NonNeighbourShade,  // a ░▒▓ of two colours of the ramp that aren't neighbours
        WideRung,           // a step of a gradient wider than maxRung along it
        RungSkip,           // a gradient jumping more than one rung
        Speckle,            // a lone cell lighter or darker than all its neighbours
        FallbackSpeck,      // a ▀/▄ beside shading in a colour it falls back to but doesn't show
        OneGlyph,           // the shading uses one of ░▒▓ only
        Noise,              // pixel noise over maxNoise
    };
    Kind kind;
    int x = 0, y = 0;  // canvas cell (the box's corner for OneGlyph and Noise)
    std::string message;
    int nx = 0, ny = 0;  // a skip's other cell, across the jump from x, y; a wide rung's run's last cell

    // A finding is all of its fields: two are the same finding only when
    // every field is equal. A wide step across a row and one down a column
    // from the same cell, of the same width, differ only in nx, ny. The
    // whole-canvas list and its tests order and compare findings with these;
    // a brush's guard compares places (below).
    friend bool operator==(const LadderFinding&, const LadderFinding&) = default;
    friend std::strong_ordering operator<=>(const LadderFinding&, const LadderFinding&) = default;

    // Where a finding is, without what it measures: how a brush's guard
    // tells findings apart (LadderIndex::apply), wide rungs aside. Its kind,
    // cell and other cell; the message and the numbers in it (a skip's
    // rungs, whether a speck is lighter or darker, the noise) aren't part of
    // it. A wide rung's place is its first cell and the cell one step along
    // its run (x + 1, y across a row; x, y + 1 down a column), so a row's
    // step and a column's at one cell are two places; the guard matches wide
    // rungs by the cells their runs share instead (LadderIndex).
    struct Place {
        Kind kind;
        int x = 0, y = 0, nx = 0, ny = 0;
        friend std::strong_ordering operator<=>(const Place&, const Place&) = default;
        friend bool operator==(const Place&, const Place&) = default;
    };
    Place place() const;
};

struct LadderReport {
    int cells = 0;         // cells checked
    int ladderCells = 0;   // cells on the ramp's ladder
    int shadeCells = 0;    // of those, ░▒▓ shades
    double noise = 0.0;
    std::vector<LadderFinding> findings;

    bool clean() const { return findings.empty(); }
};

// The outline colour beside a level (PLAN §10, "the end-level rule"): the
// ramp colour next below the darker colour of `rung`, as a solid rung; empty
// where that is black (the ground) or below the ramp. The outline brush
// draws it and checkLadder takes it as an outline at a gradient's end.
std::optional<int> outlineRung(const Ramp& ramp, int rung);

// Whether a cell of rung `g` with the neighbours' rungs `nb` (above, below,
// left, right; -1 off the ladder or the canvas) is left out of the speck
// rule as the ramp gradient's Bayer dither (PLAN §10, the speckle rule):
// the cell carries the dither mark (Cell::dithered) and every neighbour it
// has on the ladder is one rung from it, as the dither lays them. A lone
// cell drawn by hand or by any other tool carries no mark, and a dithered
// cell whose neighbours were redrawn further off is judged again. checkLadder,
// LadderIndex and the despeckle brush (speckFixes) all use this.
bool ditherExempt(bool dithered, int g, const int (&nb)[4]);

// Checks the box (the whole canvas when empty) against the ramp's ladder:
// shades only between ramp neighbours, rungs no wider than maxRung, no
// skipped rungs, no specks (nor fallback specks, with options.fallback),
// more than one shade glyph, and noise no higher than maxNoise.
//
// Rows and columns are read as gradients. Only a step of one is judged for
// width: a run of a shade rung with a lower rung at one end and a higher one
// at the other. Its width is the shorter of that run and the rung's longest
// run the other way, in cells, so a rung lying along the contour of a ball
// isn't counted wide, and a flat field isn't judged at all. A jump of more
// than one rung is a skip where three or more levels climb or fall
// together; an outline that turns back, two planes meeting (two levels and
// no third) and black against anything (ink, seams, the silhouette) aren't
// skips. An outline is not part of the gradient, for skips and widths
// alike: an end level in the outline colour of the level beside it
// (outlineRung), one cell thick along the row or column, or two where they
// are a step of a 4-connected diagonal line: the inner cell a filler with no
// outside 4-neighbour, the run not half of a 2x2 block of the level (except
// where a staircase meets a straight side), and the line going on one cell
// further out on one side and one cell further in on the other, in the same
// line colour or, where the level inside changes, another solid non-black
// one against the outside. Outside is black, off the ladder or off the box.
// A speck is a cell with three or more ladder neighbours that is lighter
// than all of them or darker than all of them;
// black (ink) and the ramp's top rung (a glint) are never specks, nor is a
// cell of the ramp gradient's dither (ditherExempt). Cells off
// the ramp (text, half blocks, RGB and other ramps' colours) aren't judged.
// Throws LintError for options.maxRung below 1.
LadderReport checkLadder(const Canvas& canvas, const Ramp& ramp, std::optional<Box> box = {},
                         const LadderOptions& options = {});

// checkLadder's findings over the whole canvas (no box), kept up to date a
// change at a time, so a brush can judge each step of a stroke by the
// findings it adds. Taking the index reads the canvas once (a whole-canvas
// cost, once per stroke); after that a change costs the rows and columns
// through its cells (O(W + H) per changed cell, a few more where a row's
// trailing blank run moves, and the rows and columns within two of it,
// whose end-level outlines read it), however large the canvas is.
//
// What a change adds is what a brush's guard refuses, judged on the whole
// canvas, the one-glyph and noise findings included when options.whole is
// on (a test checks it against linting both whole canvases):
//  - a wide rung, unless the held canvas had one the same way (across or
//    down), on the same row or column, of the same rung, whose run shares a
//    cell with its run. Narrowed or widened at either end (6 cells wide to
//    5, or 5 to 6), two runs joined or one split, it is the step that was
//    and isn't added; a band repainted to another rung is.
//  - a skip, unless the held canvas had one the same way, on the same row
//    or column, between the same two cells, or between the same two rungs
//    (a to c) with its two levels each sharing a cell with its own
//    (Skip::matches). A level grown or shrunk at the boundary moves the
//    skip a cell or more along the line, and it is the skip that was; a
//    skip between another pair of rungs a cell over is added, and so is
//    one whose level moved past the old one's.
//  - any other finding at a place (LadderFinding::Place) where the held
//    canvas had none of that kind. A finding that only measures differently
//    where it was (a skip of other rungs between the same two cells, a
//    speck gone from lighter to darker) isn't added.
//  The whole-canvas list (findings(), checkLadder) still shows a finding
//  that wasn't added, with its new message.
//  - one glyph when the held canvas didn't use one glyph only.
//  - noise when the noise is over options.maxNoise and higher than it was.
// An index holds one canvas: after an edit it wasn't told of (undo from
// elsewhere, another tool, a layer switch), take a new one.
class LadderIndex {
public:
    // A skip along a line, as positions along it: i the last cell of the
    // level at rung a and j the first of the next, at rung c; the level at a
    // starts at first and the one at c ends at last.
    struct Skip {
        int i = 0, j = 0, a = 0, c = 0, first = 0, last = 0;
        bool operator==(const Skip&) const = default;
        // Whether this is `was` (on the same line), as apply matches skips:
        // between the same two cells, or moved along its levels (the same
        // two rungs, each level sharing a cell with was's).
        bool matches(const Skip& was) const {
            return (i == was.i && j == was.j) ||
                   (a == was.a && c == was.c && first <= was.i && was.first <= i && j <= was.last && was.j <= last);
        }
    };

    // Throws LintError for options.maxRung below 1.
    LadderIndex(const Canvas& canvas, const Ramp& ramp, const LadderOptions& options = {});

    // Moves the index to `after`, which differs from the canvas it holds
    // only at `changed` cells (a cell named twice counts once; cells off the
    // canvas are ignored), and returns what the change adds, as above.
    // Throws LintError, leaving the index as it was, when `after` isn't the
    // held canvas's size, and in a debug build (no NDEBUG) when `after`
    // differs at a cell `changed` doesn't name (differsOnlyAt, a whole-canvas
    // compare). A release build trusts `changed`: a cell it leaves out keeps
    // its old findings for the index's life.
    std::vector<LadderFinding> apply(const Canvas& after, const std::vector<std::pair<int, int>>& changed);
    // What apply would return, leaving the index where it is: an apply and
    // the update back, twice apply's cost. A guard that keeps most of its
    // dabs calls apply, and on a refusal applies the held cells back
    // (apply(before, changed) adds nothing it must look at).
    std::vector<LadderFinding> added(const Canvas& after, const std::vector<std::pair<int, int>>& changed);
    // Whether `after` is the held canvas's size and equal to it at every
    // cell `changed` doesn't name: apply's precondition, read over the whole
    // canvas (O(W x H)).
    bool differsOnlyAt(const Canvas& after, const std::vector<std::pair<int, int>>& changed) const;

    // The held canvas's wide rungs across row `line` (across true) or down
    // column `line`, in order along it: O(W) or O(H). A guard that compares
    // with an earlier canvas than the held one reads the lines it changed.
    std::vector<LadderFinding> wideRungs(bool across, int line) const;
    // The held canvas's skips along row `line` (across true) or down column
    // `line`, in order along it.
    const std::vector<Skip>& skips(bool across, int line) const;
    // The held canvas's skips along the line that match none of `was` (an
    // earlier canvas's skips along it, as skips() gave them; Skip::matches),
    // as apply judges them: one pass over both lists. A guard that compares with an
    // earlier canvas than the held one reads the lines its change reaches.
    std::vector<LadderFinding> skipsAdded(bool across, int line, const std::vector<Skip>& was) const;
    // Whether the held canvas has this finding (every field equal).
    bool has(const LadderFinding& finding) const;
    // checkLadder's findings of the held canvas over the whole of it.
    std::vector<LadderFinding> findings() const;
    const Canvas& canvas() const { return canvas_; }
    double noise() const { return total_ ? double(lonely_) / double(total_) : 0.0; }

private:
    // A wide step's width, its run's last cell along the line and its rung;
    // width 0 for none.
    struct Wide {
        int width = 0, last = 0, rung = -1;
        bool operator==(const Wide&) const = default;
    };
    // Positions 0..n-1, some set: the highest set one at or below a
    // position, in a few word reads (a 64-ary tree of bitsets).
    struct Highest {
        std::vector<std::vector<std::uint64_t>> levels;
        void reset(std::size_t n);
        void set(std::size_t i, bool on);
        long long below(std::size_t i) const;
    };

    std::vector<LadderFinding> update(const std::vector<std::pair<int, Cell>>& cells);
    int rungAt(int x, int y) const;
    int pixelAt(int p, int x) const;
    int contribution(int p, int x) const;
    void bump(bool row, int slot, int by);
    void placeRow(int i);
    void placeColumn(int i);
    bool judgedRow(int start) const;
    Wide judgeRow(int start) const;
    Wide judgeColumn(int start) const;
    int speckAt(int x, int y) const;
    std::vector<Skip> judgeSkips(bool row, int line) const;
    LadderFinding skipAt(bool row, int line, const Skip& s) const;
    std::vector<LadderFinding> globals() const;

    Canvas canvas_;
    Ramp ramp_;
    LadderOptions options_;
    int w_ = 0, h_ = 0;
    // per cell, y * w + x: the rung (-1 off the ladder), the run across the
    // row [rowFirst, rowEnd) and down the column [columnFirst, columnEnd)
    std::vector<int> rung_, rowFirst_, rowEnd_, columnFirst_, columnEnd_;
    // The width rule's runs the other way, bucketed: a row's run [a, b)
    // owns slots a..b-1 of its row (y * w + slot), and each of its cells
    // counts in slot a + min(its run down, b - a) - 1, so the highest
    // counted slot in [a, b) is the rung's longest run down across the run.
    // Columns the same way (x * h + slot), with the runs across.
    std::vector<int> rowSlot_, columnSlot_, rowCount_, columnCount_;
    Highest rowHighest_, columnHighest_;
    // A cell whose run across is judged (a row reports it if it is in a
    // judged column run), and per column run, at its first cell, how many
    // of its cells are.
    std::vector<char> judgedAcross_;
    std::vector<int> seen_;
    std::vector<Wide> rowWide_, columnWide_;  // at a run's first cell
    std::vector<std::vector<Skip>> rowSkips_, columnSkips_;  // per line, in order along it
    std::vector<char> speck_, shade_, fallback_;  // speck: 0, 1 lighter, 2 darker
    // one glyph: the shade cells, by glyph
    int shadeCells_ = 0;
    std::map<char32_t, int> glyphs_;
    // noise: each row's length with its trailing black spaces trimmed, the
    // pixels (2 rows per cell row, -1 for none) and what each counts (0,
    // 1 counted, 2 counted and lonely)
    std::vector<int> length_, pixel_;
    std::vector<signed char> counts_;
    long long lonely_ = 0, total_ = 0;
    // bookkeeping for one update
    unsigned epoch_ = 0;
    std::vector<unsigned> cellMark_, rowMark_, columnMark_, nearRowMark_, nearColumnMark_, rowRunMark_, columnRunMark_,
        pixelMark_, rowWideMark_, columnWideMark_, speckMark_, shadeMark_, fallbackMark_;
};

// What changing `before` to `after` at `changed` adds, as
// LadderIndex::apply judges it, from an index of `before` built for this
// one call: a whole-canvas pass every time (milliseconds on a large
// canvas). For tests and a one-off check; a brush judging dab after dab
// keeps a LadderIndex for the stroke.
std::vector<LadderFinding> addedFindingsOnce(const Canvas& before, const Canvas& after, const Ramp& ramp,
                                             const std::vector<std::pair<int, int>>& changed,
                                             const LadderOptions& options = {});

// How many reads the lint has made on this thread so far (a cell, its rung,
// a pixel, a run's bucket, a step of comparing a line's skips; checkLadder's
// rungs and runs as its rules walk them, detail's cells): a work counter, for
// tests of what a change costs to judge.
long long cellsRead();

#ifdef BW_TEST_HOOKS
// How many LadderIndexes have been built on this thread so far (each a
// whole-canvas pass): a work counter, for tests of when a tool builds one.
// Only in builds with the tests (BW_TEST_HOOKS).
long long indexesBuilt();
#endif

} // namespace bw::lint
