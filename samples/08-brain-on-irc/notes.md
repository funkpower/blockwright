# This Is Your Brain on IRC

One egg frying in a pan, seen from above, under the caption THIS IS YOUR BRAIN ON IRC. The yolk is a speech bubble with a # pressed into it. The yolk is a dome and the pan a cylinder, two of the three forms the Form brush knows, and each is one pass of that brush over a flat copy kept hidden beneath it. Fourteen layers: a locked black background, three of lettering, the pan twice, the egg white, the yolk twice, the # and the highlight, two of heat, and one hidden layer a variant uses.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| smoke | cells | normal | - | Three wisps of grey and dark grey shade rising off the pan. The outer two rise from a single cell to three across at the top; the middle one is three cells across on each of its three rows. |
| sizzle | pixels | normal | - | The fat spitting at the white's edge: two yellow arcs on each side of the egg and two white spits. At three of the places where an arc steps a pixel diagonally, a pixel joins the step, since the lint counts a lone pixel as noise. |
| highlight | pixels | normal | - | The light on the yolk from the upper left: a white crescent along its upper left, a glint two pixels wide at its lower left and one pixel at its upper right. |
| # | pixels | normal | - | The # pressed into the yolk in brown: two bars across and two strokes down, leaning right. |
| yolk | cells | normal | alpha | The yolk after one pass of the Form brush: a dome on the skin ramp at 65%: yellow over most of it, light red and brown along its top edge, and brown over red down its right side, the side away from the light. One cell at its tail is set to the shade of the cell above it. Alpha-locked, so a brush keeps to the yolk's own cells. |
| yolk (flat) | cells | normal | alpha | Hidden: the yolk as a speech bubble in flat yellow, its tail at the lower left. The Raw egg variant shows it. |
| egg white | cells | normal | - | The white: white in the middle, thinning to grey at its edge, a fringe of browned lace with the pan showing through it, four grey blisters, and the yolk's grey shadow down and to the right. |
| pan | cells | normal | alpha | The pan after one pass of the Form brush: a cylinder on the steel ramp at 100%. The ring, the lip on the left and the handle on the right, with a hole at its end, are white along most of their upper edges and black along most of their lower ones. Eleven cells along the upper and lower edges keep the flat dark grey, where the brush took back a change the lint would have reported: four on the inner edge at the top of the ring, four under the handle and three more on the ring's sides. One cell beside the sizzle is set to the shade of its neighbour. Alpha-locked, so a brush keeps to the pan's own cells. |
| pan (flat) | cells | normal | alpha | Hidden: the pan in flat dark grey. The Raw egg variant shows it. |
| signature | cells | normal | - | The tag in dark grey in the lower right corner. |
| tagline | cells | normal | - | Any questions? in white and /msg me. in yellow, under the pan. |
| caption | cells | normal | - | THIS IS YOUR BRAIN ON IRC in two rows of block letters, white with a grey foot and IRC in yellow with a brown foot. |
| margins | cells | normal | - | Hidden: a black block in the top left corner. The No caption variant shows it. |
| background | cells | normal | locked | Black, the whole canvas, which is also the floor of the pan. Locked, so painting the egg never touches it. |

Snapshots: r1 holds the lettering, the egg white and the pan and the yolk flat; r2 has the pan and the yolk after the Form brush; r3 is the finished piece with the #, the highlight, the sizzle and the smoke.

## Variants

| Variant | Changes |
|---|---|
| Raw egg | hide pan; show pan (flat); hide yolk; show yolk (flat); hide #; hide highlight; hide sizzle; hide smoke |
| No caption | hide caption; hide tagline; show margins |

## Try this

1. Make the Raw egg variant: hide pan, yolk, #, highlight, sizzle and smoke, and show pan (flat) and yolk (flat). The pan and the yolk go flat and the heat goes. The white keeps its browned lace, which is on the egg white layer. Then put the piece back: show pan, yolk, #, highlight, sizzle and smoke, and hide pan (flat) and yolk (flat). Reopening the sample does the same.
2. Hide # and highlight. What is left on the yolk is the brush's own work. They sit on layers of their own because the brush paints every cell it covers, and a # painted into the yolk would be shaded away with the rest. Show them again.
3. Hide yolk and show yolk (flat). Select yolk (flat), then Selection > Select All. Choose Tools > Form Brush, and set Tool Options to Dome, the skin ramp, light from NW, Size 1 and Strength 65%. The default Strength is 25%, which lets a cell move only a quarter of the ramp toward the dome, so no part of the yolk gets darker than light red. Glint is on by default and Form rim light is off; leave them so. Paint over the whole yolk. The alpha lock keeps the brush to the yolk's own cells. The result is close to the yolk layer: the brush takes back a change the lint would report, so the order in which the stroke reaches the cells changes some of them. A single drag that runs left to right along the top row and back along it, then down a row and the same again, to the bottom, gives the yolk layer apart from the one cell at its tail. Lifting the button between rows, or crossing straight from the end of one row to the start of the next, changes a dozen or more cells. Undo, hide yolk (flat) and show yolk.
4. Do the same with pan (flat): hide pan, show and select pan (flat), Select All, and paint over it with the Form Brush as a Cylinder on the steel ramp at 100%, light from NW, Size 1. A shape wider than it is tall counts as a log lying on its side, rounded down its columns, so each band of the ring comes out white along most of its upper edge and black along most of its lower one: the inner edge at the top of the ring and the outer edge at the bottom. The brush's lint check leaves a few cells on those edges, and more inside the bands, in the flat dark grey. As with the yolk, the result is close to the pan layer, and the same drag gives the pan layer apart from the cell beside the sizzle. Undo, hide pan (flat) and show pan.
5. Hide caption and tagline and look at the Lint dock: the eight rows above the smoke are blank, and IRC trims blank lines at the start of a piece, so it would arrive eight lines shorter than it was drawn. Show margins and the finding goes. A black block on black looks the same as a space, and it is kept. That is the No caption variant. Show caption and tagline and hide margins again.
6. Drag caption to the top of the stack. Nothing changes: no other layer shares a cell with the lettering, so its place in the stack only decides where it is in the Layers dock.
7. Select background and try to paint on it. It is locked; unlock it from the Layers dock if the pan should stand on something other than black.
8. Every line fits one IRC message. The longest, row 21, exports at 273 bytes and comes to 301 as sent, once the script that sends it has written every colour code again in two digits, against the project's budget of 407, so the piece arrives in the 32 lines it was drawn in.
9. The project keeps three snapshots: r1 holds the lettering, the egg white and the pan and the yolk flat, as the Raw egg variant shows them; r2 adds the Form brush; r3 is the whole piece. File > Take Snapshot adds r4.
