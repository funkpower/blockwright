#!/usr/bin/env bash
# The AppImage's AppRun runs the editor, or blockwright-cli under a name
# starting blockwright-cli or with --cli first:
#   apprun_test.sh <packaging/linux/AppRun> <scratch dir>
#
# Runs it in an AppDir whose two programs print their name and arguments, as
# the AppImage runtime does (ARGV0 is the name the AppImage was run under)
# and as an extracted AppDir is run (through a symlink, no ARGV0).
set -uo pipefail
apprun=$1 tmp=$2
rm -rf "$tmp" && mkdir -p "$tmp/AppDir/usr/bin"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
cp "$apprun" "$tmp/AppDir/AppRun" && chmod +x "$tmp/AppDir/AppRun"
for p in blockwright blockwright-cli; do
  printf '#!/bin/sh\necho "%s $*"\n' "$p" > "$tmp/AppDir/usr/bin/$p"
  chmod +x "$tmp/AppDir/usr/bin/$p"
done
ln -s AppDir/AppRun "$tmp/blockwright-cli"
ln -s AppDir/AppRun "$tmp/Blockwright.AppImage"

expect() {  # <label> <expected> <command...>
  local label=$1 want=$2 got
  shift 2
  got=$("$@" 2>&1)
  [[ $got == "$want" ]] && ok "$label" || bad "$label: '$got', not '$want'"
}
run=$tmp/AppDir/AppRun
expect "the editor, with its arguments" "blockwright a b.ans" env -u ARGV0 "$run" a b.ans
expect "--cli runs blockwright-cli" "blockwright-cli --version" env -u ARGV0 "$run" --cli --version
expect "--cli alone runs blockwright-cli" "blockwright-cli " env -u ARGV0 "$run" --cli
expect "--cli only first" "blockwright x --cli" env -u ARGV0 "$run" x --cli
expect "ARGV0 blockwright-cli" "blockwright-cli info x.ans" env ARGV0=/opt/bin/blockwright-cli "$run" info x.ans
expect "ARGV0 blockwright-cli-0.1.0-x86_64.AppImage" "blockwright-cli -h" env ARGV0=./blockwright-cli-0.1.0-x86_64.AppImage "$run" -h
expect "ARGV0 the AppImage's own name" "blockwright --version" env ARGV0=./blockwright-0.1.0-x86_64.AppImage "$run" --version
expect "a blockwright-cli symlink to the AppDir" "blockwright-cli --version" env -u ARGV0 "$tmp/blockwright-cli" --version
expect "another symlink to the AppDir" "blockwright --version" env -u ARGV0 "$tmp/Blockwright.AppImage" --version
exit $((fails > 0))
