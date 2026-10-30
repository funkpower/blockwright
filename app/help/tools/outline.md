Draws a one-cell outline round a shape: each edge cell under the brush aims for the ramp colour one step darker than the darkest of the shape just inside it. Where that would break the shading rules the lint checks, the cell takes a lighter shade instead, or is left as it is.

The shape is the cells in the ramp's colours, other than black. An edge cell reads the inside next to it, or, on a part too thin to have any, the inside of the body it grows from; a shape that is all edge is left alone. Where the step below the inside is black, the line is the darkest colour above black. An edge cell already darker than the line keeps its colour, except in a fade into black, where the darker cells take the line. Either button. One stroke draws a line one cell wide, however often it passes.

- **Outline the selection**: the selection is the shape, so the seam between it and a shape beside it takes the line.
- **Ramp**: which colours make the shape.
- **Size**, **Tip**, **Pressure**, **Alpha lock**, **Symmetry**: as for every brush.
