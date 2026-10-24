#pragma once
// `blockwright-cli shade`: one pass of the ramp shade brush over a box, with
// the iCE colours the editor's ramp brush would use on the same document.

#include "blockwright/ansi.h"
#include "blockwright/brushes/shading.h"
#include "blockwright/edit.h"
#include "blockwright/lint.h"
#include "blockwright/ramp.h"

#include <optional>
#include <string>

namespace bwcli {

struct ShadeOptions {
    bw::Ramp ramp;
    bw::ShadeDirection direction = bw::ShadeDirection::Lighten;
    int steps = 1;
    std::optional<bw::lint::Box> box;
    // --ice: turn the document's iCE colours on before shading, so the brush
    // may write shades that need a bright background.
    bool ice = false;
    bool allowLossy = false;
};

struct Shaded {
    bw::CellEdit edit;  // already applied to the document
    bw::lint::Box box;  // the box, clipped to the canvas
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

// Shades `doc`, as parsed from the file, in place: opens it as the editor
// does (openAsEditor: iCE colours on for a bright background, which then
// stay on whatever the shading does, and blinking cells without their
// blink), turns iCE colours on with o.ice, then runs one pass over o.box
// (the whole canvas without one) with the document's iCE colours. With them
// off no cell is given a bright background, so the piece stays without iCE
// colours. Throws Failure when the box is off the canvas.
Shaded shadeDocument(bw::AnsiDocument& doc, const ShadeOptions& o);

} // namespace bwcli
