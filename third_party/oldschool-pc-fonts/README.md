# IBM VGA 8×16 (The Ultimate Oldschool PC Font Pack)

- **Font:** `Bm437_IBM_VGA_8x16.FON`, the IBM VGA 8×16 text-mode font in CP437 order.
- **Source:** The Ultimate Oldschool PC Font Pack v2.2 (2020-11-21) by VileR, <https://int10h.org/oldschool-pc-fonts/>; downloaded as `oldschool_pc_font_pack_v2.2_FULL.zip` (SHA-256 `21b3c0a3770ef0afc46564760613d7b078f4fcc9ed93db4b829a440b68822e08`), file `fon - Bm (windows bitmap)/Bm437_IBM_VGA_8x16.FON` (SHA-256 `b6bcbfbadeb1af3532791b2e42d3806e281dd6e335ebc02c46a397919ec0854b`).
- **Copyright:** © 2016-2020 VileR.
- **Licence:** Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0); the full text is in `LICENSE`, as shipped in the pack.
- **Extraction:** `unzip oldschool_pc_font_pack_v2.2_FULL.zip 'fon - Bm (windows bitmap)/Bm437_IBM_VGA_8x16.FON'`, then `fon2inc.py <that file> render/src/font_ibm_vga_8x16.inc`. The resulting 4096-byte table (256 glyphs × 16 rows) has SHA-256 `a8bad6fd78475a6bc2a05438c19207a9bb8c0f4f4099f60384e158cdc3eba580`; `tests/test_render.cpp` checks the compiled-in table against it. Last re-derived from a fresh download of the pack on 2026-10-22: the archive and member hashes above matched, and the regenerated `.inc` was byte-identical to the committed one.
- **What Blockwright uses:** the 256 glyph bitmaps only, converted by `fon2inc.py` into `render/src/font_ibm_vga_8x16.inc` and compiled into the render library. The bitmaps are unchanged; the 9th pixel column of 9 px rendering is added at draw time, not stored.

CC BY-SA 4.0 is one-way compatible with GPLv3 (Creative Commons compatibility list, 2015): an adaptation of BY-SA 4.0 material may be licensed under GPL-3.0, so the font can ship inside a GPL-3.0-or-later program. The attribution above and this notice travel with the source and binaries: `cmake --install` puts this README, `LICENSE` and the top-level `NOTICE` in `<prefix>/share/doc/blockwright/`, and `blockwright-cli --version` names the font, its author and licence.
