# Packaging

- macOS: `Blockwright.app`, a disk image and a Homebrew cask, built on a Mac
  with `scripts/package-macos` (below).
- Windows: an MSI cross-built on Linux with `scripts/package-windows`
  ([windows/README.md](windows/README.md)).
- Linux: a source tarball made by `scripts/release`, and the distribution
  packages built from it (below): Arch (AUR), Debian and Ubuntu, Fedora, an
  AppImage and a Flatpak bundle.
- FreeBSD: a port built from the same tarball (below).

PLAN §14 records the decisions behind them, and [RELEASING.md](RELEASING.md)
is the checklist for publishing a version.

## Linux

### The source tarball

`scripts/release (--help-bundle DIR | --no-help-bundle) <version> [<ref>]` makes `dist/blockwright-<version>.tar.gz`
and its `.sha256` with `git archive` of the tag `v<version>` or the given
commit: only tracked, committed files, under `blockwright-<version>/`, the same
bytes from the same commit. git writes the commit into
`packaging/tarball-commit` (`export-subst` in `.gitattributes`), and a build of
the tarball reports it in `--version` instead of asking git, which would answer
for whatever repository the tarball was unpacked inside. The forge's own
archive of a tag gets the same treatment. `--check DIR` builds the tarball and
runs its tests without network.

### The help bundle

The tutorials and the manual the help window shows offline (#210, PLAN §11)
ship in the tarball as `help/`, which isn't tracked: `scripts/release
--help-bundle DIR` appends the bundle in DIR, its files sorted, owned by 0,
mode 644 and dated with the commit, so the same commit and bundle give the
same tarball. It is built by `scripts/help-bundle` from the blockwright-docs
commit `packaging/docs-commit` pins, with the release's own programs:

```sh
cmake -S . -B ~/.cache/dw/bw-release-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build ~/.cache/dw/bw-release-build
scripts/help-bundle --cli ~/.cache/dw/bw-release-build/cli/blockwright-cli \
  --app ~/.cache/dw/bw-release-build/app/blockwright ~/.cache/dw/bw-help-<version>
```

It needs a blockwright-docs checkout (beside this repository, or `--docs
DIR`), Python 3 and Python-Markdown. `scripts/release` refuses a bundle whose
`bundle.json` doesn't name the pinned docs commit or wasn't checked with this
release (`checkedWith` `<version> (<commit>)`, which a CLI built from the tag
gives), so the docs pin is moved before the tag. `--no-help-bundle` makes a
tarball without one, whose builds' Help opens the website.

The CMake cache path `BW_HELP_BUNDLE` installs a bundle as
`<datadir>/blockwright/help/` (`Blockwright.app/Contents/Resources/blockwright/help/`
on macOS, `share\blockwright\help\` on Windows); configure refuses a
directory without a `bundle.json` of format 1 with contents. It defaults to
`help/` in the source tree, so a build of the tarball installs it with nothing
added to its recipe: the Debian, Fedora and AUR packages (whose file lists
already take `share/blockwright/`), the FreeBSD port (`PORTDATA` lists
`help`) and an AppImage built from the tarball. `scripts/package-flatpak`,
`package-macos` and `package-windows` build from a checkout and take
`--help-bundle DIR` (the Flatpak copies it into its exported tree as `help/`)
or `--no-help-bundle`; without either and without a `help/` in the tree they
stop. `blockwright-git` builds `main`, which has no bundle, so its Help opens
the website. `ctest` always runs `help_bundle_check`, on the bundle a build installs, or
without one (CI) on the stand-in in `tests/data/help-bundle`:
`helpdocs::check` on the bundle (the three Help pages, every tool's section,
every link and picture) and the three pages opened in the help window.

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
- `icons/hicolor/{16x16,32x32,48x48,128x128,256x256,512x512}/apps/blockwright.png`,
  from `packaging/linux/blockwright-<size>.png`;
- `metainfo/online.pumpin.funkpower.blockwright.metainfo.xml`, the AppStream
  metadata software centres read;
- `man/man1/blockwright.1` and `man/man1/blockwright-cli.1`, the manual pages,
  from `packaging/linux/`. `blockwright-cli.1` lists the commands and leaves
  the options to `blockwright-cli --help`.

The icons are rendered from the art in `branding/` by `branding/render`
(see `branding/README.md`), which writes these PNGs,
`packaging/macos/icon-1024.png` and `packaging/windows/blockwright.ico`.
Redraw the art and rerun it rather than editing the PNGs.

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
in the Noto Sans in `third_party/noto-sans`, registered under a family name of its
own, so a Noto Sans the build root has installed can't take its place. The build
profile `nocheck` (`DEB_BUILD_PROFILES=nocheck DEB_BUILD_OPTIONS=nocheck`, and
`apt-get build-dep -o APT::Build-Profiles=nocheck ./`) builds without the
tests and without Catch2.

The renderer's GNU Unifont comes from the `unifont` package: `debian/rules`
passes `-DBLOCKWRIGHT_SYSTEM_UNIFONT=/usr/share/unifont`, and the build makes
the compiled-in table from its `unifont.hex` and `unifont_all.hex` with
`scripts/gen-unifont` (so `python3` and `unifont` are build dependencies, the
tests or not). Those file names carry no release, so the package's
`--version` and About box credit GNU Unifont without one. The rpm and AUR
packages compile in `third_party/unifont/`: Fedora ships only the BMP
`unifont.hex`, without the upper planes (`unifont_upper` or `unifont_all`).

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
Fedora (Fedora 45: Qt 6.11, Catch2 3.16):

```sh
rpmdev-setuptree
cp blockwright-<version>.tar.gz ~/rpmbuild/SOURCES/
sudo dnf builddep packaging/rpm/blockwright.spec
rpmbuild -ba packaging/rpm/blockwright.spec
rpmlint packaging/rpm/blockwright.spec ~/rpmbuild/SRPMS/*.src.rpm ~/rpmbuild/RPMS/*/*.rpm
```

`%check` validates the desktop entry and AppStream metadata and runs the tests
(`--without check` skips the tests). `LICENSE`, `NOTICE` and the bundled
fonts' and icons' licences (`LICENSE.oldschool-pc-fonts`,
`LICENSE.unifont-OFL`, `LICENSE.unifont-GPL`, `LICENSE.tabler-icons`,
`LICENSE.noto-sans`) are `%license` files in
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

Running the AppImage needs FUSE: a `fusermount3` or `fusermount` command on
the `PATH`, from the `fuse3` package or the older FUSE 2 package. The
AppImage's runtime is static and loads no `libfuse`, so `libfuse2` alone is
not enough. Without one, set `APPIMAGE_EXTRACT_AND_RUN=1` or run it with
`--appimage-extract-and-run`.
Where AppImageLauncher is installed, its binfmt handler runs every AppImage
under the AppImage's own path, so a `blockwright-cli` symlink starts the
editor; use `--cli` there.
The script finishes with `tests/appimage_test.sh`, which runs the AppImage
offscreen and, under a headless weston (hence `weston` above), on Wayland,
checks with `ldd` that nothing in it loads Qt or Qt's libraries from the
system, and checks that every bundled library has its notice; with
`-DBW_APPIMAGE=<file>`, `ctest` runs it as the `appimage` test, which skips
the Wayland run where weston isn't installed. `appimage_apprun` tests `packaging/linux/AppRun`.

### Flatpak

`packaging/flatpak/online.pumpin.funkpower.blockwright.yml` builds
Blockwright against the KDE runtime 6.11 (Qt 6.11) from Flathub, with zstd
(static, for its CMake config, which the runtime's zstd lacks) and libzip as
modules pinned by sha256, and Blockwright from the tree two levels up,
without the tests. `scripts/package-flatpak [--ref REF] [--out DIR] [--check]`
exports the commit with `git archive`, as `scripts/release` does, so
`--version` reports it, builds it with `flatpak-builder` (which installs the
runtime and SDK from Flathub into the user installation when they are
missing) and writes `dist/blockwright-<version>-x86_64.flatpak` and its
`.sha256`. It needs `flatpak` and `flatpak-builder`. To keep the build and
the check out of your own installation, point `FLATPAK_USER_DIR` at a
directory of its own:

    FLATPAK_USER_DIR=~/.cache/dw/flatpak scripts/package-flatpak --check

`--check` installs the bundle and runs `tests/flatpak_test.sh`: both
programs' `--version`, `blockwright-cli` info, render and library list, the
editor offscreen, the Library, samples and notices, the exported desktop file
and icon, valid AppStream metadata and the permissions. The bundle is
installed with `flatpak install --user blockwright-<version>-x86_64.flatpak`,
which fetches the runtime from Flathub, and runs as
`flatpak run online.pumpin.funkpower.blockwright`; the CLI is
`flatpak run --command=blockwright-cli online.pumpin.funkpower.blockwright`.

The sandbox gets Wayland (X11 only without it), IPC, DRI and the home
directory. The home directory, not the file chooser portal: a portal grants
only the file picked, and pieces, projects, reference images, generator
scripts and imported scenes are paths beside each other in the home
directory, as are the CLI's arguments. Files outside it (another disk under
`/mnt`, say) need `flatpak override --user --filesystem=<path>
online.pumpin.funkpower.blockwright`. The user Library and the settings are
in `~/.var/app/online.pumpin.funkpower.blockwright/`, apart from a native
install's. Generators and imported scenes run inside the sandbox, with the
runtime's Python 3: modules installed for the system's Python (ansidraw
among them) are not there unless they are in the home directory and on
`PYTHONPATH` (`flatpak override --user --env=PYTHONPATH=...`).

To build a release rather than a commit, replace the manifest's `dir` source
with the release tarball and its sha256, as the comment there shows. The
static check `flatpak_manifest` (and `flatpak_manifest_self_test`, which
breaks copies of the manifest) runs in `ctest` without network. The app is
not on Flathub.

## FreeBSD

`packaging/freebsd/` is a port of the release tarball: `Makefile`,
`pkg-descr`, `pkg-plist` and `distinfo`. It builds with the system's clang,
packaged Qt 6 and libzip, and zstd (linked statically, so a build dependency
only), and `make test` runs the suite offscreen with `BW_FETCH_CATCH2=OFF`
against `devel/catch2`; `x11-fonts/noto-basic` is a test dependency for the
same reason as `noto-fonts` on Arch. The install block that
serves Linux (desktop entry, metainfo, icons, man pages) applies unchanged,
and the man pages land in `share/man`. `LICENSE` names the program's GPL
and the bundled fonts' and icons' licences (CC BY-SA 4.0, MIT, OFL 1.1 for
Unifont and Noto Sans), and `pkg-plist` lists their notices under `%%DOCSDIR%%`; the
`install_notices_freebsd_plist` test fails when the install's notices and
the plist part ways. The `distinfo` in the tree is a
template: `scripts/release` writes the port for its tarball, with
`DISTVERSION` and the `distinfo` (the commit's time, the tarball's SHA-256 and
size) filled in, to `dist/freebsd/blockwright/`. `MAINTAINER` is left unset,
so the ports collection's default stands, until the port is submitted. To try
it, copy that directory to `graphics/blockwright` in a ports tree, put the
tarball in `DISTDIR`, and run `make stage check-plist`, `make test` and
`portlint -AC`.

The CI's `freebsd` job builds and tests on FreeBSD 14 with the same
packages. It takes up to two hours, so it runs only as a release check: on a
push of a `v*` tag, or when the workflow is run by hand on a branch.

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

The oldest macOS the app supports is one value, `CMAKE_OSX_DEPLOYMENT_TARGET`
in `CMakeLists.txt` (12.0; `MACOSX_DEPLOYMENT_TARGET` overrides it for one
build). It is what the app, the CLI, libzip and zstd are built for, the app's
`LSMinimumSystemVersion` and the cask's `depends_on macos`. Before building
anything the script reads the minimum macOS QtCore is built for (its
`LC_BUILD_VERSION` `minos`) and refuses a Qt that needs a newer one than the
target, naming both: Qt 6.10 and later need macOS 13, so build with 6.8.3.

It builds a Release bundle, runs `macdeployqt` (the CLI too), keeps only the
app's architecture of Qt's universal libraries, removes every absolute
search path from the app and the CLI, and fails if anything in the bundle
loads a library from outside it but the system's (`/usr/lib`, `/System`) or
still has an absolute search path, or if anything in it is built for a newer
macOS than the target or its `LSMinimumSystemVersion` isn't the target, adds
the notices for what it bundled (below), signs it ad hoc, inside out, and makes
`dist/Blockwright-<version>.dmg` with dmgbuild (in a venv in the work
directory): the app beside an Applications link over a background that says
to drag it. Beside the dmg go its `.sha256` and `blockwright.rb`, the
Homebrew cask for exactly that file. `--help` lists the options;
`CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` makes a universal app (Qt's build is
universal; `macos-deps` builds both slices).

The notices go in `Contents/Resources/doc`, beside the install's `LICENSE`,
`NOTICE` and the font's and icons' notices, and before signing, so the
signature covers them (`packaging/macos/third-party`): `THIRD-PARTY.txt` (from
`packaging/macos/`, with the bundled Qt's version, source link, frameworks and
plugin directories filled in), the LGPL v3 that Qt is used under (the Windows
package's copy), libzip's and zstd's licences (`scripts/macos-deps` keeps them
in `<prefix>/licenses`), and in `qt-sbom/` the SBOM of each Qt module in the
app. Packaging fails if the app carries a Qt framework or plugin the script
can't place in a module (add it to the script's list), or a version, SBOM or
licence is missing.

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

`packaging/macos/icon-1024.png` is the app icon rendered at four times its
size by `branding/render`. `dmg-background.png` and `dmg-background@2x.png`
are drawn by `packaging/macos/make-art.py` (needs Pillow): a caption set in
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
   `brew install --cask funkpower/tap/blockwright`, `brew audit --cask
   --strict funkpower/tap/blockwright` and `brew uninstall --cask --zap
   blockwright`.

The cask installs the app and links `blockwright-cli` onto the PATH; its
caveat gives the `xattr` command that lets both run.

Install by the cask's full name, `funkpower/tap/blockwright`, here and in
RELEASING.md § 8. Homebrew 7 refuses to load a cask from a tap it does not
trust: `brew install --cask blockwright` and `brew audit` stop with
"Refusing to load cask funkpower/tap/blockwright from untrusted tap". Naming
the cask in full trusts it (`brew trust --cask funkpower/tap/blockwright`
does the same), and the short name works after that. Uninstalling removes
the trust again.
