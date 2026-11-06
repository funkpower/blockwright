# Blockwright logo

The program's own logo taken apart into the layers a block-letter logo is made of: the mark's four blocks, the BLOCK and WRIGHT lettering in half-block pixels, a blue drop under the letters, and the shade cells that take each stroke from light cyan down to cyan, all over a locked black backdrop. Shown together they make the logo exactly as it ships with Blockwright.

The letters are drawn flat first, in two colours, and alpha-locked, so they can be recoloured without masking. The drop is the same letters moved one column right and one pixel down, so it only shows along their right and lower edges. The shade glyphs need whole cells, so they sit on a cells layer of their own over the pixel letters, where they can be redrawn or hidden without touching the letter shapes.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| shading | cells | normal | - | ▓ ▒ ░ in light cyan on cyan, a rung a row down each stroke, with a stem's left column a rung lighter where the light from the upper left falls |
| letters | pixels | normal | alpha | BLOCK over WRIGHT in half-block pixels, two pixels to a stroke with rounded corners: light cyan, and cyan at the foot. Alpha-locked, so paint stays inside the letters |
| drop | pixels | normal | - | The letters moved one column right and one pixel down, in blue, under them: a rim along their right and lower edges that lifts them off the black. It stays out of the counters of the B, O and R |
| gold block | cells | normal | - | The mark's top block, yellow going through ▓ ▒ ░ to brown, with a white top edge and a brown side and foot |
| white blocks | cells | normal | - | The mark's three white blocks in an L, white on grey going from ▓ to ░, with a dark grey side and foot |
| backdrop | cells | normal | locked | Black |

Snapshots: r1 is the mark and the letters in flat light cyan; r2 the letters going dark at the foot and the drop under them; r3 the finished logo, with the shade cells.

## Variants

| Variant | Changes |
|---|---|
| No shading | hide shading |
| No drop | hide drop |
| Wordmark only | hide gold block; hide white blocks |
| Mark only | hide shading; hide letters; hide drop |

## Try this

1. Hide shading. The letters still go from light to dark, in two flat colours; the shade cells are what turn that step into a ramp. Show it again and compare.
2. Hide drop. Without the blue rim the letters sit flat on the black. A drop one pixel deep is enough to lift them off it.
3. Select letters and paint over them with another colour. The alpha lock keeps it inside the strokes, and the shading and the drop stay as they were.
4. Select drop, turn on its alpha lock and paint over it in dark grey. Only the drop's own pixels take the colour, and the letters over it don't change.
5. Select gold block and Alt+drag it a cell or two. Its layer is only the size of the block, so it moves alone and the white blocks stay put.
6. Hide gold block and white blocks for the wordmark on its own, or the letters, the drop and the shading for the mark on its own, the way the logo appears at small sizes.
