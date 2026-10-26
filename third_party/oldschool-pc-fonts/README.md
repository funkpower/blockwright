# IBM VGA 8×16 and IBM PC BIOS 8×8 (The Ultimate Oldschool PC Font Pack)

- **Fonts:** `Bm437_IBM_VGA_8x16.FON`, the IBM VGA 8×16 text-mode font, and `Bm437_IBM_BIOS.FON`, the IBM PC BIOS 8×8 font, both in CP437 order.
- **Source:** The Ultimate Oldschool PC Font Pack v2.2 (2020-11-21) by VileR, <https://int10h.org/oldschool-pc-fonts/>; downloaded as `oldschool_pc_font_pack_v2.2_FULL.zip` (SHA-256 `21b3c0a3770ef0afc46564760613d7b078f4fcc9ed93db4b829a440b68822e08`), files `fon - Bm (windows bitmap)/Bm437_IBM_VGA_8x16.FON` (SHA-256 `b6bcbfbadeb1af3532791b2e42d3806e281dd6e335ebc02c46a397919ec0854b`) and `fon - Bm (windows bitmap)/Bm437_IBM_BIOS.FON` (SHA-256 `1eee9aea62b1aa6cd6970020f5fef82b4da2319994e1feb2eef9819de7fd7dfc`).
- **Copyright:** © 2016-2020 VileR.
- **Licence:** Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0); the full text is in `LICENSE`, as shipped in the pack.
- **Extraction:** `unzip oldschool_pc_font_pack_v2.2_FULL.zip 'fon - Bm (windows bitmap)/Bm437_IBM_VGA_8x16.FON' 'fon - Bm (windows bitmap)/Bm437_IBM_BIOS.FON'`, then `fon2inc.py <Bm437_IBM_VGA_8x16.FON> core/src/font_ibm_vga_8x16.inc` and `fon2inc.py <Bm437_IBM_BIOS.FON> core/src/font_ibm_bios_8x8.inc`. The resulting tables have these SHA-256 hashes, which `tests/test_render.cpp` and `tests/test_face.cpp` check against the compiled-in tables:
  - VGA 8×16, 4096 bytes (256 glyphs × 16 rows): `a8bad6fd78475a6bc2a05438c19207a9bb8c0f4f4099f60384e158cdc3eba580`
  - BIOS 8×8, 2048 bytes (256 glyphs × 8 rows): `75c79a7e7fa423dda67ec6d6d76cec86b63f85677726368750c75b0920ddf319`

  Last re-derived from a fresh download of the pack on 2026-10-25: the archive and member hashes above matched, the regenerated VGA `.inc` was byte-identical to the committed one, and the BIOS `.inc` was generated then.
- **What Blockwright uses:** the 256 glyph bitmaps of each font only, converted by `fon2inc.py` into the two `.inc` tables and compiled into the core library. The renderer draws cells with the VGA font, and the `block` and `vga` text faces draw lettering with the BIOS and VGA fonts. The bitmaps are unchanged: the 9th pixel column of 9 px rendering is added at draw time, not stored, and the slashed 0 of the `block` face is the BIOS font's own 0.

CC BY-SA 4.0 is one-way compatible with GPLv3 (Creative Commons compatibility list, 2015): an adaptation of BY-SA 4.0 material may be licensed under GPL-3.0, so the fonts can ship inside a GPL-3.0-or-later program. The attribution above and this notice travel with the source and binaries: `cmake --install` puts this README, `LICENSE` and the top-level `NOTICE` in `<prefix>/share/doc/blockwright/`, and `blockwright-cli --version` names the fonts, their author and licence.
