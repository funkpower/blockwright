#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Regenerates the bundled glyph tables from the pack's Windows bitmap fonts:
#   fon2inc.py <Bm437_IBM_VGA_8x16.FON> core/src/font_ibm_vga_8x16.inc
#   fon2inc.py <Bm437_IBM_BIOS.FON> core/src/font_ibm_bios_8x8.inc
# Reads the single FNT resource of the NE executable and writes its 256
# glyphs, one byte per row with bit 7 leftmost (16 rows for the VGA font,
# 8 for the BIOS one).
import os
import struct
import sys

NAMES = {
    'Bm437_IBM_VGA_8x16.FON': 'IBM VGA 8x16',
    'Bm437_IBM_BIOS.FON': 'IBM PC BIOS 8x8',
}

data = open(sys.argv[1], 'rb').read()
fon = os.path.basename(sys.argv[1])
assert fon in NAMES, 'not one of ' + ', '.join(NAMES)
ne = struct.unpack_from('<I', data, 0x3c)[0]
assert data[ne:ne + 2] == b'NE', 'not an NE executable'
rt = ne + struct.unpack_from('<H', data, ne + 0x24)[0]
shift = struct.unpack_from('<H', data, rt)[0]
p, fonts = rt + 2, []
while (tid := struct.unpack_from('<H', data, p)[0]) != 0:
    count = struct.unpack_from('<H', data, p + 2)[0]
    p += 8
    for _ in range(count):
        if tid == 0x8008:  # RT_FONT
            fonts.append(struct.unpack_from('<H', data, p)[0] << shift)
        p += 12
assert len(fonts) == 1, 'expected one font resource'
f = data[fonts[0]:]
version, height = struct.unpack_from('<H', f, 0)[0], struct.unpack_from('<H', f, 0x58)[0]
first, last = f[0x5f], f[0x60]
assert version == 0x200 and height in (8, 16) and (first, last) == (0, 255), (version, height, first, last)
glyphs = bytearray(256 * height)
for c in range(256):
    width, off = struct.unpack_from('<HH', f, 0x76 + c * 4)
    assert width == 8
    glyphs[c * height:(c + 1) * height] = f[off:off + height]

lines = [
    '// %s, CP437 order: 256 glyphs x %d rows, one byte per row, bit 7' % (NAMES[fon], height),
    '// leftmost. Extracted from %s in The Ultimate Oldschool' % fon,
    '// PC Font Pack v2.2 (https://int10h.org/oldschool-pc-fonts/), (c) 2016-2020',
    '// VileR, licensed under CC BY-SA 4.0. See third_party/oldschool-pc-fonts/.',
]
for c in range(256):
    lines.append(', '.join('0x%02x' % b for b in glyphs[c * height:(c + 1) * height]) + ',  // 0x%02X' % c)
open(sys.argv[2], 'w').write('\n'.join(lines) + '\n')
