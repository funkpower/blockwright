#pragma once
// `blockwright-cli shade`: one pass of a shading brush over a selection (a
// box, a mask or both), with the iCE colours the editor's brush would use
// on the same document: the ramp shade brush's rung step, or the form,
// despeckle or rim-light brush.

#include "blockwright/ansi.h"
#include "blockwright/brush.h"
#include "blockwright/brushes/form.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/edit.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"

#include <optional>
#include <string>

namespace bwcli {

// What a pass does: one a call.
enum class ShadeMode {
    Step,       // the ramp shade brush: every cell `steps` rungs lighter or darker
    Form,       // the form brush: the selection shaded as a dome, cylinder or plane
    Despeckle,  // the despeckle brush over the selection
    Rim,        // the rim-light brush over the selection
};

struct ShadeOptions {
    bw::Ramp ramp;
    ShadeMode mode = ShadeMode::Step;
    // Step
    bw::ShadeDirection direction = bw::ShadeDirection::Lighten;
    int steps = 1;
    // Form: the shape and the Brush dock's Glint toggle
    bw::FormShape form = bw::FormShape::Dome;
    bool glint = false;
    // Form and Rim: where the light comes from (BrushSettings::lightDirection)
    bw::Compass light = bw::Compass::NW;
    // The selection: the box, the mask (the size of the canvas), both
    // (their intersection) or neither (the whole canvas).
    std::optional<bw::lint::Box> box;
    std::optional<bw::Mask> mask;
    // --ice: turn the document's iCE colours on before shading, so the brush
    // may write shades that need a bright background.
    bool ice = false;
    bool allowLossy = false;
};

struct Shaded {
    bw::CellEdit edit;  // already applied to the document
    // The box the pass covered and the check reads: the selection's bounds,
    // or the box clipped to the canvas when the selection is empty.
    bw::lint::Box box;
    // The input had iCE colours off and the document is now written with
    // them on (--ice, or bright backgrounds it already held).
    bool iceTurnedOn = false;
    // Cells that lost their blink when the document was opened as the editor
    // opens it (openAsEditor) and still don't blink once shaded, as the
    // editor counts them (lostBlinkCount); writeAnsi no longer counts them.
    int blinkDropped = 0;
    // Why a lossy result is refused, when the shading made it lossy; empty
    // when it was lossy before.
    std::string why;
};

// A mask piece's shape: every cell that isn't blank as transparent paste
// reads it (isBlank: a space or NUL on black without blink).
bw::Mask maskOf(const bw::Canvas& c);

// Shades `doc`, as parsed from the file, in place: opens it as the editor
// does (openAsEditor: iCE colours on for a bright background, which then
// stay on whatever the shading does, and blinking cells without their
// blink), turns iCE colours on with o.ice, then runs one pass of o.mode's
// brush over the selection with the document's iCE colours. With them off
// no cell is given a bright background, so the piece stays without iCE
// colours.
//
// Step paints every selected cell as a drag over it does (paintRegion).
// Form, Despeckle and Rim are one full-strength stroke of a square tip
// covering the selection, as one dab over the selection does in the
// editor: the form brush shades the selection's shape (formRungs) lit from
// o.light and keeps its lint guard, so the pass adds no ladder finding the
// input didn't have; despeckle clears the specks its guard allows; the rim
// light lights the shape's edge cells that face o.light (an accent, not
// held to the lint, as the editor's default). A selection wider or taller
// than the widest odd tip (255 cells) takes a dab per 255-cell square,
// rows from the top. Throws Failure when the box is off the canvas or the
// mask isn't the canvas's size.
Shaded shadeDocument(bw::AnsiDocument& doc, const ShadeOptions& o);

} // namespace bwcli
