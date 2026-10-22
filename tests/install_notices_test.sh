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
# dev install carries them and refuses an install without them.
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
credits "installed --version credits the font" "$stage$bindir/blockwright-cli"

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
credits "blockwright-cli-dev --version credits the font" "$home/.local/bin/blockwright-cli-dev"

echo old > "$prefix/marker"
out=$(NODOCS=1 dev); st=$?
if [[ $st -ne 0 ]] && grep -q 'left out share/doc/blockwright/' <<<"$out"; then
  ok "install-dev refuses an install without the notices"
else
  bad "install-dev took an install without the notices (exit $st): $out"
fi
if [[ -f $prefix/marker && ! -e $prefix.new ]]; then ok "the refused install keeps the old dev build"; else bad "the refused install replaced the old dev build"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
