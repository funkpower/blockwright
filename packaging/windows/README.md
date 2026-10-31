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
`dist/blockwright-<version>-x86_64.msi`.

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
(`BW_WINEPREFIX` names another). The tests skip what a Windows program can't
do (POSIX permissions, sh generators, Python scenes when the prefix has
no Python, and for now the symbolic-link tests, #251);
`test_generator_win.cpp` runs generators the Windows way,
`test_junction_win.cpp` checks junctions are refused as symbolic links are,
and `test_scene_import.cpp` finds a scene's Python (`py -3`, `python`,
`python3`) among stand-ins built from `bw-gen-helper.exe`. Qt's offscreen
platform finds no font under wine, so the configure step
copies one sans family from `/usr/share/fonts` (Noto Sans, else DejaVu Sans;
`-DBW_WINE_FONTS=<files>` names others) into the build's `wine-fonts` and
`wine-run` points `QT_QPA_FONTDIR` there. Wine's FreeType hints them a
pixel or two apart from Linux's; the Tool Options layout tests draw with the
Noto Sans in `tests/data/fonts` and fit under wine too.
