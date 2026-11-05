# Ridges at dusk

Mountain ridges and stars made by two generator layers, programs that ship inside the project with their parameters, under a lake, a shore, pines and a cabin on layers of their own.

Open it, select the ridges layer, and its parameter form appears in the Layers dock under the layer buttons: change a value and the generator runs again. Blockwright asks before a project's generators first run in a session; nothing runs until you say yes. Both generators are short shell scripts, kept with the project under `scripts/<id>/`. They need a POSIX `sh` and `awk` on the `PATH`, which a stock Windows install doesn't have; with them, the same parameters draw the same cells.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| cabin | pixels | normal | - | A cabin on the shore in half-block pixels, its window lit |
| pines | pixels | normal | - | Six pines in half-block pixels, standing over the lake and the ridges, green where the light from the right falls and dark in the shade of each bough |
| shore | cells | normal | locked | A strip of grass along the bottom |
| lake | cells | normal | locked | The water under the ridges, on the sea ramp, with ripples a rung up on every other row |
| ridges (baked) | cells | normal | - | Hidden: the ridges layer baked, its cells as they are, with no generator behind them |
| ridges (seed 12) | generator | normal | - | Hidden: the same ridges generator with other parameters (seed 12, four ridges, sharp peaks) |
| ridges | generator | normal | - | `ridges.sh`: three ridges from value noise, far to near, each darker than the one behind it as haze makes them (light blue, then dark grey, then slate) and all darker than the glow; its form has Seed, Ridges, Roughness, Sharp peaks and Highest row |
| stars | generator | chars-only | - | `stars.sh`: stars in the black rows from a seed; chars only, so each takes the sky's colours under it |
| sky | cells | normal | locked | Black at the top, then a rung a row up the sunset ramp to a light red glow behind the ridges |

Snapshots: r1 is the first run of the ridges, two smooth ones; r2 the parameters the piece keeps (three ridges, roughness 0.3); r3 the finished piece with the lake, shore, pines and cabin.

## Variants

| Variant | Changes |
|---|---|
| Another seed | show ridges (seed 12); hide ridges |
| Baked | show ridges (baked); hide ridges |
| No stars | hide stars |
| Generated only | hide lake; hide shore; hide pines; hide cabin |

## Try this

- Select the ridges layer and drag Seed: every value is another range of mountains, and the same value always gives the same one.
- Set Ridges to 1 or 4, Roughness to 0 for rolling hills, or turn on Sharp peaks. Snapshots r1 and r2 show two settings side by side.
- Set Highest row under 18: the peaks climb into the rows where the sky is still a gradient. They stay darker than the glow, but near the top of the sky they stand lighter than the dusk around them.
- Hide ridges and show ridges (baked): the picture is the same, but the baked layer is plain cells you can paint on, and it no longer has a form.
- Turn the stars layer's mode to normal: each star brings its own white on black instead of the sky's colours.
- From a terminal, `blockwright-cli run-generators` runs every generator layer of a saved copy again and keeps the cells they make.
