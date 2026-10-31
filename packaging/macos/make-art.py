#!/usr/bin/env python3
"""Draws the macOS app icon and the disk image's background.

    python3 packaging/macos/make-art.py

writes, beside this script:
    icon-1024.png              the app icon (CMake makes blockwright.icns of it)
    dmg-background.png         the disk image window, 660 x 400
    dmg-background@2x.png      the same at twice the size

The icon is a "B" of square block pixels in the 16-colour palette with a
shade-block shadow; the background's caption is set in the IBM VGA 8x16
glyphs the editor ships (core/src/font_ibm_vga_8x16.inc). Needs Pillow.
"""

import math
import pathlib
import re

from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent

# the 16-colour palette's entries the art uses
WHITE, YELLOW, LCYAN, CYAN = "#ffffff", "#ffff55", "#55ffff", "#00aaaa"
LBLUE, BLUE, GREY, DGREY = "#5555ff", "#0000aa", "#aaaaaa", "#555555"
TOP, BOTTOM = (0x22, 0x26, 0x44), (0x0b, 0x0c, 0x18)

LETTER = [
    "######.",
    "##...##",
    "##...##",
    "######.",
    "##...##",
    "##...##",
    "##...##",
    "######.",
]
ROW_COLOURS = [WHITE, LCYAN, LCYAN, LCYAN, CYAN, CYAN, LBLUE, LBLUE]


def gradient(size, top=TOP, bottom=BOTTOM):
    w, h = size
    img = Image.new("RGB", size)
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    return img


def shade(d, x, y, size, colour, sub):
    """A medium-shade block (a checkerboard of `sub`-pixel squares)."""
    n = size // sub
    for j in range(n):
        for i in range(n):
            if (i + j) % 2 == 0:
                d.rectangle([x + i * sub, y + j * sub, x + (i + 1) * sub - 1, y + (j + 1) * sub - 1], fill=colour)


def squircle_mask(size, inset, scale=4):
    """Apple's icon tile: a superellipse inset from the canvas edge."""
    big = size * scale
    m = Image.new("L", (big, big), 0)
    c = big / 2
    r = (size / 2 - inset) * scale
    n = 5.0
    pts = []
    for k in range(720):
        a = 2 * math.pi * k / 720
        ca, sa = math.cos(a), math.sin(a)
        pts.append((c + r * abs(ca) ** (2 / n) * (1 if ca >= 0 else -1), c + r * abs(sa) ** (2 / n) * (1 if sa >= 0 else -1)))
    ImageDraw.Draw(m).polygon(pts, fill=255)
    return m.resize((size, size), Image.LANCZOS)


def icon():
    size, inset, px = 1024, 100, 64
    tile = gradient((size, size))
    d = ImageDraw.Draw(tile)
    rows, cols = len(LETTER), len(LETTER[0])
    x0 = (size - (cols + 1) * px) // 2
    y0 = (size - (rows + 1) * px) // 2
    lit = {(c, r) for r, line in enumerate(LETTER) for c, ch in enumerate(line) if ch == "#"}
    # the shadow falls outside the letter, not into its counters
    outside, todo = set(), [(-1, -1)]
    while todo:
        c, r = todo.pop()
        if (c, r) in outside or (c, r) in lit or not (-1 <= c <= cols + 1 and -1 <= r <= rows + 1):
            continue
        outside.add((c, r))
        todo += [(c + 1, r), (c - 1, r), (c, r + 1), (c, r - 1)]
    for c, r in sorted(lit):
        if (c + 1, r + 1) in outside:
            shade(d, x0 + (c + 1) * px, y0 + (r + 1) * px, px, BLUE, 8)
    for c, r in sorted(lit):
        d.rectangle([x0 + c * px, y0 + r * px, x0 + (c + 1) * px - 1, y0 + (r + 1) * px - 1], fill=ROW_COLOURS[r])
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(tile, (0, 0), squircle_mask(size, inset))
    return out


def vga_glyphs():
    text = (ROOT / "core/src/font_ibm_vga_8x16.inc").read_text()
    data = [int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]{2})", re.sub(r"//[^\n]*", "", text))]
    assert len(data) == 256 * 16
    return data


def caption(d, glyphs, x, y, parts, scale):
    """Sets (text, colour) parts in the VGA font, each glyph pixel `scale` wide."""
    for text, colour in parts:
        for ch in text:
            code = ord(ch)
            for row in range(16):
                bits = glyphs[code * 16 + row]
                for col in range(8):
                    if bits & (0x80 >> col):
                        d.rectangle([x + col * scale, y + row * scale, x + (col + 1) * scale - 1, y + (row + 1) * scale - 1], fill=colour)
            x += 8 * scale
    return x


def background(scale):
    # Finder labels the icons in black (light mode) or white (dark mode) on
    # whatever is behind them, so the icons sit on a slate that reads with
    # either, and only the caption band is dark
    w, h, band = 660 * scale, 400 * scale, 112 * scale
    img = gradient((w, h), (0x74, 0x79, 0x98), (0x62, 0x67, 0x86))
    img.paste(gradient((w, band)), (0, 0))
    d = ImageDraw.Draw(img)
    for x in range(0, w, 2 * scale):
        if (x // (2 * scale)) % 2 == 0:
            d.rectangle([x, band, x + 2 * scale - 1, band + 2 * scale - 1], fill=BOTTOM)
    glyphs = vga_glyphs()
    parts = [("Drag ", GREY), ("Blockwright", LCYAN), (" to Applications", GREY)]
    width = sum(len(t) for t, _ in parts) * 8 * 2 * scale
    caption(d, glyphs, (w - width) // 2, 40 * scale, parts, 2 * scale)
    # an arrow of block pixels from the app (x 170) to Applications (x 490),
    # at the icons' centre line (y 230): a shaded shaft, a solid head
    p = 8 * scale
    cy = 230 * scale
    for i in range(14):
        shade(d, (254 + i * 8) * scale, cy - p, p, LCYAN, 2 * scale)
        shade(d, (254 + i * 8) * scale, cy, p, LCYAN, 2 * scale)
    head = 366 * scale
    for k in range(4):
        d.rectangle([head + k * p, cy - (4 - k) * p, head + (k + 1) * p - 1, cy + (4 - k) * p - 1], fill=LCYAN)
    return img


if __name__ == "__main__":
    icon().save(HERE / "icon-1024.png", optimize=True)
    background(1).save(HERE / "dmg-background.png", optimize=True)
    background(2).save(HERE / "dmg-background@2x.png", optimize=True)
