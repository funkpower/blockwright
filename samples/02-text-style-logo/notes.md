# Ember Kiln

A two-word logo set in big block text, built from eight layers: two saved style stacks, one of them on both words, a red plate under a small tagline, a baked copy of a word, and a second stack on hidden copies of the words to swap the whole logo from fire to ice.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| tagline | text | normal | - | ART AT WHITE HEAT in the mini face, yellow on a red plate one cell wider all round, from the Kiln tag stack. |
| KILN | text | normal | - | The second word in big block text, in the same Molten stack as EMBER: one style on two layers. Change the stack on one and Save as Preset, then pick it for the other. |
| EMBER | text | normal | - | The first word in big block text, Molten stack: a flat yellow fill, a fire Shade lit from below so the letters run from yellow at the foot through light red to red at the top, and a dark grey extrude two cells to the south-east. |
| KILN (quenched) | text | normal | - | KILN again in the Quenched stack, the same effects on the ice ramp, hidden. The Quenched variant shows it. |
| EMBER (quenched) | text | normal | - | EMBER in the Quenched stack, hidden. |
| EMBER (baked) | cells | normal | - | EMBER baked to cells (Layer > Bake Layer), hidden: the same look, now plain cells to edit one by one, and no longer a word the Text tool can retype. |
| sparks | cells | normal | - | Embers drifting up off the lettering, a glyph each in the fire colours, in the two corners the words leave empty. |
| backdrop | cells | normal | locked | Black blocks under everything, locked, so the empty rows over and under the lettering keep their place when the logo is pasted to IRC. |

## Variants

| Variant | Changes |
|---|---|
| Quenched | hide EMBER; hide KILN; show EMBER (quenched); show KILN (quenched); hide sparks |
| Baked | hide EMBER; show EMBER (baked) |
| No tagline | hide tagline |
| No sparks | hide sparks |

## Try this

1. Click EMBER with the Text tool and type another word. The style follows the letters: the shade, the fill and the extrude are redrawn for the new word.
2. Open Layer > Layer Style… on EMBER. The effects run top to bottom as a stack: Shadow, Fill, Shade. Untick Shade to see the flat fill under it, then Cancel.
3. Now open Layer > Layer Style… on KILN and turn Shade's light from S to N. The dialog's lint list gains one finding, at 26,4 counted from the layer's corner: a speck, lighter than every neighbour. It is the upturned tip of the L's foot. Lit from above, that tip stays solid light red while the cells under it drop to darker rungs. Press OK and the Lint dock lists the same speck at its canvas cell, 63,14; Undo puts the light back. The same change on EMBER leaves the lint clean.
4. Add a Glow to Molten and look at the lint again: on letters this big the glow's ramp skips rungs at every corner. That is why the logo has none.
5. Change Molten on EMBER, press Save as Preset…, and give it a name. Then pick that preset in KILN's Layer Style dialog: one stack, two words.
6. The project carries its three stacks in styles/: Molten, Quenched and Kiln tag. Copy the .style.json files into your user library folder (~/.local/share/Blockwright/library on Linux) and press Refresh in the Library dock to use them on your own pieces. Both word stacks need iCE colours, for the fire and ice ramps' bright rungs.
7. Show the Quenched variant by hand: hide EMBER, KILN and sparks, show the two quenched copies. Same letters, same layout, a different stack.
8. Retype EMBER and compare it with EMBER (baked): the baked copy keeps the old word, because baking turns a text layer into plain cells.
9. The project keeps two snapshots: r1, the two words alone, and r2, the finished logo with its tagline. File > Take Snapshot adds r3.
