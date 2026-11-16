# Gas-mask operator

A soldier at an early-nineties desk, in a tan helmet, khaki fatigues and a dark green gas mask, turns from his computer and looks back over his shoulder through the mask's round eyepieces. Two putty-coloured towers with drive bays and green lights, a monitor with a blue text screen, a keyboard, a notepad and a mouse on its pad stand on the left; the figure fills the right. A Micron page in twelve-bit colour, every cell but the tag's a half block, so each cell is two pixels: light falls from the upper left, the screen's blue light catches the edge of the figure that faces it, and the only white in the picture is in the eyes. Seventeen layers: a locked wall, fourteen layers of desk, hardware and figure, a colours-only glow, and the tag.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| tag | cells | normal | - | funkpower in light grey on the wall, row 1, columns 70 to 78. |
| glow | cells | colours-only | - | The monitor's blue light, columns 14 to 52, rows 9 to 35: the left edge of the helmet, mask, canister, body and sleeve, three pixels deep, a pool on the keyboard, notepad, mouse and pad in front of the screen, and the side of the tall tower that faces the monitor, columns 16 to 20, rows 21 to 24. It holds colours and no glyphs, so with it hidden every cell keeps its shape and loses the blue. No cell with an eye in it is touched. |
| eyes | cells | normal | - | The eyes, columns 46 to 62, rows 18 and 19: white under a black upper lid, the pupils hard against the lid on the right, toward the viewer. The only white in the picture. |
| helmet | cells | normal | - | The helmet, columns 37 to 72, rows 6 to 16: a tan dome lit from the upper left with a pale highlight, in shade toward the lower right, and the brim round its foot a step darker, darkest along its underside. |
| shoulder | cells | normal | - | The near shoulder and back in the lower right, columns 52 to 79, rows 25 to 41: khaki, lit on the shoulder's crown and in shade toward the right edge, with a dark strap across it. |
| canister | cells | normal | - | The filter canister on the front of the mask, columns 46 to 60, rows 21 to 28: a steel face with a highlight to the upper left, a dark groove ring and hub, and its side showing to the right. |
| mask | cells | normal | alpha | The gas mask, columns 41 to 68, rows 14 to 27: dark green rubber, lit on the left cheek, in shade down the right and almost black under the brim, and two steel eyepiece rims round black lenses. Alpha-locked, so paint stays on the mask's own cells. |
| body | cells | normal | - | The back in shade under the mask, and the sleeve reaching down to the keyboard, lit along its top, with a green rubber glove on the keys; columns 26 to 66, rows 28 to 41. |
| keyboard | cells | normal | - | The keyboard in the lower left, columns 0 to 33, rows 30 to 41: putty, a pale lip along the top and a shaded right edge, and staggered rows of grey keys. |
| mouse | cells | normal | - | The mouse pad, dark teal, columns 21 to 40, rows 24 to 31, and the mouse on it with its cable running back to the monitor. |
| notepad | cells | normal | - | A notepad between the near tower and the pad, columns 13 to 22, rows 25 to 29, ruled in pale blue. |
| screen | cells | normal | alpha | The monitor's screen, columns 23 to 33, rows 12 to 18: a blue text page with a cyan title bar, pale blue lines and one picked in yellow. Alpha-locked, so paint stays on the screen. |
| CRT | cells | normal | - | The monitor, columns 15 to 37, rows 8 to 24: a putty case with its top and left side showing, a dark glass, a badge and a green power light, and the foot it stands on. |
| towers | cells | normal | - | Two putty towers, columns 0 to 21, rows 10 to 35: the tall one behind with two drive bays, a floppy slot, a green and a dim green light and vents, the short one in front with a bay and two green lights. |
| figure shadow | cells | normal | - | The figure's shadow on the wall, down and to the right of the helmet, columns 43 to 76, rows 8 to 22, a step darker than the wall. |
| desk | cells | normal | - | The desk from row 22 down, brown, a darker strip along its back edge. |
| wall | cells | normal | locked | The wall: cool slate, with a warm wedge of light falling from the upper left behind the hardware. Locked, so painting the room never touches it. |

Snapshots: r1 holds the room and the hardware on the desk; r2 adds the figure, the mask, the canister and the helmet; r3 is the finished piece with the eyes, the glow and the tag.

## Variants

| Variant | Changes |
|---|---|
| No glow | hide glow |
| Power cut | hide screen; hide glow |
| Blank lenses | hide eyes |
| Empty chair | hide figure shadow; hide body; hide mask; hide canister; hide shoulder; hide helmet; hide eyes; hide glow |

## Try this

1. Hide glow, as the No glow variant does. The cool edge along the left of the mask, helmet and sleeve goes back to the same khaki, green and steel as the rest of the figure, and the blue goes off the keyboard and the pad. Every glyph stays: the layer holds colours only.
2. Keep glow hidden and hide screen as well, which is the Power cut variant. The monitor is dark glass, and with the light gone the figure is lit from the upper left alone.
3. Hide eyes, as the Blank lenses variant does. The lenses go black and the face has nowhere to look; the eyes layer is two rows of cells and the whole of the picture's focus.
4. Hide every layer of the figure, as the Empty chair variant does. The desk, the hardware and the shadowless wall stay as they were: the figure is drawn on its own layers over the room.
5. Select mask and paint across the face. The alpha lock keeps the paint on the mask's own cells, so the helmet and the wall beside it stay clean. Undo the paint before the next step.
6. Select glow and paint over a cell of the screen-lit edge in another colour. The layer is colours only, so the half block underneath keeps its shape and takes the new colour. Undo the paint before the next step.
7. Export the page as Micron. Every colour in it is one of the 4096 twelve-bit colours a page can write, three digits each, so nothing is rounded on the way out.
