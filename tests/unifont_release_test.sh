#!/usr/bin/env bash
# gen-unifont --release names the release the .hex files it would read name,
# and nothing when they don't all name the same one:
#   unifont_release_test.sh <gen-unifont> <scratch dir>
set -uo pipefail
gen=$1 tmp=$2
rm -rf "$tmp" && mkdir -p "$tmp"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# release <name> <expected> <file>...: the files in a directory of their own
release() {
  local name=$1 want=$2 dir=$tmp/$((++n)) got
  shift 2
  mkdir -p "$dir"
  for f in "$@"; do : >"$dir/$f"; done
  got=$(python3 -I "$gen" --release "$dir"); local st=$?
  if [[ $st -eq 0 && $got == "$want" ]]; then
    ok "$name"
  else
    bad "$name: exit $st, '$got', not '$want'"
  fi
}
n=0

release "a release's font/precompiled" 18.0.01 \
  unifont-18.0.01.hex unifont_upper-18.0.01.hex unifont_all-18.0.01.hex unifont_csur-18.0.01.hex
release "gzipped" 16.0.04 unifont-16.0.04.hex.gz unifont_upper-16.0.04.hex.gz
release "the BMP set and unifont_all" 15.1.01 unifont-15.1.01.hex unifont_all-15.1.01.hex
release "Debian's /usr/share/unifont names none" "" unifont.hex unifont_all.hex
release "one file naming none" "" unifont-18.0.01.hex unifont_upper.hex
release "two releases" "" unifont-16.0.04.hex unifont_upper-18.0.01.hex
# what it reads with no release named is what the table is made from
release "the newest of two" 18.0.01 unifont-17.0.03.hex unifont-18.0.01.hex unifont_upper-18.0.01.hex

rm -rf "$tmp"
exit $((fails > 0))
