#!/usr/bin/env bash
# CLI scene import tests: cli_scene_test.sh <blockwright-cli> <tests/data> <scratch dir> <ansidraw dir>
# import-scene on tests/data/scenes/lighthouse: the project's layers composite
# to what the scene writes, and the scene's directory is left alone. Skips
# (77) without python3 or ansidraw.py.
set -uo pipefail
cli=$1 data=$2 tmp=$3 ansidraw=${4:-}
if [[ -n ${BW_CLI_WINDOWS:-} ]]; then
  echo "skip: a Windows CLI runs the scene with a Windows Python (py -3, python or python3)"
  exit 77
fi
if ! command -v python3 >/dev/null || [[ ! -f $ansidraw/ansidraw.py ]]; then
  echo "skip: no python3, or no ansidraw.py in $ansidraw"
  exit 77
fi
export ANSIDRAW_DIR=$ansidraw
rm -rf "$tmp" && mkdir -p "$tmp/scene" "$tmp/flat"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# expect <name> <exit status> <stdout+stderr pattern> -- command...
expect() {
  local name=$1 want=$2 pat=$3; shift 4
  local out st
  out=$("$@" 2>&1); st=$?
  if [[ $st -ne $want ]]; then bad "$name: exit $st, wanted $want: $out"; return; fi
  if ! grep -Eq -- "$pat" <<<"$out"; then bad "$name: output doesn't match /$pat/: $out"; return; fi
  ok "$name"
}

cp "$data/scenes/lighthouse/scene.py" "$tmp/scene/"
cp "$data/scenes/lighthouse/scene.py" "$tmp/flat/"
(cd "$tmp/flat" && python3 scene.py >/dev/null 2>&1) || bad "the scene draws flat"

expect "--help lists import-scene" 0 '^  import-scene <scene.py> <out>$' -- "$cli" --help
out=$("$cli" import-scene "$tmp/scene/scene.py" "$tmp/p.bwp" 2>&1)
want=$'layer 1/4 (sky)\nlayer 2/4 (sea)\nlayer 3/4 (tower)\nlayer 4/4 (beam)\n4 layers, 24x8'
[[ $out == "$want" ]] && ok "import-scene to a zip names each layer as it runs" || bad "import-scene to a zip: $out"
[[ $(ls -A "$tmp/scene") == scene.py ]] && ok "nothing written beside the scene" || bad "files beside the scene: $(ls -A "$tmp/scene")"
expect "info lists four generator layers" 0 '^  4  generator  24x8 at 0,0 .* beam$' -- "$cli" info "$tmp/p.bwp"
expect "info has the scene's title" 0 'lighthouse' -- "$cli" info "$tmp/p.bwp"
expect "import-scene to a directory" 0 '^4 layers, 24x8$' -- "$cli" import-scene "$tmp/scene/scene.py" "$tmp/dir"
grep -q "\"--layer-ans=sea\"" "$tmp/dir/scripts/2/generator.json" && grep -q '"key": 13' "$tmp/dir/scripts/2/generator.json" &&
  grep -qF "\"$(cd "$tmp/scene" && pwd -P)/scene.py\"" "$tmp/dir/scripts/2/generator.json" &&
  ok "a layer's generator.json runs the scene at its path" || bad "generator.json: $(cat "$tmp/dir/scripts/2/generator.json")"
expect "run-generators runs the scene's layers again" 0 '' -- "$cli" run-generators "$tmp/p.bwp"

# the composite looks as the scene's own .ans does (a blank by its background)
expect "convert the project" 0 '' -- "$cli" convert "$tmp/p.bwp" "$tmp/p.ans"
expect "the composite is the flat render" 0 '^same$' -- python3 - "$tmp/p.ans" "$tmp/flat/lighthouse.ans" "$ansidraw" <<'PY'
import sys
sys.path.insert(0, sys.argv[3])
import ansidraw as D
W, H = 24, 8
def looks(path):
    rows = D.read_ans(path)[0]
    out = []
    for r in range(H):
        row = list(rows[r]) if r < len(rows) else []
        row += [(" ", D.GREY, D.BLACK)] * (W - len(row))
        out.append([("blank", bg) if D.blank_cell((g, fg, bg)) else (g, fg, bg) for g, fg, bg in row[:W]])
    return out
print("same" if looks(sys.argv[1]) == looks(sys.argv[2]) else "differ")
PY

expect "an existing zip isn't replaced" 1 'p\.bwp: not written: it exists$' -- "$cli" import-scene "$tmp/scene/scene.py" "$tmp/p.bwp"
mkdir -p "$tmp/full" && touch "$tmp/full/x"
expect "a directory holding something isn't written" 1 "it exists and isn't an empty directory" \
  -- "$cli" import-scene "$tmp/scene/scene.py" "$tmp/full"
printf 'import sys\nsys.exit("no layers here")\n' > "$tmp/broken.py"
expect "a failing scene says why" 1 'no layers here' -- "$cli" import-scene "$tmp/broken.py" "$tmp/broken.bwp"
[[ ! -e $tmp/broken.bwp ]] && ok "a failing scene writes nothing" || bad "broken.bwp written"
expect "a missing scene" 1 'is not a file' -- "$cli" import-scene "$tmp/missing.py" "$tmp/missing.bwp"
expect "import-scene takes two arguments" 2 '' -- "$cli" import-scene "$tmp/scene/scene.py"

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
