#!/usr/bin/env bash
# The licence and font notices install with the binary:
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
# installs blockwright-dev and its launcher entry.
set -uo pipefail
build=$1 src=$2 tmp=$3 bindir=$4 docdir=$5 config=${6:-}
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

notices=(LICENSE NOTICE oldschool-pc-fonts/LICENSE oldschool-pc-fonts/README.md)
sources=(LICENSE NOTICE third_party/oldschool-pc-fonts/LICENSE third_party/oldschool-pc-fonts/README.md)

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

# credits <name> <cli>: --version names the font, its author and licence
credits() {
  local out; out=$("$2" --version 2>&1)
  if grep -q 'IBM VGA 8x16 font, (c) 2016-2020 VileR, CC BY-SA 4.0' <<<"$out" \
     && grep -q 'share/doc/blockwright/' <<<"$out"; then
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
credits "installed --version credits the font" "$stage$bindir/blockwright-cli"
credits "installed GUI --version credits the font" "$stage$bindir/blockwright"

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
credits "blockwright-cli-dev --version credits the font" "$home/.local/bin/blockwright-cli-dev"
credits "blockwright-dev --version credits the font" "$home/.local/bin/blockwright-dev"
if [[ $("$home/.local/bin/blockwright-dev" --version | head -n1) =~ ^blockwright\ [0-9.]+\ \(.+\)$ ]]; then
  ok "blockwright-dev --version names the commit"
else
  bad "blockwright-dev --version: $("$home/.local/bin/blockwright-dev" --version 2>&1)"
fi

# The launcher entry: the dev name, the commit, the dev binary with %f
desktop=$home/.local/share/applications/blockwright-dev.desktop
want=$(printf '%s\n' '[Desktop Entry]' 'Type=Application' 'Name=Blockwright (dev)' 'GenericName=ANSI art editor' \
  'Comment=Blockwright dev build 0123456' "Exec=\"$home/.local/bin/blockwright-dev\" %f" 'Terminal=false' \
  'Categories=Graphics;2DGraphics;')
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

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
