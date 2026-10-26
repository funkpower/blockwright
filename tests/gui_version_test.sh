#!/usr/bin/env bash
# blockwright --version, --help and usage errors, with no display: gui_version_test.sh <blockwright>
set -uo pipefail
bin=$1
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# No display at all: --version must not need one.
out=$(env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM= "$bin" --version 2>&1); st=$?
first=$(head -n1 <<<"$out")
if [[ $st -eq 0 && $first =~ ^blockwright\ 0\.1\.0\ \([0-9a-f]{7,}(-dirty)?\)$|^blockwright\ 0\.1\.0\ \(unknown\)$ ]]; then
  ok "--version: $first"
else
  bad "--version (exit $st): $out"
fi
if grep -q 'IBM VGA 8x16 and IBM PC BIOS 8x8 fonts, (c) 2016-2020 VileR,' <<<"$out" && grep -q 'CC BY-SA 4.0, https://int10h.org/oldschool-pc-fonts/' <<<"$out" && grep -q 'share/doc/blockwright/' <<<"$out"; then
  ok "--version credits the font"
else
  bad "--version has no font credit: $out"
fi
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$bin" --help 2>&1); st=$?
if [[ $st -eq 0 ]] && grep -q 'Usage: blockwright' <<<"$out"; then ok "--help"; else bad "--help (exit $st): $out"; fi
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$bin" --bogus 2>&1); st=$?
if [[ $st -eq 2 ]] && grep -q 'unknown option' <<<"$out"; then ok "unknown option"; else bad "unknown option (exit $st): $out"; fi
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$bin" a.ans b.ans 2>&1); st=$?
if [[ $st -eq 2 ]] && grep -q 'one file at a time' <<<"$out"; then ok "two files"; else bad "two files (exit $st): $out"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
