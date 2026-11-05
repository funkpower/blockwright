# Stars over the dark top of the sky: `count` of them in rows 0 to
# `bottom`, from `seed`, never two side by side. The layer is chars only,
# so each star takes the colours of the sky cell under it.
#
# Reads the generator's JSON on stdin and writes an .ans on stdout, one row
# of the layer a line. Every other cell is a black space, which the
# layer's key colour (black) makes transparent. Its own random numbers (a
# 32-bit LCG), not awk's rand(), so every awk places the same stars.

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

function below(n) {
    state = (state * 1664525 + 1013904223) % 4294967296
    return int(state / 4294967296 * n)
}

{ input = input $0 }

END {
    canvas = object("canvas")
    params = object("params")
    w = field(canvas, "width") + 0
    h = field(canvas, "height") + 0
    state = field(params, "seed") + 0
    count = field(params, "count") + 0
    bottom = field(params, "bottom") + 0
    if (bottom >= h)
        bottom = h - 1
    # '*', '+' and the CP437 middle dot
    split("* + \372 \372", glyph, " ")

    for (placed = 0; placed < count && tries < 1000 * (count + 1); ++tries) {
        x = below(w - 1)
        y = below(bottom + 1)
        if ((x, y) in star || (x - 1, y) in star || (x + 1, y) in star)
            continue
        star[x, y] = glyph[below(4) + 1]
        ++placed
    }
    for (y = 0; y < h; ++y) {
        line = ""
        for (x = 0; x < w; ++x)
            line = line ((x, y) in star ? "\033[0;1;37m" star[x, y] : "\033[0m ")
        printf "%s\033[0m\r\n", line
    }
}
