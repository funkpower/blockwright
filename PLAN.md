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
- **Renderer:** pre-built glyph atlas per font, `drawImage` from source rects, dirty-region repaint, integer zoom with smoothing off. Behind an interface so a `QRhiWidget` shader renderer can replace it if large canvases get slow. Avoids the GPU-driver problems that hit Moebius (Electron) and Icy Draw (wgpu). In `render/`: `CanvasRenderer` (the interface) and `AtlasRenderer`, which keeps a 1× cache filled from a `GlyphAtlas` (256 glyphs × 16 palette colours, 24-bit colours made on demand) and repaints only the cells marked dirty; views scale the cache by the zoom. Views draw in view pixels, each a whole block of `max(1, floor(dpr))` device pixels starting on the first whole device pixel inside the widget (`app/device_grid.h`), so under fractional display scaling (1.25, 1.5) every cell still covers a whole number of device pixels at every zoom and the 1× preview shows one block per canvas pixel; at 1.5 that makes the art a little smaller than the UI around it. It must draw exactly what `renderCanvas` (the CLI's PNG renderer) draws, and the tests compare the two.
- **Undo:** `QUndoStack` with commands that store cell deltas per layer. The deltas are `CellEdit`s from `core/` (`edit.h`): keyboard edits are core functions that return one plus the next cursor, and the canvas view sends every edit through one `commit()`, which pushes it on the document's stack as one command (one per key; an edit that changes no cell is not pushed). Undo and redo repaint only the cells the edit changed and put the cursor back where the edit was made or left it. The modified flag is the stack's clean state: save marks it clean and open/new clear the stack, so undoing back to the saved cells clears the flag. Edit > Undo/Redo use the platform keys (Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y).
- **SAUCE, iCE and letter spacing** are document state with undo. File > SAUCE... edits title (35), author (20), group (20), date (CCYYMMDD) and the COMNT lines (64 each, up to 255); the fields take only text CP437 can store, and OK waits until every field fits. A field (or the comment block) left as shown comes back byte for byte and is not checked, so a malformed date from a file (`00000000`) doesn't block editing the rest, and trailing blank comment lines from the record stay. View > iCE Colours and View > 9 px Letter Spacing are checkable. Each change is one undo step (`FormatCommand`, the SAUCE record and iCE before and after), so it sets the modified flag; the record's other fields (font, aspect, the flags not being changed) are kept, and a dialog accepted unchanged adds nothing. iCE follows Moebius and PabloDraw: blink and a bright background are one bit (`iceCell` in `core/`), so the cells stay as drawn and the toggle changes how they show and save. With iCE off a bright background shows as blink on its dark colour, drawn in its visible phase as `renderCanvas` draws blink, and saves as SGR 5; with iCE on a blink cell shows and saves as the bright background. A file whose iCE flag is off but which sets bright backgrounds anyway (SGR 100–107) opens with iCE on, as `writeAnsi` and `cli render` already treat it; its blinking cells lose the blink on open and keep their dark background, as `writeAnsi` writes them and `renderCanvas` draws them, and until the next save the save warns that the original file's blinking cells lost their blink when it was opened (a fact about the open, so it stays true if iCE is turned off or the cells are erased). `writeAnsi` doesn't trim a trailing blinking space as a blank when it writes without iCE, so a bright-black background survives an iCE-off save; with iCE on the blink is dropped and the space is trimmed like any black space, as the editor writes it. 9 px redraws the canvas, preview and zoomed view with 9-px cells; turning 9 px on for a canvas too large for the 1× cache at 9 px leaves it at 8 px with a status-bar message, and opening a file that asks for 9 px and is too large at 9 px is refused, like any canvas too large to display. None of the three has a shortcut.
- **Keyboard mode** follows Moebius:
  - F1–F10 type the glyphs of the current F-key set in the pen's colours. The sets are Moebius's 16 (from PabloDraw, `core/glyph_sets.h`), all CP437 glyphs `.ans` can hold; set 6 (░▒▓█▀▄▌▐■·) is the default. Ctrl+, and Ctrl+. step to the previous and next set (wrapping), Ctrl+/ goes back to the default, and Ctrl+Shift+F1–F10 pick sets 1–10; sets 11–16 have no direct chord and are picked from the Glyphs menu, which lists all 16, or by stepping. The status bar shows the current set and its tooltip the keys.
  - The direct chords deviate from Moebius on purpose. Moebius uses Alt+F1–F10 and Alt+Shift+F-keys, but desktops take those before the editor sees them: Alt+F4 closes the window on GNOME, KDE Plasma and Xfce, and Alt+F2/F5–F8/F10 are GNOME window commands (Alt+F6–F11 on Xfce, Alt+F3 on KDE). Ctrl+F1–F4 switch desktops on KDE (Ctrl+F1–F12 on Xfce), and Ctrl+Alt+F-keys switch Linux VTs. None of the three desktops binds Ctrl+Shift+F-keys by default. On macOS Qt maps Ctrl to Cmd, so the chord is Cmd+Shift+F-key (with fn on keyboards whose top row defaults to media keys); macOS binds Control+Shift+F6 but no Cmd+Shift+F-key. A test keeps every shortcut off Alt+F-keys and bare Ctrl+F-keys.
  - The palette dock shows the 16 colours and the pen: a left click picks the foreground, a right click the background. Ctrl+Up/Down step the foreground, Ctrl+Left/Right the background (wrapping), Ctrl+D resets the pen to grey on black.
  - Typing on the last column leaves the cursor there, past the glyph: Backspace then clears that cell instead of the one to its left, and any move ends it.
  - A key is text when it gives printable text, unless Ctrl or Meta is held without Alt. So AltGr (Ctrl+Alt on Windows) and macOS Option characters type, and Ctrl/Cmd chords stay shortcuts.
- **Tools:** the canvas view hands mouse and key events (in cells) to the active tool first; whatever it doesn't take goes on to keyboard mode. A `Tool` (`app/tool.h`) has an id and a name, `activate`/`deactivate`, `mousePress`/`mouseMove`/`mouseRelease`/`keyPress` returning whether it took the event, `cancel` (drop what is in progress without an edit), `cancelDrag` (end a drag the mouse can no longer finish, without an edit), `dragging` (whether a drag is in progress), `settingsChanged` (the view's mirror mode or selection changed: a drag in progress redoes its edit and preview) and `finish` (write it as an edit), `paintCells` for cells in progress (drawn over the canvas, under the reference image and guides), `paint` for its marks (over everything but the cell cursor) and a `changed` signal. It edits through `CanvasView::applyEdit`, so every edit is one undo command and mirror mode applies to it; a tool that has already mirrored its edit (the drawing tools) passes `mirrored` so the view doesn't mirror it again, and `CanvasView::mirrored` gives a tool the view's mirror of an edit for its preview. The view calls `cancel` when the document changes under the tool (undo, redo, an edit the view didn't push, a new canvas). It calls `cancelDrag` when it loses focus during a drag the tool took: a floating selection being dragged goes back to where the drag picked it up and stays floating, and a new selection rectangle ends as far as it got (a menu popup doesn't count, so the menus can still act on a floating selection). It calls `finish` before an edit of its own (typing, Backspace, Delete, F-keys), which then lands on top, and the window calls it before Save and Save As write and before the unsaved-changes check that New, Open and closing the window make. So a floating selection is stamped before any save or unsaved-changes check, and stays stamped (an ordinary undoable edit) if the user then backs out: Cancel on the unsaved-changes prompt, the Open dialog or the lossy-save warning, or a lossy save that is refused. A paint that throws leaves the rest of that paint out (the canvas too, if the renderer threw) rather than escaping `paintEvent`; the cell cursor is still drawn. A `ToolRegistry` holds the tools and the active one; the Tools menu is built from it (Keyboard Ctrl+K, Select Ctrl+E, and the drawing tools below). A `ToolEvent` carries the cell and, for the pixel tools, the half-block row under the pointer (`halfRow`, twice the cell row plus one in a cell's bottom half). A tool's `status()` text shows in the status bar while it is active.
- **Selections** (`core/selection.h`, `app/select_tool.h`): a rectangle for now, with a mask possible later behind `Selection::contains`. The selection belongs to the view (`CanvasView::selection`, `selectionChanged`), which draws its outline, so it lasts across tool switches; a new canvas clears it. Drag to select; drag inside the selection or press an arrow to lift its cells into a floating selection, which is only drawn (as the stamp would leave the canvas, rendered at 1× and scaled as it paints) until Enter or a click stamps it as one edit; Esc ends any drag and puts it back, or deselects. A floating selection never outlives a change it didn't make: switching tools, typing or saving stamps it first, and so does the unsaved-changes check, so a paste on a clean canvas counts as an unsaved change; undo, redo or a new canvas cancels it, leaving the canvas as the undo left it and the selection on the cells it was lifted from (or none, for a paste). Ctrl+X/C/V cut, copy and paste through the tool's own clipboard (cells with colours; nothing goes to the system clipboard); a paste floats at the cell cursor. Delete clears the selection (in keyboard mode it still clears the cell). The Selection menu has Select All (Ctrl+A), Deselect (Ctrl+Shift+A), Flip Horizontally/Vertically (Ctrl+Shift+H/V), Rotate Clockwise/Counterclockwise (Ctrl+R, Ctrl+Shift+R) and the paste mode: normal; transparent (the block's blank cells, a space or NUL on black without blink, don't write); under (only the canvas's blank cells are written). Flips remap the glyphs that face one way (▀↔▄, ▌↔▐, box corners and tees, slashes, brackets, arrows); rotation turns half blocks and box drawing a quarter turn (▀→▐→▄→▌) and keeps the selection's top-left corner. Glyphs that only flip turn in 4-cycles that make a half turn equal both flips for all 256 CP437 glyphs, and four turns the identity: brackets ( [ ) ], < « > », { ≤ } ≥, and ¬ ⌡ ⌐ ⌠ (« » flip sideways, ⌠ ⌡ upside down). For the three bracket cycles a turn and a flip don't commute as they would on a picture (Rotate Clockwise then Flip Horizontally makes ( a ], Flip Horizontally then Rotate Counterclockwise makes it a [), and no mapping can fix that: a vertical flip leaves those glyphs alone, so their half turn is just the sideways flip. Undo restores cells, not the selection.
- **Drawing tools** (`app/draw_tools.h`, phase 2.1), in the Tools menu: Half-Block Brush (Ctrl+B), Shade Brush (Ctrl+Shift+B), Glyph Brush (Ctrl+G), Colorize Brush (Ctrl+Shift+C), Line (Ctrl+L), Rectangle (Ctrl+U), Ellipse (Ctrl+Shift+O), Fill (Ctrl+J) and Eyedropper (Ctrl+I), with Filled Shapes (Ctrl+Shift+F) toggling rectangle and ellipse between outline and filled. Ctrl+Shift+U is left alone because IBus takes it for Unicode entry, and Ctrl+F is kept for Find; no two actions share a shortcut (a test checks it). Every one is a drag: it is previewed over the canvas and lands on release as one undo step, and the preview is the edit that lands: toggling mirror mode or Filled Shapes, or changing the selection, mid-drag redoes the preview at once. Esc drops it, and so does the undo key (Ctrl+Z) pressed during the drag, which undoes nothing else, even when there is nothing to undo; any undo or redo (from the menu or the stack), a format change (iCE, 9 px) and its undo or redo also drop it; switching tools lands it. Fill floods once, from where it is pressed; the release point doesn't move it. The preview renders only the cells the edit changes, and a filled shape recomputes only the shape on each move. The left button draws with the pen's foreground, the right with its background; the shade brush builds up one rung a pass with the left button and takes one down with the right (a space, ░, ▒, ▓ or █ moves, written in the pen's colours both ways, as Moebius does; text, ▀, ▄ and every other glyph is left alone, both ways), the glyph brush writes its glyph with the left and erases to a blank cell with the right, and the colorize brush sets the foreground with the left and the background with the right, keeping the glyphs. The glyph brush's glyph is █ until F1–F10 pick one from the current F-key set while it is active; the status bar shows it. The brushes are size 1 for now. Selections clip every tool: the view's selection becomes the stroke's mask (`selectionMask` in `brush.h`).
- **Half-block pixels** (`core/halfblock.h`, `core/shapes.h`): the half-block brush, line, rectangle, ellipse, fill and eyedropper work on pixels two to a cell. A cell holds pixels when it is a space, █, ▀ or ▄; plotting into any other cell replaces it, its background colour filling the other half. Cells are written in one canonical way: one colour is a space on a dark background or █ on black for a bright one, two colours are ▀ unless only ▄ keeps the background dark; two bright colours need a bright background (iCE). Every tool reads a cell by the colours it shows: with iCE off a bright background shows as its dark colour (and blinks), so the halves, the fill's colour test and the eyedropper all see the dark one. With iCE off a write puts the bright colour in the foreground, and when both halves would be different bright colours the half already there keeps its colour and the one being painted shows as its dark counterpart (only one bright fits; painting both halves of a cell gives the colour itself). A pixel write keeps the cell's other attributes (Blink included) and makes a transparent cell opaque; locked cells are never written. The pencil joins its points with 4-connected lines. A line is drawn the same whichever end it starts from (its ends are ordered top to bottom first). An ellipse is the box's pixels whose centres lie within the ellipse through the box's outer pixel centres, each radius r tuned to sqrt(r(r + 1)) so small circles read round (5 × 5 gives rows 3, 5, 5, 5, 3), and its outline is the filled pixels with a neighbour (of eight) outside, which is 4-connected. Fill floods the 4-connected pixels of one colour, through pixel cells only, so text and shades bound it. The eyedropper picks the half's colour in a pixel cell and otherwise the cell's foreground (left button) or background (right) into the pen.
- **Mirror mode** (Edit > Mirror Mode, Ctrl+M) is a transform of every edit the view makes: a change at column x is repeated at width − 1 − x with its glyph flipped horizontally, in integer maths; where the edit already changes the mirrored cell (the centre column of an odd width, an edit spanning both halves) its own change wins. The pixel tools (half-block brush, line, rectangle, ellipse, fill) mirror pixels, not cells: the mirrored pixel goes into the mirror cell's own half and its other half stays as it was, and fill paints the mirror image of the region it flooded. The shade brush steps the mirror cell's own level. The glyph and colorize brushes mirror the cells the whole stroke writes. The selection clips each side on its own: a stroke and its mirror image are each painted where they fall inside the selection (and where the transparent lock allows), so a stroke outside the selection still paints its mirror inside it, and the preview shows the same.
- **Guides and reference image** are view overlays, never saved: View > Guides draws a line after column 80 and every 25, 40 or 50 rows; File > Open Reference Image… shows a PNG or JPEG across the canvas width at its own aspect, one scale both ways (only the part over the canvas is scaled, and each of its pixels is what the whole picture scaled by that factor has there: averaged when the picture is reduced, interpolated when it is enlarged; the cached copy is at most the canvas size at the current zoom and 2^24 px, drawn scaled up past that, with a row more where the picture goes on below the canvas by at least a whole cache row so its bottom edge blends as the whole picture does (by less, the last row is read again, as at the picture's own bottom edge), the same whatever part of the view is repainted; a reduced picture is read and converted at most 2^20 px at a time, a row wider than that in pieces; if scaling fails the status bar says so and it isn't retried until the zoom, canvas or image changes), with View > Reference Image to show or hide it, set its opacity (25–100 %) or clear it. It lasts the session; the project format's `refs/` (§6) will keep it.
- **Fonts:** bundle the CP437 VGA 8×16 / 9×16 family first; Amiga fonts later once their licences are checked.
- **Build:** CMake + Qt 6.8 LTS (6.4 is the minimum, for distro LTS releases), `vcpkg` or system Qt on Linux. CI on GitHub Actions for Linux; macOS/Windows jobs added in Phase 6.
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
- Source: git submodule `third_party/ans2irc` (its URL is in `.gitmodules`) at `a pinned commit`. The ans2irc repo itself is not modified: the C ABI is a crate of ours, `ffi/ans2irc-ffi/` (staticlib, header `include/ans2irc.h`), that depends on the submodule by path and exposes the row encoder and the RGB → mIRC quantiser.
- Corrosion v0.6.1 from FetchContent, pinned by URL_HASH SHA256 `e9e95b1e…babf8e3` (`cmake/Ans2irc.cmake`).
- Export = Blockwright's parser (above) → canvas → ans2irc's row encoder, not ans2irc's own parser, so the CLI export, the editor's export and the meter all measure the same canvas. Export encodes the cells as they are: the 16-colour palette, since the parser quantises 256-colour (`38/48;5;n`), 24-bit (`38/48;2;r;g;b`) and PabloDraw `CSI t` colours to it as it reads them (the table above). A palette index maps to mIRC as ans2irc maps SGR colours (CGA index → its `ANSI_NORMAL`/`ANSI_BRIGHT`). The encoder also maps an RGB cell to the nearest of mIRC's 99, but no cell holds RGB until cells keep it (24-bit editing, §11 phase 7 "Later"). Transparent cells export blank, as `.ans` writes them, and a C0 control or DEL in a cell exports as its CP437 glyph (NUL as a space), never as a raw byte; a C1 control (U+0080–U+009F) exports as `?`, since some clients read `C2 9B` as a CSI. Trailing black spaces and the blank rows above and below the art are dropped as the ans2irc CLI drops them. Divergences from the ans2irc CLI, which parses the file itself: (1) colours: ans2irc keeps 256-colour and 24-bit colours and maps them to the nearest of mIRC's 99 (`38;5;208` → mIRC 53, `38;2;0;128;255` → 59), where Blockwright exports its 16-colour quantisation (mIRC 7 and 10), pinned in `tests/test_irc.cpp`; (2) every parse divergence above where Blockwright follows libansilove and ans2irc does not changes the IRC output too: ans2irc stops a CUF at the last column, libansilove (and Blockwright) at the width, so text after `ESC[nC` to the edge wraps differently, and the CR and TAB rules above (a CR doesn't move the cursor, a TAB can reach the width) give different rows for a file that relies on them. Over the library corpus, which has no 256-colour, 24-bit or `CSI t` colours, the CUF wrap is the only difference seen (1 of 509 files).
- `cli export --irc` encodes losslessly and never cuts: lines over the budget (default 407) are listed, and the file is not written (exit 3) unless `--allow-over-budget`. Fit-to-budget (ans2irc's colour merging) is Phase 4.

## 9. Pipeline integration

- **Publish to library:** write `.ans` + `.txt` into the `ascii` repo's `funkpower/`, update `.tags.json` and `.dates.json`.
- **Send to WeeChat:** copy to a configured folder on the WeeChat host as `<slug>-r<N>.ans` and send `*/ascii reload` to the fifo.
- **CLI:** `render`, `lint` (the lint rules), `export --irc`, `run-generators`.
- **Same-file output:** `cli convert x.ans x.ans` rewrites the file in place through a temporary file (normalising a piece is a real use), while `export` and `render` refuse an output that is their input under any name, since they would put a different format over the source.
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

In the core (`brush.h`): a stroke is a list of dabs joined by 4-connected lines, with a square or round tip (a round tip's rows count as two columns, and every size keeps at least the rows of a smaller one). The tip is 1 to 256 cells wide: a larger size, set or reached through pressure, is taken as 256, far more than an 80- or 160-column piece needs, so a joining line walks at most a few hundred points beyond the canvas. The unclipped footprint leaves out cells past the ends of int. Only the part of a joining line within reach of the canvas (the canvas grown by the tip's half width) is walked, in 64-bit arithmetic, so a dab far off the canvas costs nothing. The walk is the whole line's own cells: it starts from the point where the line comes within reach, with Bresenham's error term worked out there rather than stepped to, so a stroke paints exactly what stamping every dab of the line would. A stroke paints each cell at most once, so one pass of the shade brush moves a cell one rung; a build-up toggle may come later. A dab on the same cell as the last stamps again, so pressing harder without moving grows the brush. A stroke gives a preview canvas and one `CellEdit` (the editor's undo type, each change with its before and after), and that edit is the undo step. Pressure scales size (size × p) or strength (rungs × p, rounded, never below one), so pressure to strength changes nothing at the default of one rung a pass; there is no randomness. Until layers exist, the cell's transparent flag stands in for alpha-lock.

The ladder (`ramp.h`) holds palette colours only; an RGB cell is off every ramp. A shade can be written either way round (B▓A shows as A░B). The brush writes the way that keeps the background dark (0–7) when only one way does, so a piece needs no iCE colours for it; otherwise it keeps the way the cell was written, lighter on top for new shades. Two bright neighbours need a bright background either way: `cli shade` refuses to write a non-iCE piece that would lose its blink to that, and says so. Step counts saturate at the ramp's ends.

The ladder lint (`lint.h`) measures pixel `noise` (at most 0.02, from smaug's 0.033 → 0.016 despeckle), with each row's trailing black spaces trimmed as in the written file, and adds the ladder rules:
- Shades may only join ramp neighbours; a shade of two colours of the ramp that aren't neighbours is flagged whether or not it blinks, and other ramps' colours aren't judged.
- No shade rung may be wider than 3 cells along the gradient (rungs are 1–3 cells wide *along the gradient*, so a band reads as a turn, not a stripe). Only a step of a gradient is judged: a run of a shade rung, across a row or down a column, with a lower rung at one end and a higher one at the other. Its width is the shorter of that run and the rung's longest run the other way, in cells. A flat field of one shade and the plane at a gradient's end aren't judged; black is ink or the ground, not the gradient's dark end, so a plane that runs into black is a plane at the end, as at the canvas edge.
- No rung skips. Each row and each column is read as a sequence of levels (runs of one rung); a jump of more than one rung between two levels is a skip when the level before it or the one after it goes on the same way, so three or more levels climb or fall together (bands 3, 5, 8). Two solid colours side by side are judged like any other levels, so solid DarkGrey then solid White at the top of a climbing gradient (1 2 3 4 then 12) is a skip. An outline that turns back (10, 11 then 8), two levels meeting (a fold, two planes: solid DarkGrey against solid White with no third level, or White against a w▒K face) and black against anything (ink, seams, the silhouette: black and cells off the ramp end a sequence) aren't skips. The one end level left out of a sequence is the outline brush's line: a level at either end (against black, off the ramp or the canvas edge) that is one cell wide, solid, and in the ramp colour next below the darker colour of the level beside it, so DarkGrey round bands 9 to 12 is clean. Every other end level is a rung like the rest, a lighter one (a highlight) included, so a single White cell after 1 2 3 4, and 8, 10, 12 between black, are skips.
- No specks (a lone non-black, non-glint pixel reads as dirt): a cell lighter or darker than all of its neighbours on the ladder, judged only where it has three or more, so the corners of a gradient and the ends of a band aren't. Black cells (ink, a rivet, an eye) and the ramp's top rung (a glint, the highlight) are never specks. Pixel noise can't see shade cells, so this is the speckle rule for shading. Known gap: stripes of two shades a column or a row each are not specks (every cell has a neighbour of its own rung), and noise doesn't see them either; an eye for regular repeats is what catches them for now. Known limits the other way: the one-cell peak of a ball that stops below the top rung is a speck (one), and a one-wide diagonal line one rung off its field is all specks. A Bayer-dithered gradient is speckle too (a 4×4 Bayer over rungs 3–9 gives 60), so the gradient tool's ordered dither and this rule have to be reconciled before that tool lands.
- A form needs more than one shade glyph.

`cli shade` runs one pass over a box and prints the check; it exits 0 with findings (a `lint` command comes in Phase 4). `--ramp` takes a name or palette indices.

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
- Every merge to main installs a dev build on the dev machine (`blockwright-dev`, `blockwright-cli-dev` and a "Blockwright (dev)" launcher entry).
- Bundled font: IBM VGA 8×16 from VileR's Ultimate Oldschool PC Font Pack (CC BY-SA 4.0, compatible one way with GPLv3), in `third_party/oldschool-pc-fonts/` and credited in `NOTICE`. `LICENSE`, `NOTICE` and the font's licence and README install to `<prefix>/share/doc/blockwright/`, and `blockwright --version`, `blockwright-cli --version` and the GUI's About box credit the font.
