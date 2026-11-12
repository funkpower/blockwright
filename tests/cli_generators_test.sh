#!/usr/bin/env bash
# CLI generator tests: cli_generators_test.sh <blockwright-cli> <tests/data> <scratch dir>
# add-generator and run-generators on .bwp and directory projects.
set -uo pipefail
cli=$1 data=$2 tmp=$3
if [[ -n ${BW_CLI_WINDOWS:-} ]]; then
  echo "skip: the generators here are sh scripts (test_generator_win.cpp runs generators on Windows)"
  exit 77
fi
rm -rf "$tmp" && mkdir -p "$tmp"
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
same() { if cmp -s "$2" "$3"; then ok "$1"; else bad "$1"; fi; }

piece=$data/golden/tux.ans

# a generator: blue ground (palette 1, the key) with red "AB" on it
gen=$tmp/sky
mkdir -p "$gen/lib" "$gen/.git"
printf '{"command": ["sh", "gen.sh"], "key": 1}\n' > "$gen/generator.json"
printf '{"word": "AB"}\n' > "$gen/params.json"
printf 'w=$(sed -n '\''s/.*"word":"\\([A-Z]*\\)".*/\\1/p'\'')\n. lib/colours.sh\nprintf "$BLUE  $RED%%s" "$w"\n' > "$gen/gen.sh"
printf 'BLUE=$(printf "\\033[0;44m")\nRED=$(printf "\\033[31m")\n' > "$gen/lib/colours.sh"
printf 'secret\n' > "$gen/.git/config"

expect "--help lists add-generator" 0 'add-generator <project> <dir>' -- "$cli" --help
expect "--help lists run-generators" 0 'run-generators <project>' -- "$cli" --help

"$cli" pack "$piece" "$tmp/p.bwp" >/dev/null || bad "pack"
expect "add-generator prints the new layer" 0 '^layer 2 \(sky\): 30x1$' -- "$cli" add-generator "$tmp/p.bwp" "$gen/"
expect "info lists the generator layer" 0 '^  2  generator  30x1 at 0,0 .* sky$' -- "$cli" info "$tmp/p.bwp"
expect "info lists its files" 0 '^  scripts/2/lib/colours\.sh ' -- "$cli" info "$tmp/p.bwp"
out=$("$cli" info "$tmp/p.bwp" 2>&1)
if grep -q 'scripts/2/\.git' <<<"$out"; then bad "hidden files were added"; else ok "hidden files are skipped"; fi
expect "--name names the layer" 0 '^layer 3 \(stars\): 30x1$' -- "$cli" add-generator --name stars "$tmp/p.bwp" "$gen"

# the export shows the generator's glyphs over tux and tux through its key colour
"$cli" convert "$tmp/p.bwp" "$tmp/p.ans" >/dev/null 2>&1 || bad "convert p.bwp"
"$cli" convert "$piece" "$tmp/tux.ans" >/dev/null 2>&1
if grep -aq 'AB' "$tmp/p.ans"; then ok "export shows the generator's output"; else bad "export lacks the output"; fi
if cmp -s "$tmp/p.ans" "$tmp/tux.ans"; then bad "export unchanged by the generator"; else ok "export changed by the generator"; fi

# run-generators on a directory project, after the script changes
"$cli" unpack "$tmp/p.bwp" "$tmp/dir" >/dev/null || bad "unpack"
printf '{"word": "XYZ"}\n' > "$tmp/dir/scripts/2/params.json"
printf '{"word": "XYZ"}\n' > "$tmp/dir/scripts/3/params.json"
expect "run-generators runs each one" 0 '^2 generators run$' -- "$cli" run-generators "$tmp/dir"
expect "run-generators reports each layer" 0 '^layer 2 \(sky\): 30x1$' -- "$cli" run-generators "$tmp/dir"
"$cli" convert "$tmp/dir" "$tmp/dir.ans" >/dev/null 2>&1 || bad "convert dir"
if grep -aq 'XYZ' "$tmp/dir.ans"; then ok "run-generators saved the new output"; else bad "new output not saved"; fi
expect "--layer runs only that one" 0 '^1 generator run$' -- "$cli" run-generators --layer 3 "$tmp/dir"

# a failing run writes nothing, not even the new output of a generator below
# it that ran first and succeeded
cp "$tmp/p.bwp" "$tmp/before.bwp"
cp -r "$tmp/dir" "$tmp/dir-before"
printf '{"word": "QQ"}\n' > "$tmp/dir/scripts/2/params.json"
printf 'echo "no paint" >&2; exit 3\n' > "$tmp/dir/scripts/3/gen.sh"
cp "$tmp/dir/scripts/2/params.json" "$tmp/dir-before/scripts/2/params.json"
cp "$tmp/dir/scripts/3/gen.sh" "$tmp/dir-before/scripts/3/gen.sh"
expect "a failing generator fails the run" 1 'layer 3 \(stars\): sh exited with status 3:' -- "$cli" run-generators "$tmp/dir"
expect "the one below it ran first" 1 '^layer 2 \(sky\): 30x1$' -- "$cli" run-generators "$tmp/dir"
expect "its stderr is reported" 1 '^no paint$' -- "$cli" run-generators "$tmp/dir"
expect "and nothing is written" 1 'dir: not written$' -- "$cli" run-generators "$tmp/dir"
if diff -r "$tmp/dir" "$tmp/dir-before" >/dev/null; then ok "the failed run left the project alone"; else bad "the failed run changed the project"; fi
same "layer 2 keeps its old output" "$tmp/dir/layers/2.cells" "$tmp/dir-before/layers/2.cells"
expect "layer 2 alone runs" 0 '^1 generator run$' -- "$cli" run-generators --layer 2 "$tmp/dir"
if cmp -s "$tmp/dir/layers/2.cells" "$tmp/dir-before/layers/2.cells"; then bad "layer 2's new output is the old one"; else ok "layer 2's new output differs"; fi
mkdir -p "$tmp/broken"
printf '{"command": ["sh", "-c", "exit 4"]}' > "$tmp/broken/generator.json"
expect "add-generator with a failing run" 1 'exited with status 4' -- "$cli" add-generator "$tmp/p.bwp" "$tmp/broken"
same "the failed add left the project alone" "$tmp/p.bwp" "$tmp/before.bwp"

mkdir -p "$tmp/slow"
printf '{"command": ["sh", "-c", "exec sleep 30"]}' > "$tmp/slow/generator.json"
start=$SECONDS
expect "--timeout stops a slow generator" 1 'took longer than 1000 ms; stopped' -- "$cli" add-generator --timeout 1 "$tmp/p.bwp" "$tmp/slow"
if (( SECONDS - start < 15 )); then ok "the slow generator was stopped in time"; else bad "the slow generator ran on"; fi
same "the stopped add left the project alone" "$tmp/p.bwp" "$tmp/before.bwp"

# a generator still running when the CLI is ended by a signal goes with it,
# and so does what it started
mkdir -p "$tmp/lingering"
printf '{"command": ["sh", "gen.sh"]}' > "$tmp/lingering/generator.json"
printf '(sleep 1; printf leaked > "%s") </dev/null >/dev/null 2>&1 &\nsleep 30\n' "$tmp/term-marker" > "$tmp/lingering/gen.sh"
"$cli" add-generator "$tmp/p.bwp" "$tmp/lingering" >/dev/null 2>&1 & clipid=$!
sleep 0.5
kill -TERM "$clipid"
wait "$clipid"; st=$?
if [[ $st -eq 143 ]]; then ok "SIGTERM ends the CLI as it would have"; else bad "SIGTERM: exit $st, wanted 143"; fi
sleep 1.5
if [[ -e $tmp/term-marker ]]; then bad "the generator's child outlived the CLI"; else ok "the generator's child went with the CLI"; fi
same "the ended add left the project alone" "$tmp/p.bwp" "$tmp/before.bwp"

# an ignored SIGCHLD survives exec, so a parent can hand one down; the CLI
# puts it back to the default, so the run still stops what the generator
# left running
if command -v python3 >/dev/null; then
  mkdir -p "$tmp/orphan"
  printf '{"command": ["sh", "gen.sh"]}' > "$tmp/orphan/generator.json"
  printf '(sleep 1; printf leaked > "%s") </dev/null >/dev/null 2>&1 &\nprintf ok\n' "$tmp/chld-marker" > "$tmp/orphan/gen.sh"
  cp "$tmp/p.bwp" "$tmp/chld.bwp"
  expect "add-generator under an inherited SIG_IGN for SIGCHLD" 0 '^layer [0-9]+ \(orphan\): ' -- \
    python3 -c 'import os, signal, sys
signal.signal(signal.SIGCHLD, signal.SIG_IGN)
os.execv(sys.argv[1], sys.argv[1:])' "$cli" add-generator "$tmp/chld.bwp" "$tmp/orphan"
  sleep 1.5
  if [[ -e $tmp/chld-marker ]]; then bad "under an inherited SIG_IGN the generator's child outlived the run"; else ok "under an inherited SIG_IGN the generator's child was stopped"; fi
else
  echo "skip SIGCHLD inheritance (no python3)"
fi

mkdir -p "$tmp/empty"
expect "a directory without generator.json" 1 'needs a generator\.json' -- "$cli" add-generator "$tmp/p.bwp" "$tmp/empty"
expect "a file for the directory" 1 'not a directory' -- "$cli" add-generator "$tmp/p.bwp" "$gen/gen.sh"
ln -s "$gen/gen.sh" "$tmp/empty/link.sh"
printf '{"command": ["sh", "link.sh"]}' > "$tmp/empty/generator.json"
expect "a link in the directory" 1 'link\.sh: not a plain file' -- "$cli" add-generator "$tmp/p.bwp" "$tmp/empty"
expect "add-generator on a flat file" 1 'not a project' -- "$cli" add-generator "$tmp/tux.ans" "$gen"
expect "run-generators on a flat file" 1 'not a project' -- "$cli" run-generators "$tmp/tux.ans"
expect "--layer of a cells layer" 1 'layer 1 \(Background\) is not a generator layer' -- "$cli" run-generators --layer 1 "$tmp/p.bwp"
expect "--layer of no layer" 1 'no layer 99' -- "$cli" run-generators --layer 99 "$tmp/p.bwp"
same "refused runs left the project alone" "$tmp/p.bwp" "$tmp/before.bwp"
expect "--layer takes an id" 2 "--layer takes a layer id, not 'sky'" -- "$cli" run-generators --layer sky "$tmp/p.bwp"
expect "--timeout range" 2 "--timeout must be 1-3600, not '0'" -- "$cli" run-generators --timeout 0 "$tmp/p.bwp"
expect "--timeout belongs to the generator commands" 2 '^blockwright-cli: --timeout is an add-generator, run-generators or import-scene option, not a info option$' -- "$cli" info --timeout 5 "$tmp/p.bwp"
expect "--name belongs to add-generator" 2 '^blockwright-cli: --name is an add-generator option, not a run-generators option$' -- "$cli" run-generators --name x "$tmp/p.bwp"
expect "add-generator usage" 2 'add-generator takes a project and a generator directory' -- "$cli" add-generator "$tmp/p.bwp"
expect "run-generators usage" 2 'run-generators takes one project' -- "$cli" run-generators "$tmp/p.bwp" "$gen"

# a hidden generator layer runs only when --layer names it (#440)
"$cli" unpack "$tmp/p.bwp" "$tmp/hid" >/dev/null || bad "unpack hid"
sed -e 's/"visible": true/"visible": false/' "$tmp/hid/layers/3.json" > "$tmp/hid-3.json"
mv "$tmp/hid-3.json" "$tmp/hid/layers/3.json"
grep -q '"visible": false' "$tmp/hid/layers/3.json" || bad "layer 3 not hidden"
printf '{"word": "HID"}\n' > "$tmp/hid/scripts/2/params.json"
printf '{"word": "HID"}\n' > "$tmp/hid/scripts/3/params.json"
cp "$tmp/hid/layers/3.cells" "$tmp/hid-3-before.cells"
cp "$tmp/hid/layers/2.cells" "$tmp/hid-2-before.cells"
out=$("$cli" run-generators "$tmp/hid" 2>/dev/null); st=$?
if [[ $st -eq 0 ]]; then ok "run-generators with a hidden layer succeeds"; else bad "run-generators with a hidden layer: exit $st"; fi
if grep -q '^layer 3 (stars): hidden, not run$' <<<"$out"; then ok "it says the hidden layer was skipped"; else bad "no skip line: $out"; fi
if grep -q '^layer 2 (sky): 30x1$' <<<"$out"; then ok "the visible layer still runs"; else bad "the visible layer didn't run: $out"; fi
if grep -q '^1 generator run$' <<<"$out"; then ok "only the visible layer is counted"; else bad "wrong count: $out"; fi
same "the hidden layer keeps its cells" "$tmp/hid/layers/3.cells" "$tmp/hid-3-before.cells"
if cmp -s "$tmp/hid/layers/2.cells" "$tmp/hid-2-before.cells"; then bad "the visible layer kept its old cells"; else ok "the visible layer has new cells"; fi
# a hidden layer whose program fails doesn't stop the others
printf 'exit 4\n' > "$tmp/hid/scripts/3/gen.sh"
expect "a failing hidden layer isn't run" 0 '^1 generator run$' -- "$cli" run-generators "$tmp/hid"
printf 'w=$(sed -n '\''s/.*"word":"\\([A-Z]*\\)".*/\\1/p'\'')\n. lib/colours.sh\nprintf "$BLUE  $RED%%s" "$w"\n' > "$tmp/hid/scripts/3/gen.sh"
out=$("$cli" run-generators --layer 3 "$tmp/hid" 2>&1); st=$?
if [[ $st -eq 0 ]] && grep -q '^layer 3 (stars): 30x1$' <<<"$out" && grep -q '^1 generator run$' <<<"$out"; then
  ok "--layer runs a hidden layer"; else bad "--layer of a hidden layer: exit $st: $out"; fi
if grep -q 'hidden, not run' <<<"$out"; then bad "--layer reported a skip: $out"; else ok "--layer skips nothing"; fi
if cmp -s "$tmp/hid/layers/3.cells" "$tmp/hid-3-before.cells"; then bad "the named hidden layer kept its old cells"; else ok "the named hidden layer has new cells"; fi
expect "--layer runs hidden and visible together" 0 '^2 generators run$' -- "$cli" run-generators --layer 3 --layer 2 "$tmp/hid"
grep -q '"visible": false' "$tmp/hid/layers/3.json" && ok "running it leaves it hidden" || bad "running it showed the layer"

# a project whose generators are all hidden runs none and is left alone
"$cli" unpack "$tmp/p.bwp" "$tmp/allhid" >/dev/null || bad "unpack allhid"
for id in 2 3; do
  sed -e 's/"visible": true/"visible": false/' "$tmp/allhid/layers/$id.json" > "$tmp/allhid-$id.json"
  mv "$tmp/allhid-$id.json" "$tmp/allhid/layers/$id.json"
done
cp -r "$tmp/allhid" "$tmp/allhid-before"
out=$("$cli" run-generators "$tmp/allhid" 2>&1); st=$?
if [[ $st -eq 0 ]]; then ok "all hidden: exit 0"; else bad "all hidden: exit $st: $out"; fi
if grep -q '^layer 2 (sky): hidden, not run$' <<<"$out" && grep -q '^layer 3 (stars): hidden, not run$' <<<"$out"; then
  ok "all hidden: a skip line for each"; else bad "all hidden: skip lines missing: $out"; fi
if grep -q '^0 generators run$' <<<"$out"; then ok "all hidden: 0 generators run"; else bad "all hidden: wrong count: $out"; fi
if diff -r "$tmp/allhid" "$tmp/allhid-before" >/dev/null; then ok "all hidden: the project is unchanged"; else bad "all hidden: the project changed"; fi

# a project with no generators runs none and isn't rewritten
"$cli" pack "$piece" "$tmp/plain.bwp" >/dev/null
cp "$tmp/plain.bwp" "$tmp/plain-before.bwp"
touch -t 200101010000 "$tmp/plain.bwp" && touch -t 200101020000 "$tmp/a-day-later"
expect "no generators" 0 '^0 generators run$' -- "$cli" run-generators "$tmp/plain.bwp"
if [[ $tmp/plain.bwp -ot $tmp/a-day-later ]]; then ok "a project with none isn't rewritten"; else bad "a project with none was rewritten"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
