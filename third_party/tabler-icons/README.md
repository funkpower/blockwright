# Tabler Icons

- **Icons:** eleven outline icons from Tabler Icons, used for the generic tools in the Toolbox and the Tools menu (`app/tool_icon.cpp`):

  | Tool | Icon |
  |---|---|
  | Keyboard | `keyboard.svg` |
  | Select | `marquee-2.svg` |
  | Move/Transform | `arrows-move.svg` |
  | Line | `line.svg` |
  | Rectangle | `square.svg` |
  | Ellipse | `oval.svg` |
  | Fill | `bucket-droplet.svg` |
  | Eyedropper | `color-picker.svg` |
  | Text | `typography.svg` |
  | Clone stamp | `rubber-stamp.svg` |
  | Glyph spray | `spray.svg` |

- **Source:** Tabler Icons v3.50.0, <https://tabler.io/icons>, <https://github.com/tabler/tabler-icons>; downloaded as the npm package `@tabler/icons` 3.50.0 (`https://registry.npmjs.org/@tabler/icons/-/icons-3.50.0.tgz`, SHA-256 `4c2eb678ed71cdcbf57a3e899c4e91f3445d680fbc1d57d27c7083a321c47303`), files `package/icons/outline/<name>.svg` and `package/LICENSE`.
- **Licence:** MIT; the copyright notice and the full text are in `LICENSE`, as shipped in the package.
- **Changes:** none. The files in `icons/` are byte for byte the package's. The program recolours them when it draws them, putting the palette's button text colour in place of `currentColor`.
- **SHA-256 of the files in `icons/`:**
  - `arrows-move.svg` `45b5a5c95a430f7512b9b20d48a3fed70571f4cb31069da74000c94550c7ec4c`
  - `bucket-droplet.svg` `1c8e71f5249b33c62aeb723d389489d4791efa0ed4fca14e8c2c9d2c6ae3dbaa`
  - `color-picker.svg` `854dc49964f90582d7e2fbe6a9853528eef51652c056c766c8e6d3eaecc4f799`
  - `keyboard.svg` `2cc2bdc28c8bd4e20b136581491b423ae894886f5e57d378644bd90e1d9da453`
  - `line.svg` `6fb926e644c2826e734d805a00ed86dc2eefe0d6fd309b732fb6ec011982cfc8`
  - `marquee-2.svg` `9047140847319a685648f3ffe8df0cbdabebb8ce010d48f50026dce3dc895367`
  - `oval.svg` `42a48ff66c7db37a52cd52a1cdaf9f95f1a0e4feb6f3fadce18d305994db5b1d`
  - `rubber-stamp.svg` `906444358b6427f9d7edd09c8efbbef5db9e44ff50aa4b784853b6b028c14aea`
  - `spray.svg` `1d81befac8dd674ccae350c3647498490a2d83f63418dbb512f7202d40e25346`
  - `square.svg` `5b470dfa3b1fd4896af6c23fccb18c07fde0245078003abc31bc2593f1933851`
  - `typography.svg` `ccf0ac7285d7f0e5bddd9a740752f5702365563cdb6146f4f263389ff80bef5e`

The ANSI-specific tools' icons (half-block, shade, glyph, colorize, the ramp brushes, the detail brushes and the ramp gradient) are not from the set: they are drawn in code on the same 24-unit grid and 2-unit stroke.

The MIT licence asks for the copyright notice and permission notice in copies of the icons: `cmake --install` puts this README and `LICENSE` in `<prefix>/share/doc/blockwright/tabler-icons/`, the top-level `NOTICE` names them, and `blockwright --version` and the About box credit the set.
