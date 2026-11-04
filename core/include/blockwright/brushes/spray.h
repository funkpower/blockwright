#pragma once
// Glyph spray (PLAN §10, "Detail"): stars, haze and smoke scattered at a
// density. Like the texture stamp, each kind is a field over the whole
// canvas fixed by its seed, and the brush uncovers it where it passes: dabs
// that overlap and a drag aim at the one field, and a stroke over another
// takes cells only toward that field and never past it, so nothing piles up
// (below, for what the guard holds back and what a repeat can change).
//
// Stars: one candidate cell in each 8 x 4 block of the canvas, jittered
// inside the block so two are never closer than five columns or three rows;
// a candidate is a star with the density's chance. A star is one of
// . · ∙ + * ☼ (all CP437), small dots far more often than the rest, in the
// pen's foreground on the ground under it. It lands only on open ground,
// four pixels off every shape, so no star touches a form: the cell and every cell within
// four columns and two rows of it read as one flat colour (a space, a full
// block or a shade of one colour, not blinking), a star already there
// counting as its background. So a star never touches a shape or sits in a
// dithered band. The test reads the canvas as the stroke began, so which
// cells a stroke may star doesn't depend on the order of its dabs (but see
// the guard, below, for which of them land). A star can't be written on a bright
// ground with iCE off (it would need a bright background) or on ground of the
// pen's own colour. The right button takes stars off: any of the six glyphs
// under the brush becomes a space on its background, whoever put it there
// (a . or * typed or drawn before the spray goes too). Stars and their wipe
// go through the ladder lint's guard (StrokeLint, guard.h) like haze: each
// star, with its mirror images, is judged as it lands, and one that would add
// a finding to the stroke's start (a noisy canvas's noise, say) is held back.
// Each dab tries again the held stars and wipes within four columns and four
// rows of a cell of its footprint (or of an image's; for a round tip, of the
// round, not the box round it), in row-major order, until a pass lands none,
// so a held one in reach that a change in this dab frees lands in the same
// dab. Noise is read over the whole canvas, so stars compete for its budget:
// where it binds, which stars land depends on the order the stroke tries
// them, and the same dabs in another order can land a different set.
//
// Haze and smoke: a ░▒ lift along the ramp. The field is one octave of
// seeded value noise: for haze a slow horizontal wave (20 columns by 3 rows
// a lattice step, "one layer ... thinning to ░ on one slow wave"), for smoke
// tall wisps 4 columns by 10 rows, curled sideways by a second noise along
// the rows. The density is the share of the canvas the field covers, by a
// quantile of the field's values; a covered cell with fewer than two
// covered neighbours (four ways) is dropped and an uncovered one with three
// or more is filled, so the cover has no lone cells or pin holes. A covered
// cell is lifted two rungs (▒) where all eight of its neighbours are
// covered and one (░) on the rim, and a ▒ cell with no ▒ beside it (four
// ways) is ░: so ░ edges every patch, a rung apart from what it covers and
// from the ▒ inside it. Off the canvas the field repeats its edge cells.
// The left button lightens: a cell at rung r goes to its colour's solid rung
// (the rung r rounds down to on the ladder) plus the lift, if that is above
// r; the right button darkens to the solid rung r rounds up to, minus the
// lift. The target is absolute, so painting again over a cell already there
// changes nothing, and haze over a ░ that is lighter already leaves it. The
// top colour can't be lightened, black can't be darkened, and cells off the
// ramp, blinking or transparent are left; with iCE off a cell goes only as
// far as the rungs it can write run unbroken from its own.
//
// The stroke's lint guard (StrokeLint, guard.h) decides what lands. After
// any dab the canvas has no ladder lint finding the stroke's start didn't
// have: a change is kept only when the guard finds nothing new in it. A
// cell only moves toward its rung of the field, never past it, and one the
// stroke has reached and not yet taken there is held. A dab tries the held
// cells that lie within four columns and four rows of a cell of its
// footprint (or of an image's): those of this dab, and those earlier dabs
// held back. It tries no others, so a dab's work is its neighbourhood's
// however long the stroke, and a held cell further off waits for a dab near
// it, even when a cell this dab keeps is what frees it. They are judged
// together; the cells a finding blames (a speck's cell, or the changed
// cells round it; the changed cell of a skip; a wide rung's run; any for
// one glyph and noise) go back a rung toward where they were, with their
// mirror images, and the rest are judged again until clean. Then each cell
// still short is tried alone a rung further, with its mirror images, row by
// row, and the round repeats while any cell moves. Every keep moves each
// of its cells nearer the field, so the round ends, and when it does no
// cell in reach can go a rung further by itself (with its images) without
// a finding.
//
// So the density sets the field, not how much of it lands. How much a
// stroke lands depends on the dab's shape, where it sits (a cell on an edge
// or in a corner has fewer neighbours to be a speck against), the order the
// stroke meets the cells, what is already on the canvas and the lint rules;
// at high density on a flat canvas much of a stroke can be held back. For
// instance, at 100% on a blank 40 x 20 canvas, where the field is all ▒, a
// 3 x 3 dab in the middle lands nothing, while a 2 x 2 dab in the top left
// corner lands three ▒ and a ░.
//
// What is held back at the stroke's end stays. A cell the stroke leaves at
// the field stays there under a second stroke, and a second stroke moves
// only cells the first left short, toward the field and never past it. It
// can move some: the guard judges a stroke against its own start, and its
// rules (a wide rung the start already had, one glyph and noise, read over
// the whole canvas) don't add up across strokes, so a cell the first stroke
// had to hold back can be clean against the canvas the first stroke left.
// Making every repeat a no-op would mean judging each stroke against some
// canvas other than its start, and giving up that guarantee. Which cells
// give way depends on the order the stroke meets them, and the field's edge
// where a stroke stops can be held back where a wider stroke would take it.
//
// Under a mirror symmetry the field is folded about the mirror axes, as the
// texture stamp's: every cell reads it at its fold in the left (top) half,
// so the field a stroke and its images uncover is its own mirror image, and
// a star whose mirror partner would be one to four columns (one or two
// rows) from it across the fold is dropped. The dab's images are judged
// together, as the smudge brush's, and a cell is tried with its images; on
// a flat canvas, for instance, a mirrored drag paints the exact mirror
// image. Radial images read the plain field at their own cells. Size and
// pressure set the footprint; strength is ignored.

#include "blockwright/brush.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/ramp.h"

#include <optional>
#include <string_view>

namespace bw {

enum class SprayKind { Stars, Haze, Smoke };

constexpr int kMinSprayDensity = 1, kMaxSprayDensity = 100;  // percent
constexpr int kMaxSpraySeed = 99999;

struct SprayOptions {
    SprayKind kind = SprayKind::Stars;
    int density = 30;  // percent: of the candidate cells (stars), of the canvas (haze, smoke)
    int seed = 1;      // 0 to kMaxSpraySeed

    bool operator==(const SprayOptions&) const = default;
};

// The six star glyphs (never a cluster or the continuation).
bool isSprayStar(Glyph g);

// The field's star at cell (x, y) of a canvas `width` x `height` (0: no edge
// that way), or empty; always empty for haze and smoke. `foldX` (`foldY`)
// folds the field across the canvas's centre column (row) as a mirror
// symmetry paints it. Density and seed are taken within their ranges.
std::optional<char32_t> sprayStar(const SprayOptions& options, int x, int y, int width = 0, int height = 0,
                                  bool foldX = false, bool foldY = false);

// The field's lift at cell (x, y): 0, 1 (░) or 2 (▒); always 0 for stars.
int sprayLift(const SprayOptions& options, int x, int y, int width = 0, int height = 0, bool foldX = false,
              bool foldY = false);

class SprayBrush : public Brush {
public:
    // `ink` is the stars' colour; `direction` Darken takes stars off, and
    // darkens with haze and smoke.
    SprayBrush(Ramp ramp, SprayOptions options, Color ink, ShadeDirection direction = ShadeDirection::Lighten);

    std::string_view name() const override { return "spray"; }
    void dab(Stroke& stroke, const Dab& d, int size, double strength) const override;
    std::optional<Cell> paintCell(const Stroke& stroke, int x, int y, double strength) const override;

private:
    Ramp ramp_;
    SprayOptions options_;
    Color ink_;
    ShadeDirection direction_;
};

} // namespace bw
