# Blockwright — plan

Native ANSI/IRC art editor. C++20, Qt 6 Widgets, Linux first, then macOS and Windows.
Feature baseline is Moebius; the additions are what IRC block art needs:
layers, script-generated layers, a live IRC byte budget and a craft lint built into the editor.

## 1. Goals and non-goals

**Goals**
- Moebius-level drawing tools, keyboard-first.
- Layers as real composited stacks, not draw-order functions.
- Generator layers: a script produces cells, the editor shows the result as a layer, re-runs it when parameters change.
- Our own project format that holds layers, scripts, references and metadata.
- Export `.ans` (with SAUCE) and IRC `.txt` (mIRC codes) that fit IRC line limits, checked live while drawing.
- A headless CLI from the same core, so scripts can render, check and export without the GUI.

**Non-goals for v1**
- Collaboration server (Moebius has one; nobody here uses it).
- RIPscrip, PETSCII, Sixel.
- Animation (see Phase 7).
- Bitmap font editor.

## 2. Why build it (what Moebius lacks)

- Unmaintained: last release 1.0.29 in April 2021, Electron 28, crashes on Ubuntu 24.04 (#245).
- No layers (#148), no TheDraw fonts (#226), no 24-bit editing (#157), no UTF-8 import (#160).
- Its UTF-8 export has a light-grey-becomes-white bug (#173).
- It opened our pieces as blank canvases when the SAUCE FileSize was 0.
- Nothing knows about IRC: no byte meter, no mIRC export, no preview of how a line gets cut.

Icy Draw (Rust, egui) is the closest modern editor and has layers, TDF fonts and Lua.
It is the reference design for layers and the file format, not a base to fork.

## 3. Lessons from our ANSI projects

These are what make it more than a Moebius clone.

| Lesson (source) | What the editor does about it |
|---|---|
| Half-block pixels and ░▒▓ shade cells collide: a pixel plotted into a shade cell leaves a fallback speck; outlines break shading; `plot` erases text | Separate layer kinds: **pixel layer** (2× vertical resolution, resolved to ▀▄█ at composite time), **cell layer** (glyph + fg + bg), **text layer**. Compositing rules are defined once, in the core. |
| Every piece ends with hand-written byte-budget hacks: iCE space runs, ▀/▄ flips, "settle to left neighbour" (`downfall-of-numenor`, `funkpower-joust`) | **Live per-row IRC byte meter** in the gutter, red past the budget. The optimisations become export options, not scene code. |
| Raw `.ans` line length overstates IRC cost by 50–100 bytes; budgets were measured three different ways | One encoder (ans2irc's) is the only source of truth for byte counts, in the meter, the lint and the export. |
| Budget is 407 bytes per line on EFnet via WeeChat; 512 minus `:mask PRIVMSG target :\r\n` in general | Budget set per project: preset "EFnet/WeeChat 407", or computed from a nick mask + channel. |
| Lettering most often fails on a letter that misreads at 1× (C/G, R/A, 0/D) | A **1× preview dock** that is always visible, plus an "IRC view" rendered in the mIRC palette with WeeChat's cuts. |
| Speckle, stray cells, same-colour merges, dotted diagonals and flat areas spoil a piece | **Lint panel** running the lint rules live, each finding clickable to its cell. |
| IRC drops trailing black cells and blank edge rows, so frames on the last column vanish | Lint warning + export option to pad with black-on-black █. |
| `• ★ … ► “”` have no CP437 code and save as `?` | Glyph picker only offers mappable glyphs in CP437 projects; lint flags any that slip in. |
| Shading ladder: dither only between ramp neighbours, 1–3 cells per rung | Shade brush and gradient tool walk a named **ramp** (fire, sunset, ice, steel, skin), never arbitrary colour pairs. |
| Mirrored shapes come out a cell off with half-to-even rounding; small circles need tuned radii | Shape rasterisers in the core with tests for symmetry; mirror mode is a canvas transform, not a re-draw. |
| A piece is worked as named layers, each looked at alone and in partial stacks | Layers keep that model, and an imported scene (§7) opens with each of its layers an editor layer. |
| A piece carries notes, a log and earlier versions | Project format has room for notes and versions/snapshots (§6). |

## 4. Architecture

```
blockwright/
  core/      C++20 library, Qt-free except QtCore types where it helps
             cell model, palettes, CP437 tables, layers + compositor,
             ANSI/SAUCE/XBIN/mIRC/UTF-8 codecs, shape rasterisers, lint, project I/O
  render/    glyph atlas + canvas renderer interface (QPainter first)
  app/       Qt 6 Widgets GUI: tools, docks, undo stack, preferences
  cli/       headless: render PNG, convert, lint, export, run generators
  tests/     Catch2; golden files from funkpower's pieces
```

- **Cell model:** codepoint (Unicode internally, CP437 mapping at the edges), fg/bg as palette index *or* RGB, attribute bits, transparent flag.
- **Renderer:** pre-built glyph atlas per font, `drawImage` from source rects, dirty-region repaint, integer zoom with smoothing off. Behind an interface so a `QRhiWidget` shader renderer can replace it if large canvases get slow. Avoids the GPU-driver problems that hit Moebius (Electron) and Icy Draw (wgpu).
- **Undo:** `QUndoStack` with commands that store cell deltas per layer.
- **Fonts:** bundle the CP437 VGA 8×16 / 9×16 family first; Amiga fonts later once their licences are checked.
- **Build:** CMake + Qt 6.8 LTS, `vcpkg` or system Qt on Linux. CI on GitHub Actions for Linux; macOS/Windows jobs added in Phase 6.
- **Licence:** GPL-3.0-or-later, matching ans2irc. Compatible with Apache-2.0 (Moebius) and MIT/Apache (Icy Draw) if code is borrowed; Qt is LGPLv3, linked dynamically.

## 5. Layers

Modelled on Icy Draw's layer record, trimmed to what we need.

| Property | Notes |
|---|---|
| kind | `cells`, `pixels`, `text` (TDF live text), `generator`, `reference` (image, not exported) |
| offset x/y, width/height | Layers can be smaller than the canvas and moved |
| visible, locked, alpha-locked | Standard |
| mode | normal, chars-only, colours-only (repaint colours without touching glyphs) |
| opacity rule | Cells are opaque or transparent; no blending, because the output is 16 colours |
| group | Phase 3b; pass-through only |

- **Composite** is defined bottom-up per cell. A `pixels` layer over a shaded cell replaces the half it covers and re-picks ▀/▄ with the right fg/bg, instead of leaving a speck.
- **Sample merged** toggle for the eyedropper and fill.
- **Bake** turns a text or generator layer into plain cells.
- Exports always flatten the visible composite.

## 6. Project format

Single file, zip container (like `.ora`/`.kra`), with an unpacked-directory form of the same layout for git and agents.

```
manifest.json      format version, canvas size, palette, font, letter spacing,
                   iCE, IRC budget profile, SAUCE fields, layer order
layers/<id>.json   layer properties
layers/<id>.cells  binary cell data (versioned, zstd)
scripts/<id>/      generator source + params.json + cached output
refs/              reference images
notes.md           brief, plan, notes
snapshots/         optional saved rounds (r1, r2…), each a flattened .ans
thumbnail.png
```

- Readers refuse unknown major versions instead of dropping data on re-save (Icy Draw's rule).
- CLI `pack` / `unpack` converts between zip and directory.

## 7. Scripts and generator layers

Recommended: **external-process generators first, embedded Lua later.**

- A generator is any executable that reads `params.json` + canvas info on stdin and writes an `.ans` on stdout. Black-on-black space is opaque; transparency comes from the layer's mask or a chosen key colour.
- Existing Python `ansidraw` scenes work unchanged: one scene layer becomes one generator layer.
- The editor shows parameters as a form (sliders, colours, ramps), re-runs on change, caches output in the project.
- Tradeoff: process spawn per run (tens of ms) and Python as a runtime dependency, against zero embedding work and full reuse of the scene's own Python.
- Phase 7: embedded Lua 5.4 via sol2 (MIT, header-only) for in-editor brushes and fast generators; sandboxed, no interpreter to ship. QJSEngine is the fallback if sol2's slow release pace becomes a problem.

## 8. Formats

| Format | Read | Write | Notes |
|---|---|---|---|
| `.ans` + SAUCE | v1 | v1 | Real FileSize; title 35 / author 20 bytes; iCE, 8/9 px, aspect flags; COMNT block; trailing-black trim; CRLF rules as common writers have them. Parse semantics follow libansilove (`src/loaders/ansi.c` @ `ced66a0`): CR doesn't move the cursor; TAB, CUF and a CUP column can take the cursor to the width, not the last column, and the next byte that draws or moves the cursor first wraps it to the next row; EL is skipped; ED 2 clears and homes; an ESC not followed by `[` skips only itself; SGR and CSI t skip empty fields, so `ESC[m` does nothing; SGR 7 as its invert: background = foreground without its bright bit, foreground = background plus the foreground bright bit, with PabloDraw CSI t colours set aside while reversed; SGR 1 drops a CSI t foreground and SGR 5 a CSI t background, iCE or not, and a dropped one stays dropped after SGR 27; CSI t acts only on selector 0 (background) or 1 (foreground), reads a missing channel or one past 255 as 0, and black (0,0,0) means no 24-bit colour. Divergences kept on purpose, each pinned by a test: (1) when a glyph fills the last column, a LF after nothing but CRs, SGRs and CSI t ends that row instead of adding a blank one; libansilove wraps on the CR and the LF adds a row, which would put a blank row under every row of the 20 avatars, comic-lulzy, baud-dudes, hb-burnlootmurder and indian-tier. Any other escape in between (EL, an unknown or private CSI, a malformed CSI, a lone ESC) wraps first, as libansilove does on every ESC, so `80×X ESC[K CR LF Y` is 3 rows high. After a CUF or TAB to the edge, CR LF adds the row as in libansilove. (2) Where libansilove computes values past the palette we stay in it: bold on an iCE bright background (`1;5;31;44;7`) ORs the bright bit (libansilove adds it and reaches index 20), a repeated SGR 1 or 5 doesn't add 8 again, and a CUP row or column of 0 is read as 1 (libansilove computes -1). (3) SGRs libansilove doesn't read keep their usual meaning: 21 and 22 bold off, 25 blink off, 39 and 49 default colours, 90–97 and 100–107, 38/48;5;n and 38/48;2;r;g;b (libansilove reads the sub-parameters as SGRs of their own). (4) An SGR field that isn't plain digits is skipped wherever it sits and the other fields still apply: a private field (`?999`, `>1`) and a colon-form colour (`38:5:n`, `38:2:r:g:b`) alike. libansilove reads such a field as 0 and resets everything, so `ESC[31m ESC[999;?999m A` and `ESC[31m ESC[38:5:2;44m A` stay red here. Plain digits too large for libansilove (`4294967296`) read as 0 and reset there too; here they are an unknown SGR, like `999`, and do nothing. (5) A cursor move with more than one parameter (`ESC[2;5C`) uses the first; libansilove's strtonum fails on the whole field and moves 1. (6) A CSI longer than 14 bytes acts as usual; libansilove looks for the final byte only in the first 14 (`ANSI_SEQUENCE_MAX_LENGTH`, `ansi.c:40/220`) and otherwise draws the sequence as text. (7) An unknown final byte ends the CSI, which is skipped; libansilove scans on past it for a known final byte, so `ESC[1zAB` moves up and draws only B there. SAUCE TInfo2 is the minimum height. Rows exist only once something is drawn on them, so parse time follows the bytes and cells drawn, not how far the cursor moves. A bright background forces iCE on when writing (100–107 aren't written); blink lost that way, or a glyph written as `?`, makes `cli convert` refuse without `--allow-lossy` |
| IRC `.txt` (mIRC codes) | v1 | v1 | Encoder = ans2irc; per-line budget; optional fit-to-budget with least-visible merges |
| PNG | — | v1 | 1× and scaled; IRC-view variant. `render/` draws with the bundled IBM VGA 8×16: 9 px when SAUCE letter spacing is 9 px (or `--font-width 9`), the 9th column repeating the 8th for 0xC0–0xDF only; iCE off draws blink cells in their visible phase with the dark background, as libansilove does; 24-bit cells draw their RGB; integer scale 1–8, nearest neighbour. The legacy aspect flag is not applied (square pixels; `cli render` prints a note). Golden tests compare cells with libansilove output and allow only the 28 glyphs where its built-in font differs from the IBM VGA one (`tests/data/render/ansilove/MANIFEST`) |
| UTF-8 ANSI | v1 | v1 | For terminals and paste |
| `.bin` / `.xb` | Phase 6 | Phase 6 | XBIN header + RLE |
| TDF fonts | Phase 5 | — | Text layers; reuse `~/tdfiglet/fonts` |
| PabloDraw 24-bit `CSI t` | v1 (import, quantised) | later | |

**ans2irc** is linked as a Rust static library through a C ABI (Corrosion in CMake), so the meter, lint and export share one encoder. The build needs a Rust toolchain.

## 9. Pipeline integration

- **Publish to library:** write `.ans` + `.txt` into the `ascii` repo's `funkpower/`, update `.tags.json` and `.dates.json`.
- **Send to WeeChat:** copy to a configured folder on the WeeChat host as `<slug>-r<N>.ans` and send `*/ascii reload` to the fifo.
- **CLI:** `render`, `lint` (the lint rules), `export --irc`, `run-generators`.
- Later: an MCP server over the CLI (Icy Draw ships one).

## 10. Advanced brushes (shading and detail)

The core of what an editor adds: the shading and detail moves of hand-made block art, done at the cursor. Each brush is a core-library function with tests, wrapped by a GUI tool.

**Shading**
- **Ramp shade brush:** lighten/darken one step along the layer's ramp per pass, using the ladder A → B░A → B▒A → B▓A → B. Never mixes colours that aren't ramp neighbours.
- **Form shading brush:** paint over a masked shape and it shades as a dome, cylinder or flat plane lit from the scene light vector, with rim light and specular as toggles.
- **Soft brush / falloff:** size and hardness; the falloff becomes banded rungs 1–3 cells wide, not noise.
- **Ramp gradient tool:** linear and radial, cell-banded or 4×4 Bayer, along a ramp.
- **Blend/smudge:** pulls ramp values along the stroke and re-quantises to the ladder.
- **Despeckle brush:** majority filter that removes isolated cells and stray shade fallbacks (the lint's speckle).

**Detail**
- **Half-block pencil** with 4-connected stroke mode, so diagonals don't dot.
- **Outline brush:** traces the edge of a shape or colour region in the next-darker ramp colour, so same-colour shapes stop merging.
- **Rim/highlight brush:** brightens only cells on the light-facing edge of the region under the cursor.
- **Texture stamps:** `lumps()`-style scales, foliage, brick, hoard and noise, seeded and density-controlled.
- **Pattern/glyph spray** for stars, smoke and haze.
- **Clone stamp** and **symmetry** (mirror X/Y, radial).

**Common to all brushes**
- Respect layer alpha-lock and selection masks.
- Tablet pressure through `QTabletEvent` maps to size or strength.
- Live preview of the stroke before commit; one undo step per stroke.

## 11. Phases

Estimates are focused working days.

| Phase | Scope | Est. | Done when |
|---|---|---|---|
| 0. Core + CLI | Cell model, CP437, ANSI parse/write, SAUCE, PNG render, `cli render/convert`; golden tests against 20 library pieces and libansilove output | 3–4 d | Every `funkpower/` piece round-trips byte-identical where possible and renders the same as libansilove |
| 1. Viewer + keyboard editor | Qt window, atlas renderer, zoom, 1× dock, keyboard mode, F-key glyph sets, palette, undo, open/save `.ans` | 4–5 d | Can open, type into and save a piece |
| 2. Moebius parity | Half-block, shade, custom-glyph, colorize brushes; line/rect/ellipse; fill; eyedropper; selections (move, flip, rotate, transparent/under paste); mirror; guides 80×25/40/50; reference image; SAUCE dialog; iCE; 8/9 px | 8–10 d | A piece can be drawn from blank without leaving the editor |
| 2b. Advanced brushes | §10, all of it | 6–8 d | A shaded sphere can be painted by hand that passes the lint's speckle and ladder checks |
| 3. Layers + project format | Layer kinds, compositor, layer panel, project zip/dir, snapshots | 5–6 d | Shade under a pixel layer composites with no specks |
| 4. IRC tooling | ans2irc link, IRC export, live row meter, IRC view, lint panel, publish/WeeChat buttons | 4–5 d | A 120-row piece exports with every line ≤ 407 bytes and the meter agreed beforehand |
| 5. Generators + text | External generator layers with param forms; TDF text layers; ramp-aware gradient tool | 5–6 d | An existing `scene.py` opens as editable layers |
| 6. macOS + Windows | Packaging (AppImage/Flatpak, .dmg, MSI), CI matrix, XBIN/BIN | 4–6 d | Signed-or-not builds install on all three |
| 7. Later | Embedded Lua, animation/ANSImation, Amiga fonts, MCP server, 24-bit editing | — | — |

v1 = Phases 0–4 including 2b: about 6–7 working weeks.

## 12. Decisions

- Name: Blockwright.
- ans2irc: linked as a Rust crate, one encoder.
- Generator output: `.ans` on stdout.
- Hosting: private on git.pumpin.online (`funkpower/blockwright`), public later.
- Default canvas: 80×40.
- Every PR gets an adversarial Codex review before merge.
- Every merge to main installs a dev build on the dev machine (`blockwright-dev`).
- Bundled font: IBM VGA 8×16 from VileR's Ultimate Oldschool PC Font Pack (CC BY-SA 4.0, compatible one way with GPLv3), in `third_party/oldschool-pc-fonts/` and credited in `NOTICE`. `LICENSE`, `NOTICE` and the font's licence and README install to `<prefix>/share/doc/blockwright/`, and `blockwright-cli --version` credits the font.
