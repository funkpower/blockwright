# Packaging

- macOS: `Blockwright.app`, a disk image and a Homebrew cask, built on a Mac
  with `scripts/package-macos` (below).
- Windows: an MSI cross-built on Linux with `scripts/package-windows`
  ([windows/README.md](windows/README.md)).

PLAN §14 records the decisions behind both.

## macOS: Blockwright.app, the disk image and the Homebrew cask

The Mac build is a `Blockwright.app` bundle holding the editor
(`Contents/MacOS/Blockwright`), the CLI beside it
(`Contents/MacOS/blockwright-cli`), the bundled Library, licences and notices
in `Contents/Resources`, and Qt in `Contents/Frameworks` and
`Contents/PlugIns`. People install it by dragging it from the disk image to
Applications, or with Homebrew (below).

### What a Mac needs

- Xcode's command line tools (`xcode-select --install`), CMake and Ninja
  (Homebrew's are fine), Python 3.
- Qt 6.8 for macOS with `qtimageformats` (WebP, TIFF and more for reference
  images). Homebrew's Qt is built for the Mac it is on and its newest macOS, so
  use Qt's own build, installed with aqtinstall into a directory of your
  choosing:

  ```sh
  python3 -m venv ~/.cache/dw/toolchains/aqt-venv
  ~/.cache/dw/toolchains/aqt-venv/bin/pip install aqtinstall
  cd ~/.cache/dw/toolchains
  aqt-venv/bin/aqt install-qt mac desktop 6.8.3 -m qtimageformats -O Qt
  # Qt is then in ~/.cache/dw/toolchains/Qt/6.8.3/macos (arm64 and x86_64)
  ```

- Static libzip and zstd built for the same oldest macOS, which
  `scripts/macos-deps <prefix>` builds from pinned, checksummed tarballs
  (`scripts/package-macos` runs it for you).

### A development build

```sh
scripts/macos-deps ~/.cache/dw/toolchains/bw-deps
cmake -S . -B ~/.cache/dw/bw-mac-build -G Ninja \
  -DCMAKE_PREFIX_PATH="$HOME/.cache/dw/toolchains/Qt/6.8.3/macos;$HOME/.cache/dw/toolchains/bw-deps"
nice -n 10 cmake --build ~/.cache/dw/bw-mac-build -j 8
cd ~/.cache/dw/bw-mac-build && QT_QPA_PLATFORM=offscreen ctest -j 8
```

The offscreen tests model a Mac's screen (72 dpi, the 13 pt system font, and
popups that never take focus), so layout tests measure what a Mac shows. Tests
that need something a Mac doesn't have skip and say why: the scene tests
(no `ansidraw`), Qt PDF decoding, xcb, non-UTF-8 file names, Qt's native
settings, and libzip allocation counting.

`-DBLOCKWRIGHT_MACOS_BUNDLE=ON` builds and installs the bundle layout instead
of a Unix prefix; with tests on, `macos_bundle` checks the bundle's
Info.plist and icon, and `gui_version` and `install_notices` check the
notices it carries.

### The disk image

```sh
scripts/package-macos --qt ~/.cache/dw/toolchains/Qt/6.8.3/macos \
  --deps ~/.cache/dw/toolchains/bw-deps
```

It builds a Release bundle, runs `macdeployqt` (the CLI too), keeps only the
app's architecture of Qt's universal libraries, removes every absolute
search path from the app and the CLI, and fails if anything in the bundle
loads a library from outside it but the system's (`/usr/lib`, `/System`) or
still has an absolute search path, signs it ad hoc, inside out, and makes
`dist/Blockwright-<version>.dmg` with dmgbuild (in a venv in the work
directory): the app beside an Applications link over a background that says
to drag it. Beside the dmg go its `.sha256` and `blockwright.rb`, the
Homebrew cask for exactly that file. `--help` lists the options;
`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` makes a universal app (Qt's build is
universal; `macos-deps` builds both slices).

The app is signed ad hoc, not with a Developer ID, and not notarized, so
Gatekeeper blocks the first run of a downloaded copy: of the app, and of
`blockwright-cli` inside it, which the Homebrew cask links onto the PATH.
Before using either, remove the quarantine flag from the whole bundle, which
clears both:

```sh
xattr -dr com.apple.quarantine /Applications/Blockwright.app
```

Until then a quarantined `blockwright-cli` stops at the Gatekeeper prompt, and
over ssh, where nothing can answer it, it never returns. The app alone can
also be allowed from System Settings > Privacy & Security after its first
launch is blocked. With a Developer ID, sign with it and `--options runtime`
instead of `-` and notarize with `notarytool`.

### The icon and the background

`packaging/macos/icon-1024.png`, `dmg-background.png` and
`dmg-background@2x.png` are drawn by `packaging/macos/make-art.py` (needs
Pillow): a "B" of block pixels in the 16-colour palette, and a caption set in
the editor's IBM VGA font. Change the script and rerun it rather than editing
the PNGs:

```sh
python3 -I packaging/macos/make-art.py
```

The build makes `blockwright.icns` from `icon-1024.png` with `make-icns`
(`sips` and `iconutil`).

### Homebrew

The cask goes in our own tap; the official homebrew-cask takes only signed,
notarized apps. Homebrew downloads the dmg and clones the tap without signing
in, so the repository's releases and the tap repository must both be readable
anonymously; if either needs a login, `brew tap` or the download fails with a
404. To publish a version:

1. Tag the release on `main` (`v<version>`, matching `project(VERSION)` in
   `CMakeLists.txt`) and create a release for it on the forge.
2. Run `scripts/package-macos` on a Mac at that tag and upload
   `dist/Blockwright-<version>.dmg` to the release. The cask's `url` is
   `https://git.pumpin.online/funkpower/blockwright/releases/download/v<version>/Blockwright-<version>.dmg`.
3. Copy `dist/blockwright.rb` to `Casks/blockwright.rb` in the tap repository,
   `funkpower/homebrew-tap`, and push it.
4. Check it on a Mac: `brew tap funkpower/tap
   https://git.pumpin.online/funkpower/homebrew-tap.git`, then
   `brew install --cask blockwright`, `brew audit --cask --strict
   funkpower/tap/blockwright` and `brew uninstall --cask --zap blockwright`.

The cask installs the app and links `blockwright-cli` onto the PATH; its
caveat gives the `xattr` command that lets both run.
