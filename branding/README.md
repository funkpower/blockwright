# Branding

The Blockwright logos and the app icon, drawn in Blockwright.

| File | What | Size |
|---|---|---|
| `logo-large.bwp` | the wordmark and the builder's mark: the start screen, the README | at most 80 × 20 cells |
| `logo-small.bwp` | the wordmark: About Blockwright | at most 40 × 6 cells |
| `logo.txt` | the wordmark in 7-bit ASCII: `--help`, `--version`, alt text | at most 72 columns |
| `icon.bwp` | the app icon, 256 px and up | 32 × 16 cells |
| `icon-small.bwp` | the app icon redrawn for 16, 32 and 48 px | 16 × 8 cells |

The projects are the source. Each `.ans` beside one is the project
flattened, signed funkpower in SAUCE; the app builds `logo-large.ans` and
`logo-small.ans` into its resources at `:/branding/`, and `logo.txt` into
both binaries.

After editing a project, rerun the script with a built CLI (it needs
ImageMagick 7 and Pillow):

```sh
branding/render path/to/blockwright-cli
```

It flattens each project to its `.ans` and renders:

- `png/logo-{large,small}.png` and `@2x.png`, the logos at 1× and 2×;
- `packaging/linux/blockwright-{256,512}.png` and
  `packaging/macos/icon-1024.png`, `icon.ans` at 1×, 2× and 4×, and
  `blockwright-128.png`, the 256 halved;
- `packaging/linux/blockwright-16.png`, `icon-small.ans` with each
  half-block pixel made one pixel, and the 32 and 48 from it with each pixel
  repeated;
- `packaging/windows/blockwright.ico`, the 16, 32, 48 and 256.

In every icon the black outside the tile, reached from its corners, is made
transparent, and so would any black inside the tile that touched it.

Never edit the PNGs by hand. The `branding` tests check that the files
match the projects and the renders, and that the logos keep to their sizes,
lint clean with iCE colours on and off, and fit one IRC message a row.
