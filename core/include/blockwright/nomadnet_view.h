#pragma once
// The NomadNet view (PLAN §17): a Micron page as NomadNet draws it in its
// 256-colour mode.
//
// NomadNet 1.5.0 (nomadnet/ui/textui/MicronParser.py, make_style) gives
// urwid every colour of a page as "#rrggbb" (high_color: a three-digit
// code's digits doubled, a truecolour one as it is) and registers it with
// Screen.register_palette_entry; in 256-colour mode urwid's AttrSpec reads
// it through _true_to_256 and _parse_color_256 (urwid/display/common.py,
// the same from urwid 3.0.5, the oldest NomadNet 1.5.0 takes, to 4.2.5):
// each channel's high nibble picks one of the six steps of xterm's colour
// cube (_CUBE_256_LOOKUP_16, nibble 0-2, 3-6, 7-9, 10-11, 12-13, 14-15),
// and the terminal draws the cube colour at that index. A page's colours
// never reach the 16 basic colours or the grey ramp, a grey ones included.

#include "blockwright/canvas.h"
#include "blockwright/palette.h"

#include <array>

namespace bw::nomadnet {

// The xterm 256-colour index urwid sends for `c` in 256-colour mode: a
// colour-cube index, 16-231.
int index256(Rgb c);

// The colour a terminal draws for index256(c): xterm's cube, whose steps
// are 00 5f 87 af d7 ff, the same in every terminal with a 256-colour
// palette (only the basic 16 vary, which a page never gets).
Rgb colour256(Rgb c);

// The page of `canvas` as NomadNet draws it at 256 colours: micron::pageCanvas
// (each cell as Export Micron writes it, a glyph NomadNet strips as '?')
// with every colour through colour256.
Canvas view256(const Canvas& canvas, const std::array<Rgb, 16>& palette, bool ice);

} // namespace bw::nomadnet
