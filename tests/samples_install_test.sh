#!/usr/bin/env bash
# The sample projects install to <datadir>/blockwright/samples/<name>/ as
# they are in the source, and the installed CLI reads each one:
#   samples_install_test.sh <build dir> <scratch dir> <full datadir> <full bindir> <source samples> [<config>]
set -uo pipefail
build=$1 tmp=$2 datadir=$3 bindir=$4 src=$5 config=${6:-}
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

stage=$tmp/stage
args=(--install "$build")
[[ -n $config ]] && args+=(--config "$config")
if ! DESTDIR=$stage cmake "${args[@]}" >"$tmp/install.log" 2>&1; then
  bad "cmake --install: $(cat "$tmp/install.log")"
  exit 1
fi
dest=$stage$datadir/blockwright/samples
cli=$stage$bindir/blockwright-cli
n=0
for s in "$src"/[0-9][0-9]-*/; do
  name=$(basename "$s")
  n=$((n + 1))
  if [[ ! -f $dest/$name/manifest.json ]]; then
    bad "$name is not installed in $dest"
    continue
  fi
  missing=$(cd "$s" && find . -type f | while read -r f; do cmp -s "$f" "$dest/$name/$f" || echo "$f"; done)
  extra=$(cd "$dest/$name" && find . -type f | while read -r f; do [[ -f $s/$f ]] || echo "$f"; done)
  if [[ -z $missing && -z $extra ]]; then ok "$name installs as in the source"; else bad "$name: differs: $missing $extra"; fi
  out=$("$cli" lint "$dest/$name" 2>&1); st=$?
  if [[ $st -eq 0 ]]; then ok "the installed CLI reads and lints $name clean"; else bad "lint $name (exit $st): $out"; fi
done
if [[ $n -ge 1 ]]; then ok "$n sample(s) in the source"; else bad "no samples in $src"; fi
# the tool that draws them is not installed with them
if [[ -z $(find "$dest" -name maker -o -name CMakeLists.txt) ]]; then ok "only the samples are installed"; else bad "more than the samples in $dest"; fi
exit $((fails > 0))
