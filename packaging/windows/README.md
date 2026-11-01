# Windows build and MSI

Blockwright for 64-bit Windows is cross-built on Linux with llvm-mingw and
packaged as an MSI with msitools' `wixl` (PLAN §14). Nothing here needs root;
the toolchain can live anywhere, `~/.cache/dw/toolchains/` on the dev machine.

## Toolchain

- **llvm-mingw** (UCRT, x86_64 Linux host): unpack a release, e.g.
  `llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64.tar.xz` from
  <https://github.com/mstorsjo/llvm-mingw/releases>.
- **Qt 6.8.3 for Windows and for Linux**, the same version: the Windows one
  links the programs, the Linux one runs moc, rcc and uic. With aqtinstall
  in a venv:

  ```sh
  python3 -m venv aqt-venv && aqt-venv/bin/pip install aqtinstall
  aqt-venv/bin/aqt install-qt -O qt windows desktop 6.8.3 win64_llvm_mingw -m qtimageformats qtpdf
  aqt-venv/bin/aqt install-qt -O qt linux desktop 6.8.3 linux_gcc_64
  ```

  This gives `qt/6.8.3/llvm-mingw_64` and `qt/6.8.3/gcc_64`. Qt SVG is part of
  the base download; Qt PDF's image plugin opens PDF reference images, and
  `scripts/package-windows` refuses a Qt without it.
- **wixl** from msitools 0.100 or later: the distribution's `msitools`
  package, or that package unpacked into a directory with `LD_LIBRARY_PATH`
  pointing at its `usr/lib` and `--extdir` at its `usr/share/wixl-*/ext`.
- **wine** (optional) runs the tests and the installer.

zlib, zstd and libzip are built from source by `build-deps`, which
`scripts/package-windows` runs; it downloads them and checks their SHA-256.

## The MSI

```sh
LLVM_MINGW=…/llvm-mingw QT_WIN=…/qt/6.8.3/llvm-mingw_64 QT_HOST=…/qt/6.8.3/gcc_64 \
WIXL=…/wixl scripts/package-windows
```

or the same as `--llvm-mingw`, `--qt`, `--qt-host` and `--wixl`. It builds
Release without the tests in `--build-dir` (default
`~/.cache/dw/blockwright-win-build`), stages the install with the Qt DLLs and
plugins, the llvm-mingw runtime and the licences (`stage.py`), and writes
`dist/blockwright-<version>-x86_64.msi`. `THIRD-PARTY.txt` names Qt as
`@QT_VERSION@` and links its source as
`https://download.qt.io/archive/qt/@QT_MINOR@/@QT_VERSION@/submodules/`;
`third-party` fills in the version of the Qt in `--qt` (from its
`Qt6ConfigVersion*.cmake`) when it stages the file, and fails if a
placeholder is left.

The MSI installs per machine into `Program Files\Blockwright` with a Start
menu shortcut. Its UpgradeCode in `blockwright.wxs` never changes and each
build gets a new ProductCode, so installing one removes the Blockwright an
earlier MSI installed, of an older version or of the same one (the
MajorUpgrade allows same-version upgrades, so two builds of 0.1.0 leave one
entry in Add or Remove Programs). An older MSI over a newer install is
refused. The component GUIDs `stage.py` derives from each file's path keep
each file's identity from one build to the next.

## Tests

A build with the toolchain file and the tests runs them under wine, which
CMake finds on its own. `<build-deps prefix>` is where
`packaging/windows/build-deps` installed zlib, zstd and libzip:
`<build-dir>/deps` after a `scripts/package-windows` run.

```sh
cmake -S . -B ~/.cache/dw/bw-win-build/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-x86_64.cmake -DLLVM_MINGW=…/llvm-mingw \
  -DCMAKE_PREFIX_PATH="…/qt/6.8.3/llvm-mingw_64;<build-deps prefix>" -DQT_HOST_PATH=…/qt/6.8.3/gcc_64 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
nice -n 10 cmake --build ~/.cache/dw/bw-win-build/build -j 8
cd ~/.cache/dw/bw-win-build/build && QT_QPA_PLATFORM=offscreen ctest -j 8
```

The build directory's `wine-run` runs each program with its own wine prefix
(`BW_WINEPREFIX` names another). It starts the wine session itself, with
none of the caller's descriptors and under a lock (`flock`): a `wineserver`
that stays up 60 seconds after its last program exits, then wine's services.
A program that started the session would start the services, and they would
hold its output open, and ctest with it, until the session ended. Each test
under wine has 300 seconds unless it sets its own `TIMEOUT` (the `cli`
script has 900), so a program that hangs fails its test instead of holding
up the run; `-DDART_TESTING_TIMEOUT=<seconds>` sets another default, and
`ctest --timeout <seconds>` overrides it for one run. `wineserver -k` with
`WINEPREFIX` set to the prefix stops the session at once; otherwise its
server and services are still running for the 60 seconds after the last
program. A server already up for the prefix gets the services started too
when it has none, as one started by hand with `wineserver -p` has, or they
would hold the next program's output until that server was killed (#273).

ctest runs each Catch2 test case as its own program. `blockwright-tests.exe`
run whole in one process takes about as long as on Linux, over ten minutes
on the dev machine, so a `timeout` round it needs more than 300 seconds.
A test spec of `"*"` takes in the hidden measurement tests (`[.measure]`,
`[.timing]`, `[.survey]`, `[.sheets]` and others) too, a few more minutes;
`[.sheets]` skips unless `BW_PRESET_SHEETS` names a directory to write to.

The tests skip what a Windows program can't do (POSIX permissions, sh
generators, Python scenes when the prefix has no Python, and the Send to
WeeChat tests whose remote command needs a POSIX sh). The symbolic-link
tests run, since wine makes links without Developer Mode; their scratch
folder is on `Z:` there, as wine reads a link named by a path that starts
with `/` as what it points at.
`test_generator_win.cpp` runs generators the Windows way,
`test_junction_win.cpp` checks junctions are refused as symbolic links are,
and `test_scene_import.cpp` finds a scene's Python (`py -3`, `python`,
`python3`) among stand-ins built from `bw-gen-helper.exe`, as
`test_app_publish.cpp` builds its ssh. Qt's offscreen
platform finds no font under wine, so the configure step
copies one sans family from `/usr/share/fonts` (Noto Sans, else DejaVu Sans;
`-DBW_WINE_FONTS=<files>` names others) into the build's `wine-fonts` and
`wine-run` points `QT_QPA_FONTDIR` there. Wine's FreeType hints them a
pixel or two apart from Linux's; the Tool Options layout tests draw with the
Noto Sans in `tests/data/fonts` and fit under wine too.
