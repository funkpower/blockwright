#!/usr/bin/env bash
# Projects on a case-folding directory (Linux casefold, as ext4 and tmpfs
# offer it, or a macOS volume): casefold_test.sh <blockwright-cli>
# <tests/data> <scratch dir>. On Linux it mounts a casefold tmpfs in a user
# and mount namespace; skips (77) where that isn't allowed.
set -uo pipefail
cli=$1 data=$2 tmp=$3
rm -rf "$tmp" && mkdir -p "$tmp/cf"
cd "$tmp" || exit 1

if [[ $(uname -s) == Darwin ]]; then
  # macOS's own volumes fold case (APFS and HFS+ unless formatted
  # case-sensitive), so cf/d is one
  inside() { rm -rf cf/d && mkdir cf/d && sh -c "$1"; }
  if ! inside 'touch cf/d/probe && [ -e cf/d/PROBE ]'; then
    echo "skip: $tmp is on a case-sensitive volume"
    exit 77
  fi
else
  inside() { unshare -rm sh -c 'mount -t tmpfs -o casefold tmpfs cf && mkdir cf/d && chattr +F cf/d && '"$1"; }
  if ! command -v unshare >/dev/null || ! command -v chattr >/dev/null || ! inside true 2>/dev/null; then
    echo "skip: no casefold tmpfs in a user namespace here"
    exit 77
  fi
fi

fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# a project holding one ref, refs/<name>, that reads <bytes>
ref() {
  rm -rf "src-$3" && "$cli" unpack "$data/golden/tux.ans" "src-$3" >/dev/null 2>&1 &&
    mkdir -p "src-$3/refs" && printf '%s' "$2" > "src-$3/refs/$1" && "$cli" pack "src-$3" "$3.bwp" >/dev/null 2>&1
}
small=$'\xc3\x9f' capital=$'\xe1\xba\x9e'
ref "$small" "content-small" small && ref "$capital" "content-capital" capital || { bad "fixtures"; exit 1; }

# capital and small sharp s are one name here, so a project holding both is
# refused (made where they are two names: not on a Mac, whose scratch
# directory folds them too)
mkdir -p both/refs && cp -r src-small/. both/ && printf 'capital' > "both/refs/$capital"
if [[ $(ls both/refs | wc -l) -ne 2 ]]; then echo "skip a project with refs/$capital and refs/$small: $tmp folds them"
elif "$cli" pack both both.bwp >/dev/null 2>&1; then bad "a project with refs/$capital and refs/$small is refused"
else ok "a project with refs/$capital and refs/$small is refused"; fi

# saving a project whose ref is renamed by case over one that held the old
# spelling keeps the ref, with the new bytes
out=$(inside "'$cli' unpack small.bwp cf/d/p && '$cli' unpack capital.bwp cf/d/p && cat cf/d/p/refs/* &&
  '$cli' pack cf/d/p '$tmp/out.bwp'" 2>&1); st=$?
if [[ $st -ne 0 ]]; then bad "save over a case-renamed ref: exit $st: $out"
elif [[ $out != content-capital ]]; then bad "save over a case-renamed ref: refs/ holds \"$out\""
else ok "save over a case-renamed ref keeps it"; fi
rm -rf back && "$cli" unpack out.bwp back >/dev/null 2>&1
if [[ $(cat back/refs/* 2>/dev/null) == content-capital ]]; then ok "the saved project packs with the ref"
else bad "the saved project packs without the ref"; fi

exit $((fails > 0))
