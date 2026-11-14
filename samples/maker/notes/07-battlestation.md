# Battlestation

A night desk seen from behind the chair. A wide monitor running a tiling layout is the only light in the room, and the blue it throws on the wall, the desk and the rack sits on a colours-only layer of its own, so the light can go out without a glyph moving. Eleven layers: a locked wall, nine layers of desk, hardware, light and chair on it, and one hidden layer a variant uses.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| signature | cells | normal | - | The tag in dark grey in the lower left corner. |
| chair | cells | normal | - | The racing chair from behind: a dark grey back under a grey headrest with a light cyan top, its sides trimmed in light blue and blue, the armrests and the base. |
| glow | cells | colours-only | - | The blue the screen throws: the rings on the wall round the monitor, the desk top and the rack's side and top edge toward the screen. In its colours-only mode it gives colours and no shapes; the glyphs are the layers' under it, so with it hidden the same cells show in the dark colours those layers have. |
| rack | cells | normal | - | The home-lab rack on the right: a dark grey cabinet with a grey post down its left side, units with light green, yellow, light red and light cyan lights, and a light green display at the foot. Its side and top edge toward the screen are dark grey here, and the side black toward the foot; the glow lights it. |
| mug and mouse | cells | normal | - | A dark grey mug of coffee left of the keyboard, its rim and right side catching light blue and light cyan, and a mouse with a light blue top on a pad to the right. |
| keyboard | cells | normal | - | Grey keys on a dark grey board edged in light blue, a light magenta key at the top left and a light cyan one on the right, and an underglow strip along the front in light cyan, light blue and light magenta. |
| screen | cells | normal | alpha | The tiling layout: a top bar with workspace markers, a title and a status block; a wallpaper banded from magenta down to blue, a row of magenta and then three rows a band; a code pane in a light cyan border, with dark grey line numbers, a bar of colour for each word and a highlighted line; a terminal pane with its cursor; and a pane of light green bars with yellow and light red peaks. No word on it can be read. Alpha-locked, so paint stays on the screen's own cells. |
| monitor frame | cells | normal | - | The bezel in dark grey with a light blue top edge, a light cyan power light at the lower right, and the neck of the stand. |
| desk | cells | normal | - | The desk with the screen off: a dark grey back edge, three rows of dark grey shading thinning toward the front, black from there to the front edge, and two dark grey legs. The glow puts the blue under the shading and on the front half. |
| margins | cells | normal | - | Hidden: a black block in the bottom left corner. The Screen only variant shows it. |
| wall | cells | normal | locked | The wall, black, with the shading of the rings round the monitor already in place in black on black, and a black block in the top left corner. Locked, so painting the desk or the screen never touches it. |

Snapshots: r1 holds the dark room with the monitor on; r2 adds the keyboard, the mug and mouse, and the rack; r3 is the finished piece with the glow, the chair and the signature.

## Variants

| Variant | Changes |
|---|---|
| No glow | hide glow |
| Lights off | hide glow; hide screen |
| Screen only | hide desk; hide keyboard; hide mug and mouse; hide rack; hide glow; hide chair; hide signature; show margins |

## Try this

1. Hide glow (the No glow variant). The screen is still on and nothing it shines on notices: the wall goes black, the desk top drops to dark grey on black and the rack's side to its own dark grey, and to black toward the foot. Every glyph stays where it was. Show glow and the piece is back, cell for cell.
2. Select glow and paint over the rings on the wall in another colour. The layer is colours only, so the shading under the brush keeps its glyphs and takes the new colour: a different wallpaper's light, without drawing the rings again.
3. Hide glow and screen (the Lights off variant). The monitor is a dark rectangle. The keyboard's underglow stays on, as it has its own light, and so do the light blue edges on the bezel, the mug, the mouse and the chair, which the layers give them whether or not anything is lit.
4. Select screen and paint across a pane. The alpha lock keeps the paint on the screen's own cells, so the layout can be recoloured without touching the bezel or the wall behind it.
5. Hide everything but the wall, the monitor frame and the screen, and look at the Lint dock: the rows under the stand are blank, and IRC trims blank lines at the end of a piece, so it arrives shorter than it was drawn. Show margins and the finding goes. A black block on black looks the same as a space, and it is kept. That is the Screen only variant.
6. Select wall and try to paint on it. It is locked; unlock it from the Layers dock if you mean to move the wall.
7. The code pane has no letters in it, only bars of colour where the words would be, so it can be pasted into any channel without showing anyone's code. Few screenshots of a terminal can say as much.
8. The project keeps three snapshots: r1 holds the dark room with the monitor on; r2 adds what stands on and beside the desk; r3 is the whole piece. File > Take Snapshot adds r4.
