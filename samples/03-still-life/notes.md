# Still life

A cylinder, a box and a ball on a table, each object on its own layer, shaded inside its own shape under an alpha lock with the light from the upper left. The shadows they cast and a rim light from the right sit on two layers of their own, so turning layers off shows how the light was built. The brushes tutorials' showcase.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| rim light | cells | normal | - | A back light from the east: a band along each object's right edge, brightest at the edge and a rung dimmer each cell in, on each object's own ramp. It only holds the cells it lights, so hiding it puts the dark sides back. |
| sphere | cells | normal | alpha | The ball, shaded as a dome on the sea ramp, lit from the north-west with a specular highlight. Smaller than the canvas, placed by its offset. |
| box | cells | normal | alpha | The box in three faces on the wood ramp without its black: a yellow top where the light falls, a brown front and a red side turned away, each edge stepping a rung at a time. |
| cylinder | cells | normal | alpha | The pillar on its round foot, shaded as a cylinder on the violet ramp, white down the lit side and dark down the far one. |
| shadows | cells | normal | - | The shadows the objects cast to the right on the table: the table's own grey rungs taken down, deepest at each foot and lifting back to the table a rung at a time. |
| table | cells | normal | locked | The table top, grey shading lighter toward the front, locked. |
| wall | cells | normal | locked | A flat dark grey wall, locked: dark enough to set the lit sides off, light enough that the black on the ball's far side still reads with the rim light off. |

## Variants

| Variant | Changes |
|---|---|
| No rim light | hide rim light |
| No shadows | hide shadows |
| Form only | hide rim light; hide shadows |

## Try this

1. Hide rim light, then show it again. The ball and the pillar lose their bright right edges and the box's side goes back to plain red: the rim was painted over the shading, not into it.
2. Hide shadows. The objects float over the table; the shadow layer is what sets them down on it.
3. Select sphere and paint across it with any brush. The alpha lock keeps the paint inside the ball: the wall and table around it stay as they were, and so do the cells of the layer's corners the ball doesn't fill.
4. Select the cylinder's cells and shade them again with the Form brush on the violet ramp, Form set to cylinder and the light from the north-west. Then try the light from the north-east and watch the white side move.
5. Move sphere with Alt+drag. Its shadow and its rim stay where they were, because each is a layer of its own; move them with it, or hide them, to see why a still life keeps its light apart.
6. The project keeps three snapshots: r1, the objects in flat colours; r2, each shaded inside its alpha lock; r3, the finished piece with its shadows and rim light.
