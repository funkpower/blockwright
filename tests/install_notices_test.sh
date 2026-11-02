#!/usr/bin/env bash
# The licence, font and icon notices install with the binary:
#   install_notices_test.sh <build dir> <source dir> <scratch dir> <full bindir> <full docdir> [<config>]
#
# <full bindir> and <full docdir> are the build's CMAKE_INSTALL_FULL_BINDIR and
# CMAKE_INSTALL_FULL_DOCDIR; <config> is the configuration the build was built
# in, passed to `cmake --install --config` (needed for a multi-config
# generator; empty or absent uses cmake's default). Installs the build with DESTDIR=<scratch>/stage,
# so every file lands below the scratch dir even when an install dir is
# absolute, and checks the notices; then runs scripts/install-dev against a
# throwaway HOME with git, tar and the build stubbed out (its install step is
# the real `cmake --install` of this build, staged the same way) to check the
# dev install carries them and refuses an install without them, and that it
# installs blockwright-dev and its launcher entry. BW_NOTICES_DIR names where
# --version says the notices are (default share/doc/blockwright/).
set -uo pipefail
build=$1 src=$2 tmp=$3 bindir=$4 docdir=$5 config=${6:-}
notices_dir=${BW_NOTICES_DIR:-share/doc/blockwright/}
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

notices=(LICENSE NOTICE oldschool-pc-fonts/LICENSE oldschool-pc-fonts/README.md
         tabler-icons/LICENSE tabler-icons/README.md)
sources=(LICENSE NOTICE third_party/oldschool-pc-fonts/LICENSE third_party/oldschool-pc-fonts/README.md
         third_party/tabler-icons/LICENSE third_party/tabler-icons/README.md)

# has_notices <name> <dir>: the notices are in <dir>, unchanged
has_notices() {
  local i f
  for i in "${!notices[@]}"; do
    f=$2/${notices[$i]}
    if [[ ! -f $f ]]; then bad "$1: no $f"; return; fi
    if ! cmp -s "$src/${sources[$i]}" "$f"; then bad "$1: ${notices[$i]} differs from ${sources[$i]}"; return; fi
  done
  ok "$1"
}

# finds_fonts <name> <dir>: <dir>/NOTICE sends the reader to the font's notices
# by a path that exists there (an install), and to the source tree's copy
finds_fonts() {
  local t; t=$(tr -s ' \n' ' ' < "$2/NOTICE")
  if [[ $t != *"the oldschool-pc-fonts/ directory beside this file (share/doc/blockwright/oldschool-pc-fonts/)"* ]]; then
    bad "$1: NOTICE does not name share/doc/blockwright/oldschool-pc-fonts/"
  elif [[ ! -f $2/oldschool-pc-fonts/LICENSE || ! -f $2/oldschool-pc-fonts/README.md ]]; then
    bad "$1: no oldschool-pc-fonts/ beside NOTICE in $2"
  elif [[ $t != *"in the source tree, third_party/oldschool-pc-fonts/."* ]]; then
    bad "$1: NOTICE does not name third_party/oldschool-pc-fonts/"
  elif [[ ! -f $src/third_party/oldschool-pc-fonts/LICENSE || ! -f $src/third_party/oldschool-pc-fonts/README.md ]]; then
    bad "$1: the source tree has no third_party/oldschool-pc-fonts/"
  else
    ok "$1"
  fi
}

# finds_icons <name> <dir>: <dir>/NOTICE sends the reader to the icons'
# notices by a path that exists there, and to the source tree's copy
finds_icons() {
  local t; t=$(tr -s ' \n' ' ' < "$2/NOTICE")
  if [[ $t != *"the tabler-icons/ directory beside this file (share/doc/blockwright/tabler-icons/)"* ]]; then
    bad "$1: NOTICE does not name share/doc/blockwright/tabler-icons/"
  elif [[ ! -f $2/tabler-icons/LICENSE || ! -f $2/tabler-icons/README.md ]]; then
    bad "$1: no tabler-icons/ beside NOTICE in $2"
  elif [[ $t != *"in the source tree, third_party/tabler-icons/."* ]]; then
    bad "$1: NOTICE does not name third_party/tabler-icons/"
  else
    ok "$1"
  fi
}

# icon_credit <name> <gui>: the GUI's --version credits the icons
icon_credit() {
  local out; out=$("$2" --version 2>&1)
  if grep -q 'Includes icons from Tabler Icons, MIT licence, https://tabler.io/icons;' <<<"$out"; then
    ok "$1"
  else
    bad "$1: $out"
  fi
}

# credits <name> <cli>: --version names the fonts, their author and licence
credits() {
  local out; out=$("$2" --version 2>&1)
  if grep -q 'IBM VGA 8x16 and IBM PC BIOS 8x8 fonts, (c) 2016-2020 VileR,' <<<"$out" \
     && grep -q 'CC BY-SA 4.0, https://int10h.org/oldschool-pc-fonts/' <<<"$out" \
     && grep -qF "notices in $notices_dir" <<<"$out"; then
    ok "$1"
  else
    bad "$1: $out"
  fi
}

# staged <build> <destdir>: install <build> below <destdir>, and fail unless
# every file its manifest lists (without the DESTDIR) is there
staged() {
  local f
  DESTDIR=$2 "$real_cmake" --install "$1" ${config:+--config "$config"} >/dev/null || return 1
  while IFS= read -r f; do
    [[ -f $2$f ]] || { echo "not staged: $f"; return 1; }
  done < "$1/install_manifest.txt"
}

real_cmake=$(command -v cmake)
stage=$tmp/stage
if staged "$build" "$stage"; then ok "cmake --install staged in $stage"; else bad "cmake --install failed or did not stage in $stage"; fi
has_notices "staged install carries the notices" "$stage$docdir"
finds_fonts "staged NOTICE points to the font notices" "$stage$docdir"
finds_icons "staged NOTICE points to the icon notices" "$stage$docdir"
credits "installed --version credits the font" "$stage$bindir/blockwright-cli"
credits "installed GUI --version credits the font" "$stage$bindir/blockwright"
icon_credit "installed GUI --version credits the icons" "$stage$bindir/blockwright"

# install-dev, with everything but the install step stubbed
home=$tmp/home stubs=$tmp/stubs
mkdir -p "$home/.cache/dw" "$home/.local/opt" "$stubs"
cat > "$stubs/git" <<'STUB'
#!/bin/sh
for a; do case $a in --short) echo 0123456; exit 0 ;; esac; done
case " $* " in *" rev-parse "*) echo 0123456789abcdef0123456789abcdef01234567 ;; esac
exit 0
STUB
printf '#!/bin/sh\nexit 0\n' > "$stubs/tar"
printf '#!/bin/sh\nexit 0\n' > "$stubs/nice"
# --install <build> [--config <c>] --prefix <stage>: the real install of this
# build, staged below the scratch dir, then laid out in <stage> the way
# install-dev's own build (default install dirs under its prefix) lays it out.
# The stub's source is fixed text: the paths reach it through STUB_* in the
# environment, so a path with spaces or shell metacharacters stays one word.
cat > "$stubs/cmake" <<'STUB'
#!/bin/sh
set -eu
[ "${1:-}" = --install ] || exit 0
prefix=
while [ $# -gt 0 ]; do
  case $1 in --prefix) prefix=$2; shift ;; esac
  shift
done
case ${STUB_TMP:-} in /?*) ;; *) echo "cmake stub: no absolute STUB_TMP" >&2; exit 1 ;; esac
case $prefix in /?*) ;; *) echo "cmake stub: no absolute --prefix" >&2; exit 1 ;; esac
d=$STUB_TMP/stub-stage
rm -rf "$d"
mkdir -p "$prefix/bin" "$prefix/share/doc"
DESTDIR=$d "$STUB_CMAKE" --install "$STUB_BUILD" ${STUB_CONFIG:+--config "$STUB_CONFIG"} >/dev/null
while IFS= read -r f; do
  [ -f "$d$f" ] || { echo "cmake stub: not staged: $f" >&2; exit 1; }
done < "$STUB_BUILD/install_manifest.txt"
cp -R "$d$STUB_BINDIR/." "$prefix/bin/"
cp -R "$d$STUB_DOCDIR" "$prefix/share/doc/blockwright"
lib=$(find "$d" -type d -path '*/blockwright/library' | head -n 1)
[ -n "$lib" ] || { echo "cmake stub: no bundled library staged" >&2; exit 1; }
mkdir -p "$prefix/share/blockwright"
cp -R "$lib" "$prefix/share/blockwright/library"
icons=$(find "$d" -type d -path '*/icons/hicolor' | head -n 1)
if [ -n "$icons" ]; then
  mkdir -p "$prefix/share/icons"
  cp -R "$icons" "$prefix/share/icons/hicolor"
fi
[ -z "${NOLIB:-}" ] || rm -rf "$prefix/share/blockwright"
[ -z "${NOICON:-}" ] || rm -rf "$prefix/share/icons"
[ -z "${NODOCS:-}" ] || rm -rf "$prefix/share/doc"
[ -z "${NOGUI:-}" ] || rm -f "$prefix/bin/blockwright"
STUB
chmod +x "$stubs"/*

dev() {
  HOME=$home PATH=$stubs:$PATH STUB_TMP=$tmp STUB_CMAKE=$real_cmake STUB_BUILD=$build \
    STUB_CONFIG=$config STUB_BINDIR=$bindir STUB_DOCDIR=$docdir \
    bash "$src/scripts/install-dev" --ref 0123456 "$@" 2>&1
}
prefix=$home/.local/opt/blockwright-dev
if out=$(dev); then ok "install-dev"; else bad "install-dev failed: $out"; fi
has_notices "install-dev carries the notices" "$prefix/share/doc/blockwright"
finds_fonts "install-dev NOTICE points to the font notices" "$prefix/share/doc/blockwright"
finds_icons "install-dev NOTICE points to the icon notices" "$prefix/share/doc/blockwright"
credits "blockwright-cli-dev --version credits the font" "$home/.local/bin/blockwright-cli-dev"
credits "blockwright-dev --version credits the font" "$home/.local/bin/blockwright-dev"
icon_credit "blockwright-dev --version credits the icons" "$home/.local/bin/blockwright-dev"
if [[ $("$home/.local/bin/blockwright-dev" --version | head -n1) =~ ^blockwright\ [0-9.]+\ \(.+\)$ ]]; then
  ok "blockwright-dev --version names the commit"
else
  bad "blockwright-dev --version: $("$home/.local/bin/blockwright-dev" --version 2>&1)"
fi

# The launcher entry: the dev name, the commit, the dev binary with %f, the icon
desktop=$home/.local/share/applications/blockwright-dev.desktop
want=$(printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=Blockwright (dev)' 'GenericName=ANSI art editor' \
  'Comment=Blockwright dev build 0123456' "Exec=\"$home/.local/bin/blockwright-dev\" %f" 'Terminal=false' \
  'Categories=Graphics;2DGraphics;')
# and on Linux the installed icon, by its path (#205)
[[ $(uname -s) == Linux ]] && want+=$'\n'"Icon=$prefix/share/icons/hicolor/256x256/apps/blockwright.png"
if [[ -f $desktop && $(<"$desktop") == "$want" ]]; then
  ok "install-dev writes the launcher entry"
else
  bad "launcher entry $desktop: $(cat "$desktop" 2>&1)"
fi
if command -v desktop-file-validate >/dev/null; then
  if out=$(desktop-file-validate "$desktop" 2>&1) && [[ -z $out ]]; then ok "the launcher entry validates"; else bad "desktop-file-validate: $out"; fi
fi
if [[ ! -e $desktop.new ]]; then ok "no temporary launcher entry left"; else bad "left $desktop.new"; fi

# --apps-dir and --bin-dir move the launcher entry and its Exec
apps=$home/elsewhere/apps
if out=$(dev --apps-dir "$apps" --bin-dir "$home/other bin"); then ok "install-dev --apps-dir"; else bad "install-dev --apps-dir failed: $out"; fi
if grep -qxF "Exec=\"$home/other bin/blockwright-dev\" %f" "$apps/blockwright-dev.desktop" 2>/dev/null \
   && [[ -x "$home/other bin/blockwright-dev" ]]; then
  ok "the launcher entry follows --apps-dir and --bin-dir"
else
  bad "launcher entry in $apps: $(cat "$apps/blockwright-dev.desktop" 2>&1)"
fi

echo old > "$prefix/marker"
out=$(NODOCS=1 dev); st=$?
if [[ $st -ne 0 ]] && grep -q 'left out share/doc/blockwright/' <<<"$out"; then
  ok "install-dev refuses an install without the notices"
else
  bad "install-dev took an install without the notices (exit $st): $out"
fi
if [[ -f $prefix/marker && ! -e $prefix.new ]]; then ok "the refused install keeps the old dev build"; else bad "the refused install replaced the old dev build"; fi

out=$(NOGUI=1 dev); st=$?
if [[ $st -ne 0 ]] && grep -q 'left out bin/blockwright;' <<<"$out"; then
  ok "install-dev refuses an install without the GUI"
else
  bad "install-dev took an install without the GUI (exit $st): $out"
fi
if [[ -f $prefix/marker && ! -e $prefix.new ]]; then ok "the GUI-less install keeps the old dev build"; else bad "the GUI-less install replaced the old dev build"; fi

out=$(NOLIB=1 dev); st=$?
if [[ $st -ne 0 ]] && grep -q 'left out the bundled library' <<<"$out"; then
  ok "install-dev refuses an install without the bundled library"
else
  bad "install-dev took an install without the bundled library (exit $st): $out"
fi
if [[ -f $prefix/marker && ! -e $prefix.new ]]; then ok "the library-less install keeps the old dev build"; else bad "the library-less install replaced the old dev build"; fi
if [[ $(uname -s) == Linux ]]; then
  out=$(NOICON=1 dev); st=$?
  if [[ $st -ne 0 ]] && grep -q 'left out the icon' <<<"$out"; then
    ok "install-dev refuses an install without the icon"
  else
    bad "install-dev took an install without the icon (exit $st): $out"
  fi
  if [[ -f $prefix/marker && ! -e $prefix.new ]]; then ok "the icon-less install keeps the old dev build"; else bad "the icon-less install replaced the old dev build"; fi
fi
has_library() { [[ -n $(find "$1/share/blockwright/library" -mindepth 2 -maxdepth 2 -name '*.ans' -print -quit 2>/dev/null) ]]; }
if has_library "$prefix"; then ok "install-dev installs the bundled library"; else bad "no bundled library in $prefix"; fi
# A prefix with glob characters is a literal path: the library check must
# not read [dev] as a character class and refuse (and delete) a good install
bracket=$home/.local/opt/blockwright[dev]
if out=$(dev --prefix "$bracket"); then ok "install-dev --prefix with brackets"; else bad "install-dev --prefix with brackets failed: $out"; fi
if has_library "$bracket" && [[ -x $bracket/bin/blockwright ]]; then ok "the bracketed prefix holds the install and its library"; else bad "no install or library in $bracket"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
