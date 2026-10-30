#!/usr/bin/env python3
"""Writes the three TheDraw test fonts (PLAN §15 fixtures) and a bundle of
all three. The letterforms are funkpower's own, drawn for these tests: one
master of A-Z, a-z, 0-9 and _ on a grid of 4 pixels (5 for M, W, m and w)
by 6 rows, rows 0-4 for capitals and digits, 1-4 for the x-height and row 5
for descenders. Each font is made from the master:

  bw-outline.tdf  outline (type 0): each master pixel 2x2 cells, traced with
                  TheDraw's outline codes; descender rows end in '&'.
  bw-block.tdf    block (type 1): each master pixel 2 columns, two master
                  rows a cell row as half blocks (█ ▀ ▄).
  bw-colour.tdf   colour (type 2): each master pixel 2 columns, one row a
                  cell row, shaded by row from █ down to ░ in cyan on blue.
  bw-all.tdf      the three, in that order, in one file.

A master column is 2 cells wide, except the counters of the 5-pixel M, W,
m and w, which are 1, so no glyph is wider than 8 columns.

Format: TheDraw font files as documented by Roy/SAC ("TheDraw Fonts File
(.TDF) Specifications", 2014). The output depends on nothing but this file:
run it with no arguments to rewrite the fonts beside it.
"""

import os
import sys

# '#' ink, '.' none; rows from the top, a row may be written shorter.
MASTER = {
    'A': [".##.", "#..#", "####", "#..#", "#..#"],
    'B': ["###.", "#..#", "###.", "#..#", "###."],
    'C': [".###", "#...", "#...", "#...", ".###"],
    'D': ["###.", "#..#", "#..#", "#..#", "###."],
    'E': ["####", "#...", "###.", "#...", "####"],
    'F': ["####", "#...", "###.", "#...", "#..."],
    'G': [".###", "#...", "#.##", "#..#", ".###"],
    'H': ["#..#", "#..#", "####", "#..#", "#..#"],
    'I': ["###", ".#.", ".#.", ".#.", "###"],
    'J': ["..##", "...#", "...#", "#..#", ".##."],
    'K': ["#..#", "#.#.", "##..", "#.#.", "#..#"],
    'L': ["#...", "#...", "#...", "#...", "####"],
    'M': ["#...#", "##.##", "#.#.#", "#...#", "#...#"],
    'N': ["#..#", "##.#", "#.##", "#..#", "#..#"],
    'O': [".##.", "#..#", "#..#", "#..#", ".##."],
    'P': ["###.", "#..#", "###.", "#...", "#..."],
    'Q': [".##.", "#..#", "#..#", "#.##", ".###"],
    'R': ["###.", "#..#", "###.", "#.#.", "#..#"],
    'S': [".###", "#...", ".##.", "...#", "###."],
    'T': ["###", ".#.", ".#.", ".#.", ".#."],
    'U': ["#..#", "#..#", "#..#", "#..#", ".##."],
    'V': ["#..#", "#..#", "#..#", ".##.", ".##."],
    'W': ["#...#", "#...#", "#.#.#", "##.##", "#...#"],
    'X': ["#..#", "#..#", ".##.", "#..#", "#..#"],
    'Y': ["#.#", "#.#", ".#.", ".#.", ".#."],
    'Z': ["####", "...#", ".##.", "#...", "####"],
    'a': ["", ".###", "#..#", "#..#", ".###"],
    'b': ["#...", "###.", "#..#", "#..#", "###."],
    'c': ["", ".##", "#..", "#..", ".##"],
    'd': ["...#", ".###", "#..#", "#..#", ".###"],
    'e': ["", ".##.", "####", "#...", ".###"],
    'f': [".##", "#..", "##.", "#..", "#.."],
    'g': ["", ".###", "#..#", ".###", "...#", "###."],
    'h': ["#...", "###.", "#..#", "#..#", "#..#"],
    'i': ["#", "", "#", "#", "#"],
    'j': [".#", "", ".#", ".#", ".#", "#."],
    'k': ["#..", "#..", "#.#", "##.", "#.#"],
    'l': ["##", ".#", ".#", ".#", ".#"],
    'm': ["", "####.", "#.#.#", "#.#.#", "#.#.#"],
    'n': ["", "##.", "#.#", "#.#", "#.#"],
    'o': ["", ".##.", "#..#", "#..#", ".##."],
    'p': ["", "###.", "#..#", "#..#", "###.", "#..."],
    'q': ["", ".###", "#..#", "#..#", ".###", "...#"],
    'r': ["", "#.#", "##.", "#..", "#.."],
    's': ["", ".##", "#..", "..#", "##."],
    't': [".#.", "###", ".#.", ".#.", "..#"],
    'u': ["", "#.#", "#.#", "#.#", "###"],
    'v': ["", "#.#", "#.#", "#.#", ".#."],
    'w': ["", "#...#", "#.#.#", "#.#.#", "#####"],
    'x': ["", "#.#", ".#.", ".#.", "#.#"],
    'y': ["", "#.#", "#.#", ".##", "..#", "##."],
    'z': ["", "###", "..#", ".#.", "###"],
    '0': [".##.", "#..#", "#.##", "##.#", ".##."],
    '1': [".#.", "##.", ".#.", ".#.", "###"],
    '2': ["###.", "...#", ".##.", "#...", "####"],
    '3': ["###.", "...#", ".##.", "...#", "###."],
    '4': ["#..#", "#..#", "####", "...#", "...#"],
    '5': ["####", "#...", "###.", "...#", "###."],
    '6': [".##.", "#...", "###.", "#..#", ".##."],
    '7': ["####", "...#", "..#.", ".#..", ".#.."],
    '8': [".##.", "#..#", ".##.", "#..#", ".##."],
    '9': [".##.", "#..#", ".###", "...#", ".##."],
    '_': ["", "", "", "", "", "####"],
}
ROWS = 6
DESCENDER = 5  # the master row below the baseline

# Cell widths of the master columns: 2 each, the counters of the 5-pixel
# letters 1.
NARROW_COUNTERS = {'M', 'W', 'm', 'w'}


def grid(ch):
    rows = MASTER[ch]
    width = max(len(r) for r in rows)
    return [[x < len(r) and r[x] == '#' for x in range(width)]
            for r in rows + [""] * (ROWS - len(rows))]


def col_widths(ch, width):
    if ch in NARROW_COUNTERS:
        assert width == 5, ch
        return [2, 1, 2, 1, 2]
    return [2] * width


def widen(ch, rows):
    """Each master column to its cell columns."""
    widths = col_widths(ch, len(rows[0]))
    return [[px for px, w in zip(r, widths) for _ in range(w)] for r in rows]


def has_descender(ch):
    return any(grid(ch)[DESCENDER])


# -- outline ---------------------------------------------------------------
# TheDraw's outline codes (Roy/SAC's table): A top edge, B bottom edge,
# C left edge, D right edge; E, F, K, L the outer corners (upper left,
# upper right, lower left, lower right); G, H, I, J the inner corners where
# the outside is to the lower right, lower left, upper right and upper left;
# O a hard space inside the letter; @ a filler outside it; & ends a
# descender row.

def outline_code(ink, x, y):
    def at(dx, dy):
        xx, yy = x + dx, y + dy
        return 0 <= yy < len(ink) and 0 <= xx < len(ink[yy]) and ink[yy][xx]
    up, down, left, right = at(0, -1), at(0, 1), at(-1, 0), at(1, 0)
    assert (left or right) and (up or down), "a stroke one cell thick"
    if not up:
        return 'E' if not left else 'F' if not right else 'A'
    if not down:
        return 'K' if not left else 'L' if not right else 'B'
    if not left:
        return 'C'
    if not right:
        return 'D'
    if not at(-1, -1):
        return 'J'
    if not at(1, -1):
        return 'I'
    if not at(-1, 1):
        return 'H'
    if not at(1, 1):
        return 'G'
    return 'O'


def outline_glyph(ch):
    cells = widen(ch, grid(ch))
    ink = [r for r in cells for _ in range(2)]  # each master row 2 cell rows
    last = 2 * (ROWS if has_descender(ch) else DESCENDER)
    rows = []
    for y in range(last):
        codes = [outline_code(ink, x, y) if ink[y][x] else '@' for x in range(len(ink[y]))]
        row = ''.join(codes).rstrip('@')
        if y >= 2 * DESCENDER:
            row += '&'
        rows.append(row.encode('ascii'))
    width = len(cells[0])
    return width, 2 * DESCENDER, b'\r'.join(rows)


# -- block -----------------------------------------------------------------

HALF = {(True, True): 0xDB, (True, False): 0xDF, (False, True): 0xDC, (False, False): 0x20}


def block_glyph(ch):
    cells = widen(ch, grid(ch))
    rows = []
    for y in range(0, ROWS, 2):
        row = bytes(HALF[(cells[y][x], cells[y + 1][x])] for x in range(len(cells[y])))
        rows.append(row.rstrip(b' '))
    return len(cells[0]), ROWS // 2, b'\r'.join(rows)


# -- colour ----------------------------------------------------------------
# Per master row: (CP437 character, attribute), the attribute background
# in the high nibble and foreground in the low one. The shade climbs the sea
# ladder one step at a time, cyan █ at the top through ▓ ▒ ░ cyan on blue to
# a space on blue at the baseline and below: ink, as a space on any
# background but black is. Every inked cell but the top row keeps a blue
# background, so a flat Fill of a blue-neighbour colour stays on its ladder.

SHADE = [
    (0xDB, 0x03),  # █ cyan
    (0xB2, 0x13),  # ▓ cyan on blue
    (0xB1, 0x13),  # ▒ cyan on blue
    (0xB0, 0x13),  # ░ cyan on blue
    (0x20, 0x10),  # space on blue
    (0x20, 0x10),  # space on blue
]


def colour_glyph(ch):
    cells = widen(ch, grid(ch))
    last = ROWS if has_descender(ch) else DESCENDER
    rows = []
    for y in range(last):
        n = len(cells[y])
        while n and not cells[y][n - 1]:
            n -= 1
        row = bytearray()
        for x in range(n):
            row += bytes(SHADE[y]) if cells[y][x] else bytes((0x20, 0x00))
        if y == DESCENDER:
            row += b'&'
        rows.append(bytes(row))
    return len(cells[0]), DESCENDER, b'\r'.join(rows)


# -- the file --------------------------------------------------------------

CHARS = [chr(c) for c in range(0x21, 0x7F)]


def font_record(name, kind, spacing, make):
    block = bytearray()
    offsets = []
    for c in CHARS:
        if c not in MASTER:
            offsets.append(0xFFFF)
            continue
        width, height, data = make(c)
        assert 1 <= width <= 8 and 1 <= height <= 12, c
        offsets.append(len(block))
        block += bytes((width, height)) + data + b'\0'
    assert len(block) < 0xFFFF
    raw = name.encode('ascii')
    assert len(raw) <= 12
    out = bytearray(b'\x55\xAA\x00\xFF')
    out += bytes((len(raw),)) + raw.ljust(12, b'\0') + bytes(4)
    out += bytes((kind, spacing)) + len(block).to_bytes(2, 'little')
    for off in offsets:
        out += off.to_bytes(2, 'little')
    return bytes(out + block)


HEADER = b'\x13TheDraw FONTS file\x1A'

FONTS = [
    ('bw-outline.tdf', font_record('BW Outline', 0, 1, outline_glyph)),
    ('bw-block.tdf', font_record('BW Block', 1, 1, block_glyph)),
    ('bw-colour.tdf', font_record('BW Colour', 2, 1, colour_glyph)),
]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, record in FONTS:
        with open(os.path.join(here, name), 'wb') as f:
            f.write(HEADER + record)
    with open(os.path.join(here, 'bw-all.tdf'), 'wb') as f:
        f.write(HEADER + b''.join(record for _, record in FONTS))
    return 0


if __name__ == '__main__':
    sys.exit(main())
