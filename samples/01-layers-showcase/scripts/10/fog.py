"""Sea fog for the Layers showcase: wisps of shade over the water.

Reads the generator's JSON on stdin and writes an .ans on stdout, one row of
the layer a line. Cells off a wisp are black spaces, which the layer's key
colour (black) makes transparent.
"""
import json
import random
import sys

args = json.load(sys.stdin)
layer = args["layer"]
params = args["params"]
w, h = layer["width"], layer["height"]
rnd = random.Random(params.get("seed", 7))
wisps = params.get("wisps", 5)
top, bottom = params.get("top", 24), params.get("bottom", 27)

grid = [[None] * w for _ in range(h)]
for _ in range(wisps):
    y = rnd.randint(top, bottom)
    length = rnd.randint(8, 18)
    x0 = rnd.randint(-4, w - 4)
    for x in range(x0, x0 + length):
        if 0 <= x < w and 0 <= y < h:
            # a wisp is thicker in its middle
            edge = min(x - x0, x0 + length - 1 - x)
            grid[y][x] = "\xb1" if edge >= 3 else "\xb0"

out = []
for row in grid:
    line = ""
    for cell in row:
        # grey on blue: the mist over the near water
        line += "\x1b[0;37;44m" + cell if cell else "\x1b[0m "
    out.append(line + "\x1b[0m")
sys.stdout.buffer.write(("\r\n".join(out) + "\r\n").encode("latin-1"))
