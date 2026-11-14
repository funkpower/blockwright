# DDOS KING

A king on a throne of server racks, crowned and in shades, holding up an ethernet cable for a sceptre while packet lightning arcs over him, with his name in gold across the top in case the lightning left any doubt. Fourteen layers: a locked backdrop, eleven layers of throne, figure, lightning and lettering on it, and two hidden layers the variants use.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| signature | cells | normal | - | The tag in grey in the lower right corner, inside the frame. |
| border | cells | normal | - | The frame: DDOS KING repeated between rising and falling block waves along the top and bottom, the chant stacked a letter a row down the sides, in the lightning's colours. |
| title | cells | normal | - | DDOS KING in square capitals 12 pixels high, strokes two pixels thick, gold by pixel row: a white top edge, yellow to the middle, brown below. The round corners of D and O are cut by a two-pixel stair, those of S and G by a single pixel; K, I and N are square. |
| packets | cells | normal | - | Green packet blocks riding the bolts. |
| finials | cells | normal | - | Gold balls on the rack corners. They sit above the lightning, so no bolt crosses them. |
| lightning | cells | normal | - | White bolt cores a pixel wide in a light cyan glow, from the sceptre's plug out over the throne. |
| head | cells | normal | - | The face, the black shades, the grin and the gold crown with its jewels. |
| sceptre | cells | normal | - | A yellow ethernet cable topped by an RJ45 plug, held up in the king's right hand. |
| king | cells | normal | - | The body: a purple robe lit from the left, the ermine collar and hem, the gold belt, the sleeves, the hand on the armrest and red boots. |
| cable tips | cells | normal | - | Hidden: half a cell more yellow on top of four cable runs, where they stop against the blue. The No lightning variant shows it. |
| cables | cells | normal | - | Bundles of yellow cables curving out of each armrest into the raised floor, with black ties. |
| throne | cells | normal | - | The tall back rack, the flanking and armrest racks with their LEDs, the dais, the panel behind the head and the raised floor. |
| margins | cells | normal | - | Hidden: black blocks over the frame's rows and its last column. The variants without the border show it. |
| backdrop | cells | normal | locked | The black and the blue the piece stands on, with the shaded fade at the top of the blue. Locked, so painting the throne or the lightning never touches it. |

Snapshots: r1 holds the throne and its cables on the backdrop; r2 adds the king, his sceptre and his head; r3 is the finished piece with the lightning, the title and the frame.

## Variants

| Variant | Changes |
|---|---|
| No lightning | hide lightning; hide packets; show cable tips |
| No border | hide border; show margins |
| Title only | hide backdrop; hide throne; hide cables; hide king; hide sceptre; hide head; hide lightning; hide finials; hide packets; hide border; hide signature; show margins |

## Try this

1. Hide lightning and packets. The king still reads, and the throne is still lit from the upper left, now by nothing in particular. Then look at the Lint dock: where four cable runs stop under the blue, the yellow meets the blue straight on and the lint reports a gradient that skips rungs. Show cable tips and the finding goes: each run now ends half a cell higher, in a half block.
2. Select backdrop and try to paint on it. It is locked; unlock it from the Layers dock if you mean to change the sky.
3. Hide border and look at the Lint dock. The frame's rows are now blank and some rows end in black spaces, and IRC has no use for either: blank lines don't get sent and trailing spaces get trimmed, so the piece arrives a different shape. Show margins and the findings go. A black block on black looks the same as a space, and it is kept.
4. Hide everything but title and margins (the Title only variant): the lettering stands on its own, ready to save to the Library as a stamp. On IRC its rows cost about as many bytes alone as in the full piece; up there the gold is the expensive part.
5. The project keeps three snapshots: r1 holds the throne and its cables on the backdrop; r2 adds the king, his sceptre and his head; r3 is the whole piece. File > Take Snapshot adds r4.
6. Select lightning and draw a bolt of your own with the white and light cyan from the palette. The layers under it don't change, and hiding the layer takes your bolt with the rest. Join the glow pixels edge to edge; a light cyan pixel on its own in the blue reads as noise, not light.
7. Every layer is plain cells, with nothing on it that redraws itself, so an edit stays as you leave it. Retouch the crown on head; the panel behind his head is on throne and doesn't change.
