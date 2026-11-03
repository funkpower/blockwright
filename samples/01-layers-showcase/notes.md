# Harbour at night

A city on the water under a full moon, built from thirteen layers: every layer kind, both blend modes, the locks, a generator, a text layer and its baked copy, and a reference sketch.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| reference sketch | reference | normal | - | The rough the piece was drawn over, hidden; show it and set Layer > Reference Opacity to trace it. |
| title | text | normal | - | HARBOUR in the block face, a flat yellow fill with a brown drop shadow to the south-west. Click it with the Text tool to change the words; the style follows. |
| title (baked) | cells | normal | - | The same title baked to cells (Layer > Bake Layer), hidden: what the text layer becomes when it has to be edited cell by cell. |
| fog | generator | normal | - | Wisps of mist over the near water, made by fog.py. Its parameters are in the Layers dock: change the seed or the rows and press Run. Black is its key colour, so the cells off a wisp are clear. |
| reflection | cells | colours-only | - | The moon's path on the water. Colours only: it paints yellow onto the water's own ripple glyphs and keeps them. |
| water | cells | normal | - | The harbour, a band a rung down the sea ramp, with ripple dashes a rung lighter. |
| quay | cells | normal | - | The stone edge of the shore. Wider than the canvas at both ends, so move it and nothing runs out. |
| skyline | cells | normal | alpha | The towers, on a strip smaller than the canvas. Alpha-locked: shade over it and only the towers take the paint, which is how their roofs caught the glow. |
| cloud | pixels | normal | - | A thin grey cloud across the moon, half a cell high. iCE is off, so a cell shows only one bright colour, and the upper layer keeps its own: the grey stays and the moon's yellow under it shows as brown. The Moon in front variant turns that round. |
| moon | pixels | normal | - | A pixel layer, two pixels to a cell, drawn with the Half-Block Brush. Smaller than the canvas and placed by its offset: Alt+drag it along the sky. |
| stars | cells | chars-only | - | Star glyphs only. Each one takes its colour from the sky cell under it. |
| day sky | cells | normal | - | A daylight sky down the sea ramp, hidden; the Day variant shows it. |
| sky | cells | normal | locked | The night sky, a band a rung up the sunset ramp to the city's glow. Locked so a stray stroke can't touch it. |

## Variants

| Variant | Changes |
|---|---|
| Day | show day sky; hide stars; hide moon; hide reflection |
| Moon set | lower moon below sky |
| Moon in front | raise moon above cloud |
| No fog | hide fog |
| Baked title | hide title; show title (baked) |

## Try this

1. Turn the layers' eyes off one at a time from the top, and watch what each one adds.
2. Make the Day variant by hand: show day sky, then hide stars, moon and reflection. The towers' lights still read in daylight because their colours are their own.
3. Drag the moon below the sky in the Layers dock. The sky is opaque, so the moon goes; drag it back. Then drag it above the cloud: now the moon's yellow wins its cells and the cloud's grey goes dark where they meet.
4. Unlock the sky and paint on it, then undo. Try painting over the skyline: with its alpha lock on, the paint stays inside the towers.
5. Select the fog, change its seed in the Layers dock and press Run: the generator runs again and the mist moves.
6. Click the title with the Text tool and type a new name. The baked copy below it doesn't change: baking makes plain cells.
7. Show the reference sketch and pick a Layer > Reference Opacity to see the plan under the art.
8. The project keeps three snapshots, r1 to r3: the scene, the scene with fog and title, and the finished project, each the piece flattened to an .ans when it was taken. File > Take Snapshot adds r4.
