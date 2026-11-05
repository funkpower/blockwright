# Traveller

A traveller with a staff and a pack, looking out over a meadow toward the hills, drawn in half-block pixels on layers of their own: an outline, the body, the face, the highlights and the shadow on the grass, all one size and in one place over a locked scene, with a hidden copy of the whole figure merged onto one layer to move with a single Alt+drag.

Alt+drag moves the active layer by whole cells, and Blockwright has no layer groups yet, so the figure comes two ways. Its five parts can each be moved by the same distance, one after another; the status bar shows how far each has gone. Or show figure (merged) and hide the parts (the One layer variant): the picture is the same, and one drag moves all of it.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| figure (merged) | pixels | normal | - | Hidden: the five layers below merged onto one, the same size and in the same place, so a single Alt+drag moves the whole figure |
| highlights | pixels | normal | - | Small clusters of light where the sun from the upper left catches the crown of the hat, the upper arm and the tip of the staff |
| face detail | pixels | normal | - | The eye and the mouth, over the skin |
| body | pixels | normal | alpha | The figure in flat colours, a colour a part: hat and band, grey hair, skin, scarf, a red coat lit down its front edges, a green pack lit along its top, trousers, boots, glove and staff. Alpha-locked, so paint stays inside the figure |
| outline | pixels | normal | - | The whole silhouette grown a pixel all round, in black, under the body: it parts the figure from the sky and the grass and fills the shade under the arm |
| shadow | pixels | normal | - | The shadow the figure casts on the grass, away from the light: a strip of darker green out from the back boot |
| meadow | cells | normal | locked | Light green grass, a brown path coming out of the hills toward the front, and tufts of darker grass in the near rows |
| hills | cells | normal | locked | Far hills in the haze, cyan against the light cyan sky, their tops in half-block steps |
| sky | cells | normal | locked | The ice ramp a rung a row: blue at the top, light blue, then light cyan down to the hills |

Snapshots: r1 is the scene; r2 the figure with its outline and its body in flat colours, no lit side; r3 the finished piece, with the lit side of the coat and the pack, the face, the highlights and the shadow.

## Variants

| Variant | Changes |
|---|---|
| No outline | hide outline |
| No highlights | hide highlights |
| Silhouette | raise outline above highlights |
| One layer | show figure (merged); hide highlights; hide face detail; hide body; hide outline; hide shadow |
| Scene only | hide highlights; hide face detail; hide body; hide outline; hide shadow |

## Try this

1. Show figure (merged), hide the five layers under it, select figure (merged) and Alt+drag it along the path. The scene is locked, so it stays put, and the traveller walks over it with the outline and the shadow coming along.
2. Without the merged copy: select shadow, outline, body, face detail and highlights in turn and Alt+drag each by the same dx and dy, reading them off the status bar. Miss one and the parts come apart, which is why the merged copy is there.
3. Hide outline. The coat and the hat lose their edge against the sky and the arm runs into the coat. Then show it and raise it above highlights: the black silhouette alone still reads as someone with a hat, a pack and a staff, which is the test a figure has to pass first.
4. Hide highlights, then face detail. The figure still reads without them; they are the last, smallest layers, kept apart so they can be redrawn without touching the body.
5. Select body and paint over the figure with any colour. The alpha lock keeps the paint inside the traveller's pixels, so a new coat colour needs no masking.
6. To use the traveller in another piece, hide sky, hills and meadow, select the figure and choose Selection > Save to Library…. With the scene hidden the stamp holds only the figure, ready to place from the Library.
