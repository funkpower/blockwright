#!/usr/bin/env python3
"""Draws the disk image's background.

    python3 packaging/macos/make-art.py

writes, beside this script:
    dmg-background.png         the disk image window, 660 x 400
    dmg-background@2x.png      the same at twice the size

The caption is set in the IBM VGA 8x16 glyphs the editor ships
(core/src/font_ibm_vga_8x16.inc). Needs Pillow. The app icon, icon-1024.png
(CMake makes blockwright.icns of it), is rendered from branding/icon.ans by
branding/render.
"""

import pathlib
import re

from PIL import Image, ImageDraw

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent

# the 16-colour palette's entries the art uses
LCYAN, GREY = "#55ffff", "#aaaaaa"
TOP, BOTTOM = (0x22, 0x26, 0x44), (0x0b, 0x0c, 0x18)



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
    background(1).save(HERE / "dmg-background.png", optimize=True)
    background(2).save(HERE / "dmg-background@2x.png", optimize=True)
