# Mountain ridges at dusk, far to near, each darker than the one behind it,
# as haze makes them: the far one light blue, the next dark grey, the nearer
# ones slate (dark grey shaded over blue or black), the nearest darkest. All
# are darker than the light red glow behind them, and none takes a colour the
# sky's rows have.
#
# Reads the generator's JSON on stdin and writes an .ans on stdout, one row
# of the layer a line. The far ridge is a line of value noise from `seed`;
# `roughness` adds a finer line on top of it, and `sharp` folds the noise
# into peaks. No peak rises above row `top`. No ridge climbs or drops more
# than a row a column, and each stands two or three rows below the one
# behind it, so it only ever meets the ridges either side of it. Cells
# above every ridge are black spaces, which the layer's key colour (black)
# makes transparent.
#
# Its own random numbers (a 32-bit LCG), not awk's rand(), so every awk
# draws the same ridges from the same seed.

function field(obj, key,    s) {
    if (!match(obj, "\"" key "\":[^,}]*"))
        return ""
    s = substr(obj, RSTART, RLENGTH)
    sub(/^"[^"]*":/, "", s)
    return s
}

function object(key) {
    if (!match(input, "\"" key "\":\\{[^}]*\\}"))
        return ""
    return substr(input, RSTART, RLENGTH)
}

function rnd() {
    state = (state * 1664525 + 1013904223) % 4294967296
    return state / 4294967296
}

function smooth(t) { return t * t * (3 - 2 * t) }

# Value noise: lattice values every `step` columns, eased between.
function noise(lat, step, x,    i, t) {
    i = int(x / step)
    t = smooth((x - i * step) / step)
    return lat[i] * (1 - t) + lat[i + 1] * t
}

# A ridge's cell from a spec: "c", solid colour c, or "dark light q", q
# quarters of `light` over `dark` (a light, medium or dark shade), as
# Blockwright's ramps draw a rung.
function paint(spec,    a, n) {
    n = split(spec, a, " ")
    if (n == 1)
        return a[1] < 8 ? sgr(0, a[1] + 0) " " : sgr(a[1] + 0, 0) "\333"
    return sgr(a[2] + 0, a[1] + 0) substr("\260\261\262", a[3], 1)
}

function sgr(fg, bg,    s) {
    s = "\033[0;" (fg >= 8 ? "1;" : "") "3" ansi[fg % 8]
    if (bg >= 8)
        s = s ";5"
    return s ";4" ansi[bg % 8] "m"
}

{ input = input $0 }

END {
    canvas = object("canvas")
    params = object("params")
    w = field(canvas, "width") + 0
    h = field(canvas, "height") + 0
    seed = field(params, "seed") + 0
    count = field(params, "ridges") + 0
    rough = field(params, "roughness") + 0
    top = field(params, "top") + 0
    sharp = field(params, "sharp") == "true"
    # no peak rises above `top`, and none of the far ridge goes under the
    # last row: on a canvas shorter than `top`, or than the rows under it the
    # ridge would take, it lies along the last rows, so every column shows it
    if (top > h - 1)
        top = h - 1

    # the PC palette's order (blue 1, red 4) to SGR's (red 1, blue 4)
    split("0 4 2 6 1 5 3 7", a, " ")
    for (i = 0; i < 8; ++i)
        ansi[i] = a[i + 1]
    # far to near, a ridge a "|": solid light blue, solid dark grey, then
    # slate, dark grey shaded over blue or black. Four ridges step down
    # through dark grey over blue (a dark shade), then over black (a medium
    # shade, then a light one). Fewer ridges leave out the middle ones, so
    # the far one is always the lightest and the nearest the darkest.
    looks[1] = "9"
    looks[2] = "9|1 8 1"
    looks[3] = "9|8|1 8 1"
    looks[4] = "9|1 8 3|0 8 2|0 8 1"
    split(looks[count], a, "|")
    for (r = 0; r < count; ++r)
        look[r] = paint(a[r + 1])

    # the far ridge is a line of value noise from `seed`, a row a column at
    # most; each nearer one stands two or three rows under the one behind
    # it, its own noise picking which, so no ridge is more than three rows
    # deep, as the ladder's steps are (only the nearest, at the end of the
    # ramp, runs to the bottom)
    state = seed
    for (r = 0; r < count; ++r) {
        for (i = 0; i <= w / 4 + 2; ++i) {
            coarse[i] = rnd()
            fine[i] = rnd()
        }
        for (x = 0; x < w; ++x) {
            v = noise(coarse, 14, x) + rough * (noise(fine, 4, x) - 0.5)
            if (r == 0) {
                if (sharp)
                    v = 1 - 2 * (v > 0.5 ? v - 0.5 : 0.5 - v)
                y = top + int((1 - v) * 7 + 0.5)
                if (y < top)
                    y = top
                if (y > h - 1)
                    y = h - 1
            } else {
                y = ridge[r - 1, x] + (v > 0.5 ? 3 : 2)
                # a row a column: the other of the two where it would be two
                if (x > 0 && (y > ridge[r, x - 1] + 1 || y < ridge[r, x - 1] - 1))
                    y = ridge[r - 1, x] + (y == ridge[r - 1, x] + 3 ? 2 : 3)
            }
            ridge[r, x] = y
        }
        # no notch or spike a column wide: it reads as a break in the line
        for (x = 1; x < w - 1; ++x) {
            y = ridge[r, x - 1]
            if (ridge[r, x + 1] == y && ridge[r, x] != y &&
                (r == 0 || (y >= ridge[r - 1, x] + 2 && y <= ridge[r - 1, x] + 3)))
                ridge[r, x] = y
        }
        if (r > 0)
            continue
        # the far ridge, no more than a row from one column to the next:
        # lower the higher side
        for (changed = 1; changed;) {
            changed = 0
            for (x = 0; x < w; ++x) {
                if (x > 0 && ridge[r, x] < ridge[r, x - 1] - 1) {
                    ridge[r, x] = ridge[r, x - 1] - 1
                    changed = 1
                }
                if (x < w - 1 && ridge[r, x] < ridge[r, x + 1] - 1) {
                    ridge[r, x] = ridge[r, x + 1] - 1
                    changed = 1
                }
            }
        }
    }

    for (row = 0; row < h; ++row) {
        line = ""
        for (x = 0; x < w; ++x) {
            c = -1
            for (r = count - 1; r >= 0; --r)
                if (row >= ridge[r, x]) {
                    c = r
                    break
                }
            line = line (c < 0 ? "\033[0m " : look[c])
        }
        printf "%s\033[0m\r\n", line
    }
}
