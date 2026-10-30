"""lighthouse: a 24x8 test scene in four layers, drawn with ansidraw's run().

    python3 scene.py          writes lighthouse.ans beside it
    python3 scene.py --list-layers / --layer-ans=NAME --key=K
                              ansidraw's layer modes (scene_import.h)

ansidraw is imported from $ANSIDRAW_DIR when it is set, else from Python's path.
"""
import os
import sys

if os.environ.get("ANSIDRAW_DIR"):
    sys.path.insert(0, os.environ["ANSIDRAW_DIR"])
import ansidraw as D  # noqa: E402

HERE = os.path.dirname(os.path.abspath(sys.argv[0]))
W, ROWS = 24, 8
H = ROWS * 2
HORIZON = 10


def sky(c):
    for y in range(HORIZON):
        c.rect(0, y, W - 1, y, D.BLUE if y < 4 else D.MAGENTA if y < 8 else D.LRED)
    c.rect(0, HORIZON, W - 1, H - 1, D.BLUE)


def sea(c):
    for y in range(HORIZON, H):
        for x in range(W):
            c.plot(x, y, D.LBLUE if (x + y) % 5 == 0 else D.BLUE)


def tower(c):
    for y in range(3, HORIZON + 2):
        c.rect(16, y, 18, y, D.WHITE if (y // 2) % 2 else D.RED)
    c.rect(15, 2, 19, 2, D.DGREY)
    c.plot(17, 1, D.YELLOW)
    # the tower's reflection darkens the water it stands in
    for y in range(HORIZON + 2, H):
        for x in range(16, 19):
            c.plot(x, y, D.BLACK if c.px[y][x] == D.BLUE else D.DGREY)


def beam(c):
    c.put_text(1, 1, "~fp~", D.YELLOW)


D.run([("sky", sky), ("sea", sea), ("tower", tower), ("beam", beam)],
      W, ROWS, os.path.join(HERE, "lighthouse.ans"), title="lighthouse")
