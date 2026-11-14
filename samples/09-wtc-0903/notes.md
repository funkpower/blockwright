# wtc-0903

Lower Manhattan at 9:03 on the morning of 11 September 2001, seen from across the East River: United Airlines Flight 175 going into the South Tower, the fireball breaking out of the tower's east face, and the North Tower burning behind it, its smoke carried left across the sky. A hundred columns wide, twenty more than a terminal opens at, and every row still fits one IRC line. Twelve layers: a locked sky, ten layers of towers, smoke, fire, light, city and water on it, and the tag.

## Layers

| Layer | Kind | Mode | Locks | What it does |
|---|---|---|---|---|
| tag | cells | normal | - | funkpower in grey in the lower right corner, row 47. |
| reflections | cells | colours-only | - | The shore buildings in the water along row 43, all in dark grey, with two columns of every five left out where the ripples break them. It holds colours and no shapes; the glyphs are the water's, so with it hidden row 43 is plain light blue. |
| water | cells | normal | - | The river from row 43 down: light blue along the shore, then blue, with short ripples in light cyan near the shore and in light blue further out. |
| skyline | cells | normal | - | Downtown in front of the towers' feet, from row 25 down: grey, white, dark grey and black buildings with bands of windows and plant rooms on some roofs, 40 Wall Street's cyan pyramid under a dark grey spire at column 79, rows 25 to 27, alone above the other roofs, and the shore along row 42, a black drive with dark grey piers. |
| plane | cells | normal | - | United 175, banked left wing down: the near wing and the top of the fuselage in grey, the rest of the fuselage, the far wing and the tail fin in dark grey. Its nose is already inside the South Tower's south face, where a black hole is cut at column 44, rows 18 and 19. |
| fireball | cells | normal | - | The fireball out of the South Tower's east face and across in front of the North Tower: a white and yellow core, brown and red around it, and a dark grey rim on the right. |
| sunlight | cells | normal | - | The morning sun on both east faces: white pinstripes on grey where the shade has grey on dark grey, and the crowns white over grey. It lights only the cells the plume and the fire left showing, so the shapes of both are cut out of it. |
| South Tower | cells | normal | alpha | The South Tower in shade, columns 42 to 55 from row 9 down: the south face, 42 to 45, and the east face, 46 to 55, both grey pinstripes on dark grey, under a grey roof line. It is nearer than the North Tower and the smoke, so it sits above them. Alpha-locked, so paint stays on the tower's own cells. |
| fire | cells | normal | - | The North Tower's fire, rows 13 to 15 across columns 60 to 73: a black gash along row 13, red and brown flames under it on the east face with yellow along their foot, and red and black on the south face. |
| plume | cells | normal | - | The smoke from the North Tower's fire, carried left behind the South Tower and out at the left edge: lobes of dark grey with grey crowns on the side toward the sun, black underneath and black where it leaves the tower, thinning to dark grey shade over the sky at the far end. The fire covers it at one cell, column 62 on row 13. |
| North Tower | cells | normal | alpha | The North Tower in shade, columns 60 to 73 from row 10 down, faces like the South Tower's, and the mast on its roof in grey up to row 2. The east-face cells the plume and the fire cover are drawn in the sun already. Alpha-locked, so paint stays on the tower's own cells. |
| sky | cells | normal | locked | The morning sky: blue at the top, light blue from row 13, light cyan from row 27, with three rows of shade at each change, rows 10 to 12 and 24 to 26. Locked, so painting the towers never touches it. |

Snapshots: r1 holds the sky, both towers in shade, the plume and the fire; r2 adds the sunlight, the fireball and the plane; r3 is the finished piece with the skyline, the water, the reflections and the tag.

## Variants

| Variant | Changes |
|---|---|
| Before 8:46 | hide plume; hide fire; hide fireball; hide plane |
| Sunlight off | hide sunlight |
| Fire only | hide North Tower; hide plume; hide South Tower; hide sunlight; hide plane; hide skyline; hide water; hide reflections; hide tag |
| Smoke in front | raise plume above sunlight |

## Try this

1. Hide sunlight, as the Sunlight off variant does. Both east faces go back to grey pinstripes on dark grey, the same as the south faces, and the crowns to a grey roof line over dark grey. 286 cells change, all of them on the two towers; the sky, the smoke and the city stay as they were.
2. Show sunlight again and hide plume, fire, fireball and plane, as the Before 8:46 variant does. The North Tower's east face is lit all the way down, rows 11 to 15 included, although the sunlight layer has nothing on those cells: it sits above the plume and the fire, so it lit only what they left showing. The North Tower layer carries those 41 cells in the sun itself.
3. Now hide sunlight as well. The rest of the faces go into shade and those 41 cells stay white on grey, a lit band across rows 11 to 15 of the North Tower. Light painted on a layer goes wherever that layer goes, including places the sun has left. Show sunlight, plume, fire, fireball and plane again before the next step.
4. Raise plume above sunlight, as the Smoke in front variant does. 79 cells change, in columns 42 to 62 and rows 9 to 14: the smoke now crosses the top of the South Tower instead of passing behind it, and covers the one cell where the fire sat over it, column 62 on row 13. Which tower is nearer is the order in the Layers dock and nothing else. Lower plume back to just above North Tower before the next step.
5. Hide every layer but sky, fire and fireball, as the Fire only variant does. The fire is a strip three rows deep, columns 60 to 73, rows 13 to 15, and the fireball a larger shape below it, rows 15 to 24, that reaches further left, to column 46 on the South Tower's east face. Each is on its own layer and can be recoloured or moved without repainting a tower. Show every layer again before the next step.
6. Select South Tower and paint across it. The alpha lock keeps the paint on the tower's own cells, columns 42 to 55, so the sky and the smoke beside it stay clean. On the east face the paint goes under the sunlight and shows only with sunlight hidden. Undo the paint before the next step.
7. Select reflections and paint along row 43 in light cyan. The layer is colours only, so the water's full blocks stay and take the new colour. Undo the paint before the next step.
8. The piece is a hundred columns wide and IRC counts none of them; it counts bytes. The dearest rows are 24 to 26, where the sky shades from light blue into light cyan: row 24 exports at 344 bytes, 347 as sent, against the project's budget of 407, and 204 of those bytes are its 68 shade glyphs, three bytes each in UTF-8 where a space takes one. The fire's rows, 13 to 15, come to 206, 241 and 280 bytes as sent.
