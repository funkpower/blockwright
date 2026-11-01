# Packaging

- macOS: `Blockwright.app`, a disk image and a Homebrew cask, built on a Mac
  with `scripts/package-macos` (below).
- Windows: an MSI cross-built on Linux with `scripts/package-windows`
  ([windows/README.md](windows/README.md)).
- Linux: a source tarball made by `scripts/release`, and the distribution
  packages built from it (below): Arch (AUR), Debian and Ubuntu, Fedora, and
  an AppImage.

PLAN §14 records the decisions behind them, and [RELEASING.md](RELEASING.md)
is the checklist for publishing a version.

## Linux

### The source tarball

`scripts/release <version> [<ref>]` makes `dist/blockwright-<version>.tar.gz`
and its `.sha256` with `git archive` of the tag `v<version>` or the given
commit: only tracked, committed files, under `blockwright-<version>/`, the same
bytes from the same commit. git writes the commit into
`packaging/tarball-commit` (`export-subst` in `.gitattributes`), and a build of
the tarball reports it in `--version` instead of asking git, which would answer
for whatever repository the tarball was unpacked inside. The forge's own
archive of a tag gets the same treatment. `--check DIR` builds the tarball and
runs its tests without network.

### Building without network

Distribution builders have no network. Configure with

- `-DBW_FETCH_CATCH2=OFF`: the tests use the installed Catch2 3, and the
  configure stops with a message if there is none, rather than downloading it
  (the default, ON, downloads a pinned Catch2 when none is installed);
- or `-DBUILD_TESTING=OFF`: no tests, and no Catch2 needed.

Nothing else is downloaded at build time. The `catch2_offline` test checks
both switches.

### Desktop files

A Linux install also puts in `<prefix>/share/`:

- `applications/blockwright.desktop`, the launcher entry (the GUI sets its
  desktop file name from the name it runs as, so the entry matches it);
- `icons/hicolor/{256x256,512x512}/apps/blockwright.png`, from
  `packaging/linux/blockwright-{256,512}.png`;
- `metainfo/online.pumpin.funkpower.blockwright.metainfo.xml`, the AppStream
  metadata software centres read;
- `man/man1/blockwright.1` and `man/man1/blockwright-cli.1`, the manual pages,
  from `packaging/linux/`. `blockwright-cli.1` lists the commands and leaves
  the options to `blockwright-cli --help`.

The icons are `packaging/macos/icon-1024.png` scaled down; after redrawing it
(below), remake them with ImageMagick:

```sh
for s in 256 512; do
  magick packaging/macos/icon-1024.png -scale $((s * 100 / 1024))% -strip \
    -define png:exclude-chunks=date,time packaging/linux/blockwright-$s.png
done
```

`linux_desktop` installs a build and checks these files, with
`desktop-file-validate` and `appstreamcli validate` where they are installed.

### Arch Linux (AUR)

`packaging/aur/blockwright/` builds a release tarball and
`packaging/aur/blockwright-git/` the newest commit of `main`, each with its
`.SRCINFO`. Both build with `BW_FETCH_CATCH2=OFF` against Arch's `catch2` and
run the tests in `check()` with `QT_QPA_PLATFORM=offscreen`; `noto-fonts` is
a check dependency because the layout tests measure text and a clean chroot has
no font. `package()` copies the notices and the font's and icons' licences into
`/usr/share/licenses/<pkgname>/` rather than linking them into
`/usr/share/doc/`, which pacman's `NoExtract` may drop. The release
PKGBUILD in the tree is a template: `scripts/release` writes the one for its
tarball, with the version and checksum filled in, to
`dist/aur/blockwright/`. After changing either PKGBUILD, regenerate its
`.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO` and check both with
`namcap`.

### Debian and Ubuntu

`packaging/debian/` is the `debian/` directory of a package of the release
tarball: debhelper compat 13, `dh` with CMake and Ninja, source format
`3.0 (quilt)` with the tarball as its orig tarball. To build it:

```sh
cp blockwright-<version>.tar.gz blockwright_<version>.orig.tar.gz
tar xf blockwright_<version>.orig.tar.gz
cd blockwright-<version>
cp -r packaging/debian debian
sudo apt-get build-dep ./
dpkg-buildpackage -us -uc
lintian -I --pedantic ../blockwright_<version>-1_*.changes
```

`dh_auto_test` runs the tests with `QT_QPA_PLATFORM=offscreen` against the
packaged Catch2 3 (`BW_FETCH_CATCH2=OFF`). The layout tests measure the docks
in the Noto Sans in `tests/data/fonts`, registered under a family name of its
own, so a Noto Sans the build root has installed can't take its place. The build
profile `nocheck` (`DEB_BUILD_PROFILES=nocheck DEB_BUILD_OPTIONS=nocheck`, and
`apt-get build-dep -o APT::Build-Profiles=nocheck ./`) builds without the
tests and without Catch2.

The package needs Qt 6.7 or newer, the minimum `CMakeLists.txt` states (#263),
so it is built only for releases that have it. Debian 12 and Ubuntu 24.04 ship
Qt 6.4.2: they are not packaged natively; use the AppImage, which bundles its
own Qt.

| Release | Qt | Catch2 | Package |
|---|---|---|---|
| Debian 13 (trixie) | 6.8.2 | 3.7.1 | built, tests run |
| Debian testing | 6.11 | 3.7 | built, tests run |
| Ubuntu 26.04 | 6.10 | 3.7 | built, tests run |
| Ubuntu 26.10 | 6.11 | 3.7 | built, tests run |
| Debian 12 | 6.4.2 | 2.13 | not packaged natively; use the AppImage |
| Ubuntu 24.04 | 6.4.2 | 3.4 | not packaged natively; use the AppImage |

Catch2 before 3.8 reads its test list as a CMake list, which an unmatched `[`
in a test name corrupts; the `catch_names` test keeps every name's brackets
matched.

### Fedora

`packaging/rpm/blockwright.spec` builds the release tarball against the newest
Fedora (Fedora 44: Qt 6.11, Catch2 3.13):

```sh
rpmdev-setuptree
cp blockwright-<version>.tar.gz ~/rpmbuild/SOURCES/
sudo dnf builddep packaging/rpm/blockwright.spec
rpmbuild -ba packaging/rpm/blockwright.spec
rpmlint packaging/rpm/blockwright.spec ~/rpmbuild/SRPMS/*.src.rpm ~/rpmbuild/RPMS/*/*.rpm
```

`%check` validates the desktop entry and AppStream metadata and runs the tests
(`--without check` skips the tests). `LICENSE`, `NOTICE` and the bundled
font's and icons' licences (`LICENSE.oldschool-pc-fonts`,
`LICENSE.tabler-icons`) are `%license` files in
`/usr/share/licenses/blockwright/`, which an install with `tsflags=nodocs`
keeps. The same notices are also in `/usr/share/doc/blockwright/`, where
`NOTICE` and `--version` say they are; its `LICENSE` links to the `%license`
copy.

`linux_packages` checks that `debian/changelog`'s newest entry, the spec's
`Version` and the manual pages are the tree's version, and that
`blockwright-cli.1` describes every command `blockwright-cli --help` lists.

### AppImage

`scripts/package-appimage` builds `dist/blockwright-<version>-x86_64.AppImage`
and its `.sha256`: a Release build installed into an AppDir, with Qt, its
plugins (X11, Wayland and offscreen platforms, Wayland's shell-integration,
decoration and graphics-integration plugins, SVG, image formats) and every
other library the programs load copied in by
[linuxdeploy](https://github.com/linuxdeploy/linuxdeploy) and its Qt plugin.
The notices for what it bundles are in `usr/share/doc/blockwright/`:
`THIRD-PARTY.txt` (from `packaging/linux/`), the LGPL v3 that Qt is used under
(the Windows package's copy), ICU 73's licence, Qt's SBOMs, and
`bundled-libraries.txt`, which names each library in `usr/lib`, its origin
(Qt, ICU or a Debian package with its version) and its notice; every Debian
package's `copyright` file is copied to `usr/share/doc/<package>/`.
Only the libraries each system has to provide itself come from the host:
glibc, the C++ runtime, OpenGL, X11 and XCB, fontconfig, FreeType and the like
(on a minimal Debian 12 or Ubuntu 24.04: `libgl1`, `libegl1`, `libfontconfig1`,
`libx11-xcb1` and `libwayland-client0`).
The script downloads linuxdeploy and its Qt and AppImage plugins, tagged
releases pinned by sha256, to `~/.cache/dw/toolchains/linuxdeploy/`
(`--tools`).

An AppImage runs where glibc is at least as new as the build system's, so the
release is built on the oldest supported release, Debian 12 (glibc 2.36). Debian
12's Qt, 6.4.2, is older than the Qt 6.7 minimum (#263), so the AppImage bundles
Qt's own 6.8.3 build (6.8 is a long-term-support release; it needs glibc 2.28)
with `qtimageformats`, installed with aqtinstall:

```sh
python3 -m venv ~/.cache/dw/toolchains/aqt-venv
~/.cache/dw/toolchains/aqt-venv/bin/pip install aqtinstall
cd ~/.cache/dw/toolchains
aqt-venv/bin/aqt install-qt -O qt-appimage linux desktop 6.8.3 linux_gcc_64 -m qtimageformats
# Qt is then in ~/.cache/dw/toolchains/qt-appimage/6.8.3/gcc_64
```

Then, from the release tarball (its `packaging/tarball-commit` gives
`--version` the commit without git), in a Debian 12 container with the
libraries Qt's plugins load, which linuxdeploy copies in or leaves to the
system:

```sh
tar xzf blockwright-<version>.tar.gz && cd blockwright-<version>
docker run --rm --cpus 8 -v "$PWD":/src:ro -v ~/.cache/dw/toolchains/linuxdeploy:/tools \
  -v ~/.cache/dw/toolchains/qt-appimage/6.8.3/gcc_64:/qt:ro -v "$PWD/dist":/out debian:12 bash -c '
  apt-get update && apt-get install -y --no-install-recommends build-essential cmake \
    ninja-build libgl-dev libegl-dev libfontconfig-dev libfreetype-dev libxkbcommon-dev \
    libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
    libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-sync1 libxcb-xfixes0 libxcb-xkb1 \
    libx11-xcb1 libwayland-client0 libwayland-cursor0 libwayland-egl1 libdbus-1-3 \
    libglib2.0-0 libzip-dev zipcmp zipmerge ziptool libzstd-dev curl file ca-certificates \
    weston &&
  useradd -m bw && mkdir /w && chown bw /w /out /tools &&
  su bw -c "/src/scripts/package-appimage --qmake /qt/bin/qmake --tools /tools --work /w --out /out"'
```

The AppImage runs the editor. Run under a name starting `blockwright-cli` (a
symlink to it) or with `--cli` first, it runs `blockwright-cli`:

```sh
ln -s blockwright-<version>-x86_64.AppImage ~/.local/bin/blockwright-cli
blockwright-cli render piece.ans piece.png
./blockwright-<version>-x86_64.AppImage --cli lint piece.ans
```

Running an AppImage needs FUSE 2 (`libfuse2`); without it, set
`APPIMAGE_EXTRACT_AND_RUN=1` or run it with `--appimage-extract-and-run`.
Where AppImageLauncher is installed, its binfmt handler runs every AppImage
under the AppImage's own path, so a `blockwright-cli` symlink starts the
editor; use `--cli` there.
The script finishes with `tests/appimage_test.sh`, which runs the AppImage
offscreen and, under a headless weston (hence `weston` above), on Wayland,
checks with `ldd` that nothing in it loads Qt or Qt's libraries from the
system, and checks that every bundled library has its notice; with
`-DBW_APPIMAGE=<file>`, `ctest` runs it as the `appimage` test, which skips
the Wayland run where weston isn't installed. `appimage_apprun` tests `packaging/linux/AppRun`.

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
