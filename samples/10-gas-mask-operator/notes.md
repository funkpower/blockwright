# Gas-mask operator

A soldier at an early-nineties desk, in a kevlar helmet and fatigues of one pale khaki cloth and a dark olive gas mask, turns from his computer and glances back over his shoulder through the mask's round lenses. Two putty-coloured towers with floppy slots and green lights, a monitor seen from behind, a spiral notepad, a keyboard and a mouse on its pad stand on the left; his gloved hand is on the mouse and his near shoulder fills the lower right. A Micron page in twelve-bit colour, every cell but the tag's a half block, so each cell is two pixels: the light falls from the upper right, the screen's cool light catches the edges of the mask and the hand that face it, and the only white in the picture is in the eyes. Seventeen layers: a locked wall, fourteen layers of desk, hardware and figure, a colours-only glow, and the tag.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| tag | cells | normal | - | funkpower in light grey on the wall, row 1, columns 70 to 78. |
| glow | cells | colours-only | - | The screen's cool light, columns 17 to 40, rows 13 to 28: the left edge of the mask and of the hand, a pixel deep, and a pool on the notepad, the pad and the mouse's cord in front of the monitor. It holds colours and no glyphs, so with it hidden every cell keeps its shape and loses the blue. No cell with an eye in it is touched. |
| eyes | cells | normal | - | The eyes behind the lenses, columns 42 to 59, rows 14 to 16: a dark lid across the top, a white and a brown iris turned to the right, back toward the viewer, a glint on each glass and the cheek under them. The only white in the picture. |
| helmet | cells | normal | - | The kevlar helmet, columns 36 to 70, rows 2 to 16: a low dome of pale khaki over the brow and down over the ears, olive in shade down its left side, one seam from the crown to the band, and the dark band round it. |
| shoulder | cells | normal | - | The cloth over the chest beside the mask and the big near sleeve in the lower right, columns 46 to 79, rows 14 to 41: pale khaki with inked folds, olive along the sleeve's inner side, and the dark pack strap arcing over the shoulder and down. |
| canister | cells | normal | - | The filter canister low on the mask's snout, columns 41 to 53, rows 18 to 25: a ribbed connector, then a grey disc with a lit rim, ribs from its hub, and the dark side of its drum showing below and to the left. |
| mask | cells | normal | alpha | The gas mask, columns 36 to 66, rows 13 to 24: dark olive rubber, lit down the middle, in shade on the left and black under the helmet's brow, and two steel-rimmed round lenses in dark sockets. Alpha-locked, so paint stays on the mask's own cells. |
| body | cells | normal | - | The arm reaching from the shoulder to the mouse and the gloved hand on it, columns 29 to 56, rows 22 to 32: khaki, the knuckles lit, ink between the fingers draped over the mouse, the heel of the hand in shade. |
| keyboard | cells | normal | - | The keyboard turned at an angle in the lower left, columns 0 to 31, rows 27 to 40: a putty case round a dark bed of grey keys, the main keys and a keypad to the right. |
| mouse | cells | normal | - | The mouse pad, dark teal, columns 13 to 47, rows 23 to 38, the pale mouse on it under the fingers, and its cord curling back past the notepad toward the towers. |
| notepad | cells | normal | - | A spiral notepad behind the pad, columns 17 to 31, rows 21 to 26: pale paper, a row of rings along its top and the edge of the pages along its foot. |
| screen | cells | normal | alpha | The screen's light round the right edge of the monitor's case, columns 34 and 35, rows 13 to 20: the screen faces him, so its light shows only as a blue strip. Alpha-locked, so paint stays on the strip. |
| CRT | cells | normal | - | The monitor from behind, columns 19 to 37, rows 9 to 21: a putty top, a cool grey back with its moulded panel, and the edge of the case down its left. |
| towers | cells | normal | - | Two putty towers, columns 0 to 18, rows 10 to 29: the tall one seen from its front corner, its top, its plain face and its drive face with two floppy slots and a green light, and the short one at the left edge with a green light. |
| figure shadow | cells | normal | - | The figure's shadow on the wall to the left of the helmet, columns 34 to 63, rows 4 to 20, a step darker than the wall; most of it lies behind the monitor and the figure. |
| desk | cells | normal | - | The desk from row 21 down, dark brown, darker along its back edge. |
| wall | cells | normal | locked | The wall: slate, with a wedge of light falling from the upper right behind the figure. Locked, so painting the room never touches it. |

Snapshots: r1 holds the room and the hardware on the desk; r2 adds the figure, the mask, the canister and the helmet; r3 is the finished piece with the eyes, the glow and the tag.

## Variants

| Variant | Changes |
|---|---|
| No glow | hide glow |
| Power cut | hide screen; hide glow |
| Blank lenses | hide eyes |
| Empty chair | hide figure shadow; hide body; hide mask; hide canister; hide shoulder; hide helmet; hide eyes; hide glow |

## Try this

1. Hide glow, as the No glow variant does. The cool edge along the left of the mask and the hand goes back to the same green and khaki as the rest of the figure, and the blue goes off the notepad, the pad and the cord. Every glyph stays: the layer holds colours only.
2. Keep glow hidden and hide screen as well, as the Power cut variant does. The blue strip on the monitor's edge goes back to grey, and with the screen's light gone the figure is lit from the upper right alone.
3. Hide eyes, as the Blank lenses variant does. The lenses go dark and the face has nowhere to look; the eyes layer is three rows of cells and the whole of the picture's focus.
4. Hide every layer of the figure, as the Empty chair variant does. The desk, the hardware and the wall stay as they were: the figure is drawn on its own layers over the room.
5. Select mask and paint across the face. The alpha lock keeps the paint on the mask's own cells, so the helmet and the wall beside it stay clean. Undo the paint before the next step.
6. Select glow and paint over a cell of the lit edge in another colour. The layer is colours only, so the half block underneath keeps its shape and takes the new colour. Undo the paint before the next step.
7. Export the page as Micron. Every colour in it is one of the 4096 twelve-bit colours a page can write, three digits each, so nothing is rounded on the way out.
