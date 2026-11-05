#!/usr/bin/env bash
# The samples checked in are what bw-make-samples writes now, byte for byte,
# the same files under the same names and no others (PLAN §12): a change to
# the maker or its notes that wasn't regenerated, or a sample file edited
# by hand, fails here.
#   samples_remade_test.sh <bw-make-samples> <source samples> <scratch dir>
set -uo pipefail
maker=$1 src=$2 tmp=$3
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

out=$tmp/samples
if ! "$maker" "$out" >"$tmp/maker.log" 2>&1; then
  bad "bw-make-samples: $(cat "$tmp/maker.log")"
  exit 1
fi
# every file of the source's samples, past the maker itself and the build
# file that installs them, and every file the maker drew
(cd "$src" && find . -type f ! -path './maker/*' ! -path ./CMakeLists.txt) | LC_ALL=C sort >"$tmp/checked-in"
(cd "$out" && find . -type f) | LC_ALL=C sort >"$tmp/remade"
if [[ ! -s $tmp/remade ]]; then
  bad "bw-make-samples drew nothing in $out"
  exit 1
fi
if cmp -s "$tmp/checked-in" "$tmp/remade"; then
  ok "the same $(wc -l <"$tmp/remade" | tr -d ' ') files, under the same names"
else
  bad "the file names differ (< checked in, > remade): $(diff "$tmp/checked-in" "$tmp/remade" | grep '^[<>]' | tr '\n' ' ')"
fi
differ=$(while IFS= read -r f; do
  [[ -f $src/$f ]] && ! cmp -s "$src/$f" "$out/$f" && echo "$f"
done <"$tmp/remade")
if [[ -z $differ ]]; then
  ok "each file as the maker draws it"
else
  bad "remade differently: $(echo "$differ" | tr '\n' ' ')(run bw-make-samples on the source's samples/)"
fi
[[ $fails -eq 0 ]] && rm -rf "$tmp"
exit $((fails > 0))
