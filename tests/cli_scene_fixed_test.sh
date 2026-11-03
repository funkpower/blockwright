#!/usr/bin/env bash
# import-scene's options for a project that rebuilds byte for byte (#308),
# and a scene's "ice": cli_scene_fixed_test.sh <blockwright-cli> <scratch dir>
# The scenes are small Python scripts written here that answer
# --list-layers and --layer-ans= themselves, so ansidraw isn't needed.
# Skips (77) without python3.
set -uo pipefail
cli=$1 tmp=$2
if [[ -n ${BW_CLI_WINDOWS:-} ]]; then
  echo "skip: a Windows CLI runs the scene with a Windows Python (py -3, python or python3)"
  exit 77
fi
if ! command -v python3 >/dev/null; then
  echo "skip: no python3"
  exit 77
fi
unset SOURCE_DATE_EPOCH
rm -rf "$tmp" && mkdir -p "$tmp/a" "$tmp/b"
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
same() { cmp -s "$2" "$3" && ok "$1" || bad "$1: $(cmp "$2" "$3" 2>&1)"; }
differ() { cmp -s "$2" "$3" && bad "$1: $2 and $3 are the same" || ok "$1"; }
# The zip's entries in order, each with its time, method, CRC and size.
entries() {
  python3 -I - "$1" <<'PY'
import sys, zipfile
for i in zipfile.ZipFile(sys.argv[1]).infolist():
    print(i.filename, i.date_time, i.compress_type, i.CRC, i.file_size, i.external_attr)
PY
}

# A scene of two layers, its title from $1 (the issue's repro scene).
scene() {
  cat <<EOF
import json, sys
W, H = 4, 1
def cell(fg, bg, ch): return f"\x1b[0;3{fg};4{bg}m{ch}"
if "--list-layers" in sys.argv:
    print(json.dumps({"width": W, "rows": H, "title": "$1",
                      "layers": [{"name": "sky"}, {"name": "sun", "key": 5}]}))
    sys.exit()
name = next(a.split("=", 1)[1] for a in sys.argv if a.startswith("--layer-ans="))
row = [cell(7, 4, " ")] * W
if name == "sun":
    row = [cell(7, 5, " ")] * W
    row[1] = cell(3, 5, "\xdb")
sys.stdout.buffer.write(("".join(row) + "\x1b[0m\r\n").encode("latin1"))
EOF
}
scene tiny > "$tmp/a/scene.py"
cp "$tmp/a/scene.py" "$tmp/b/scene.py"
scene "" > "$tmp/a/untitled.py"
today=$(date +%Y%m%d)

expect "--help lists --bake" 0 '^  --bake +import-scene: write each layer as the cells it made' -- "$cli" --help
expect "--help lists --author" 0 '^  --author <name> +import-scene: the SAUCE author' -- "$cli" --help
expect "--help lists --date" 0 '^  --date <CCYYMMDD> +import-scene: the SAUCE date' -- "$cli" --help
expect "--help says how to rebuild a project byte for byte" 0 'To rebuild a project byte for byte' -- "$cli" --help

# Without the options nothing changes: the layers run the scene by its
# absolute path, the date is today's and the author empty.
expect "plain import" 0 '^2 layers, 4x1$' -- "$cli" import-scene "$tmp/a/scene.py" "$tmp/plain1.bwp"
expect "plain import again" 0 '' -- "$cli" import-scene "$tmp/a/scene.py" "$tmp/plain2.bwp"
same "a plain import of the same scene on the same day is the same file" "$tmp/plain1.bwp" "$tmp/plain2.bwp"
expect "plain import from another directory" 0 '' -- "$cli" import-scene "$tmp/b/scene.py" "$tmp/plain-b.bwp"
differ "a plain import from another directory names that scene" "$tmp/plain1.bwp" "$tmp/plain-b.bwp"
expect "plain import keeps generator layers" 0 '^  2  generator  4x1 at 0,0 .* sun$' -- "$cli" info "$tmp/plain1.bwp"
expect "plain import keeps the scripts" 0 '^files: +2$' -- "$cli" info "$tmp/plain1.bwp"
expect "plain import's date is today" 0 "^date: +$today$" -- "$cli" info "$tmp/plain1.bwp"
expect "plain import has no author" 0 '^author: *$' -- "$cli" info "$tmp/plain1.bwp"
expect "plain import is iCE on" 0 '^ice: +on$' -- "$cli" info "$tmp/plain1.bwp"
expect "plain import to a directory" 0 '' -- "$cli" import-scene "$tmp/a/scene.py" "$tmp/plain-dir"
grep -qF "\"$(cd "$tmp/a" && pwd -P)/scene.py\"" "$tmp/plain-dir/scripts/2/generator.json" &&
  ok "a plain import's generator.json runs the scene at its path" ||
  bad "generator.json: $(cat "$tmp/plain-dir/scripts/2/generator.json")"

# --bake --author --date: the same file from another path, on another day
# (a time zone 26 hours from the first) and run after run.
fixed=(--bake --author funkpower --date 20261103)
expect "fixed import" 0 '^2 layers, 4x1$' -- "$cli" import-scene "${fixed[@]}" "$tmp/a/scene.py" "$tmp/fixed1.bwp"
expect "fixed import again" 0 '' -- "$cli" import-scene "${fixed[@]}" "$tmp/a/scene.py" "$tmp/fixed2.bwp"
expect "fixed import from another directory" 0 '' -- "$cli" import-scene "${fixed[@]}" "$tmp/b/scene.py" "$tmp/fixed-b.bwp"
expect "fixed import east of UTC" 0 '' -- env TZ=Etc/GMT-14 "$cli" import-scene "${fixed[@]}" "$tmp/a/scene.py" "$tmp/fixed-east.bwp"
expect "fixed import west of UTC" 0 '' -- env TZ=Etc/GMT+12 "$cli" import-scene "${fixed[@]}" "$tmp/b/scene.py" "$tmp/fixed-west.bwp"
for f in fixed2 fixed-b fixed-east fixed-west; do
  same "fixed1.bwp and $f.bwp are byte for byte the same" "$tmp/fixed1.bwp" "$tmp/$f.bwp"
  [[ $(entries "$tmp/fixed1.bwp") == "$(entries "$tmp/$f.bwp")" ]] &&
    ok "fixed1.bwp and $f.bwp have the same entries in the same order with the same times" ||
    bad "entries differ: $(diff <(entries "$tmp/fixed1.bwp") <(entries "$tmp/$f.bwp"))"
done
entries "$tmp/fixed1.bwp" | grep -q '^scripts/' && bad "a baked zip holds scripts" || ok "a baked zip holds no scripts"
grep -qa 'scene\.py' "$tmp/fixed1.bwp" && bad "a baked zip names the scene" || ok "a baked zip doesn't name the scene"
expect "baked layers are cells" 0 '^  2  cells  4x1 at 0,0 .* sun$' -- "$cli" info "$tmp/fixed1.bwp"
expect "a baked project keeps no files" 0 '^files: +0$' -- "$cli" info "$tmp/fixed1.bwp"
expect "--author sets the SAUCE author" 0 '^author: +funkpower$' -- "$cli" info "$tmp/fixed1.bwp"
expect "--date sets the SAUCE date" 0 '^date: +20261103$' -- "$cli" info "$tmp/fixed1.bwp"
expect "the scene's title stays" 0 '^title: +tiny$' -- "$cli" info "$tmp/fixed1.bwp"
expect "run-generators has nothing to run in a baked project" 0 '^0 generators run$' -- "$cli" run-generators "$tmp/fixed2.bwp"
# baking changes no cell: the composite is the plain import's
expect "render the plain import" 0 '' -- "$cli" render "$tmp/plain1.bwp" "$tmp/plain1.png"
expect "render the baked import" 0 '' -- "$cli" render "$tmp/fixed1.bwp" "$tmp/fixed1.png"
same "the baked composite renders as the plain one" "$tmp/plain1.png" "$tmp/fixed1.png"

# a directory is the same tree from either path
expect "fixed import to a directory" 0 '' -- "$cli" import-scene "${fixed[@]}" "$tmp/a/scene.py" "$tmp/fixed-dir-a"
expect "fixed import to a directory from another path" 0 '' -- "$cli" import-scene "${fixed[@]}" "$tmp/b/scene.py" "$tmp/fixed-dir-b"
diff -r "$tmp/fixed-dir-a" "$tmp/fixed-dir-b" >/dev/null && ok "both directories hold the same files" ||
  bad "the directories differ: $(diff -r "$tmp/fixed-dir-a" "$tmp/fixed-dir-b")"
[[ ! -e $tmp/fixed-dir-a/scripts ]] && ok "a baked directory has no scripts/" || bad "scripts/ in a baked directory"

# --bake alone takes the path out; the date is still today's
expect "--bake alone" 0 '' -- "$cli" import-scene --bake "$tmp/a/scene.py" "$tmp/bake-a.bwp"
expect "--bake alone from another path" 0 '' -- "$cli" import-scene --bake "$tmp/b/scene.py" "$tmp/bake-b.bwp"
same "--bake alone gives the same file from either path" "$tmp/bake-a.bwp" "$tmp/bake-b.bwp"
expect "--bake alone keeps today's date" 0 "^date: +$today$" -- "$cli" info "$tmp/bake-a.bwp"

# SOURCE_DATE_EPOCH stands in for today, in UTC whatever TZ says
expect "SOURCE_DATE_EPOCH" 0 '' -- env SOURCE_DATE_EPOCH=1700000000 TZ=Etc/GMT-14 \
  "$cli" import-scene "$tmp/a/scene.py" "$tmp/epoch-east.bwp"
expect "SOURCE_DATE_EPOCH sets the date in UTC" 0 '^date: +20231114$' -- "$cli" info "$tmp/epoch-east.bwp"
expect "SOURCE_DATE_EPOCH west of UTC" 0 '' -- env SOURCE_DATE_EPOCH=1700000000 TZ=Etc/GMT+12 \
  "$cli" import-scene "$tmp/a/scene.py" "$tmp/epoch-west.bwp"
same "SOURCE_DATE_EPOCH gives the same file in any time zone" "$tmp/epoch-east.bwp" "$tmp/epoch-west.bwp"
expect "SOURCE_DATE_EPOCH 0" 0 '' -- env SOURCE_DATE_EPOCH=0 "$cli" import-scene "$tmp/a/scene.py" "$tmp/epoch0.bwp"
expect "SOURCE_DATE_EPOCH 0 is 1970-01-01" 0 '^date: +19700101$' -- "$cli" info "$tmp/epoch0.bwp"
expect "--date wins over SOURCE_DATE_EPOCH" 0 '' -- env SOURCE_DATE_EPOCH=1700000000 \
  "$cli" import-scene --date 20000229 "$tmp/a/scene.py" "$tmp/epoch-date.bwp"
expect "--date's day is the one kept" 0 '^date: +20000229$' -- "$cli" info "$tmp/epoch-date.bwp"
expect "an empty SOURCE_DATE_EPOCH is unset" 0 '' -- env SOURCE_DATE_EPOCH= \
  "$cli" import-scene "$tmp/a/scene.py" "$tmp/epoch-empty.bwp"
expect "an empty SOURCE_DATE_EPOCH leaves today" 0 "^date: +$today$" -- "$cli" info "$tmp/epoch-empty.bwp"
expect "SOURCE_DATE_EPOCH on another command does nothing" 0 '' -- env SOURCE_DATE_EPOCH=junk \
  "$cli" info "$tmp/plain1.bwp"

# A scene without a title gets no SAUCE record, unless --author or --date
# asks for one; SOURCE_DATE_EPOCH alone makes none.
expect "untitled scene" 0 '' -- "$cli" import-scene --bake "$tmp/a/untitled.py" "$tmp/untitled.bwp"
expect "an untitled scene has no SAUCE" 0 '^sauce: +none$' -- "$cli" info "$tmp/untitled.bwp"
expect "untitled scene with SOURCE_DATE_EPOCH" 0 '' -- env SOURCE_DATE_EPOCH=1700000000 \
  "$cli" import-scene --bake "$tmp/a/untitled.py" "$tmp/untitled-epoch.bwp"
same "SOURCE_DATE_EPOCH makes no SAUCE record" "$tmp/untitled.bwp" "$tmp/untitled-epoch.bwp"
expect "untitled scene with --author" 0 '' -- env SOURCE_DATE_EPOCH=1700000000 \
  "$cli" import-scene --author funkpower "$tmp/a/untitled.py" "$tmp/untitled-author.bwp"
expect "--author makes a record" 0 '^author: +funkpower$' -- "$cli" info "$tmp/untitled-author.bwp"
expect "the record it makes takes SOURCE_DATE_EPOCH's day" 0 '^date: +20231114$' -- "$cli" info "$tmp/untitled-author.bwp"
expect "untitled scene with --date" 0 '' -- "$cli" import-scene --date 19991231 "$tmp/a/untitled.py" "$tmp/untitled-date.bwp"
expect "--date makes a record" 0 '^date: +19991231$' -- "$cli" info "$tmp/untitled-date.bwp"
expect "an author CP437 holds" 0 '' -- "$cli" import-scene --author 'Ré░Ç 20 characters!!' "$tmp/a/scene.py" "$tmp/cp437.bwp"
expect "...is kept as given" 0 '^author: +Ré░Ç 20 characters!!$' -- "$cli" info "$tmp/cp437.bwp"

# Refused before the scene runs, writing nothing (exit 2).
refused() {
  local name=$1 pat=$2; shift 2
  expect "$name" 2 "$pat" -- "$@" "$tmp/a/scene.py" "$tmp/refused.bwp"
  [[ ! -e $tmp/refused.bwp ]] || { bad "$name: refused.bwp written"; rm -f "$tmp/refused.bwp"; }
}
refused "an author of 21 characters" "--author takes 1 to 20 printable CP437 characters" \
  "$cli" import-scene --author 123456789012345678901
refused "an empty author" "--author takes 1 to 20" "$cli" import-scene --author ''
refused "an author CP437 can't hold" "--author takes 1 to 20" "$cli" import-scene --author 'funk✓'
refused "an author with a control code" "--author takes 1 to 20" "$cli" import-scene --author $'funk\tpower'
refused "a day that isn't one" "--date takes a day as CCYYMMDD, such as 20261103, not '20230229'" \
  "$cli" import-scene --date 20230229
refused "a month that isn't one" "--date takes a day as CCYYMMDD" "$cli" import-scene --date 20261301
refused "a short date" "--date takes a day as CCYYMMDD" "$cli" import-scene --date 2026113
refused "a date with dashes" "--date takes a day as CCYYMMDD" "$cli" import-scene --date 2026-11-03
for v in -1 1.5 ' 1' 1e9 abc 253402300800 99999999999999; do
  refused "SOURCE_DATE_EPOCH='$v'" "SOURCE_DATE_EPOCH is seconds since 1970-01-01 UTC as digits" \
    env SOURCE_DATE_EPOCH="$v" "$cli" import-scene
done
expect "the last second of 9999 is a SOURCE_DATE_EPOCH" 0 '' -- env SOURCE_DATE_EPOCH=253402300799 \
  "$cli" import-scene "$tmp/a/scene.py" "$tmp/epoch-last.bwp"
expect "...and its day is 99991231" 0 '^date: +99991231$' -- "$cli" info "$tmp/epoch-last.bwp"
for o in --bake '--author x' '--date 20261103'; do
  # shellcheck disable=SC2086
  expect "$o is refused on another command" 2 "${o%% *} is an import-scene option, not a run-generators option" \
    -- "$cli" run-generators $o "$tmp/plain1.bwp"
done

# "ice": false in the listing makes an iCE-off project whose composite is the
# scene's own flat picture; left out it is iCE on, as before.
cat > "$tmp/a/iceoff.py" <<'EOF'
import json, sys
W, H = 4, 1
ICE = sys.argv[0].endswith("iceoff.py")
def cell(fg, bg, ch, blink=False): return f"\x1b[0;{'5;' if blink else ''}3{fg};4{bg}m{ch}"
def sauce(ice):
    rec = b"SAUCE00" + b"iCE off".ljust(35) + b" " * 40 + b"20261103" + bytes(4) + bytes([1, 1])
    rec += W.to_bytes(2, "little") + H.to_bytes(2, "little") + bytes(4) + bytes([0, 1 if ice else 0])
    return b"\x1a" + rec + b"IBM VGA".ljust(22, b"\0")
def ans(cells, ice=False):
    return ("".join(cells) + "\x1b[0m\r\n").encode("latin1") + sauce(ice)
ground = [cell(7, 1, " "), cell(7, 4, "A", True), cell(7, 1, " "), cell(7, 1, " ")]
moon = [cell(7, 5, " ")] * 3 + [cell(3, 2, "B", True)]
if "--list-layers" in sys.argv:
    listing = {"width": W, "rows": H, "title": "iCE off",
               "layers": [{"name": "ground"}, {"name": "moon", "key": 5}]}
    if ICE:
        listing["ice"] = False
    print(json.dumps(listing))
elif "--flat" in sys.argv:
    sys.stdout.buffer.write(ans(ground[:3] + moon[3:]))
else:
    name = next(a.split("=", 1)[1] for a in sys.argv if a.startswith("--layer-ans="))
    sys.stdout.buffer.write(ans(ground if name == "ground" else moon))
EOF
cp "$tmp/a/iceoff.py" "$tmp/a/iceon.py"
python3 -I -B "$tmp/a/iceoff.py" --flat > "$tmp/flat.ans"
expect "the flat picture is iCE off" 0 '^ice: +off$' -- "$cli" info "$tmp/flat.ans"
expect "render the flat picture" 0 '' -- "$cli" render "$tmp/flat.ans" "$tmp/flat.png"
expect "an iCE-off scene" 0 '^2 layers, 4x1$' -- "$cli" import-scene "$tmp/a/iceoff.py" "$tmp/iceoff.bwp"
expect "an iCE-off scene makes an iCE-off project" 0 '^ice: +off$' -- "$cli" info "$tmp/iceoff.bwp"
expect "render the iCE-off project" 0 '' -- "$cli" render "$tmp/iceoff.bwp" "$tmp/iceoff.png"
same "the iCE-off composite renders as the flat picture" "$tmp/iceoff.png" "$tmp/flat.png"
expect "an iCE-off scene baked" 0 '' -- "$cli" import-scene --bake "$tmp/a/iceoff.py" "$tmp/iceoff-baked.bwp"
expect "a baked iCE-off project is iCE off" 0 '^ice: +off$' -- "$cli" info "$tmp/iceoff-baked.bwp"
expect "render the baked iCE-off project" 0 '' -- "$cli" render "$tmp/iceoff-baked.bwp" "$tmp/iceoff-baked.png"
same "the baked iCE-off composite renders as the flat picture" "$tmp/iceoff-baked.png" "$tmp/flat.png"
expect "a scene without \"ice\"" 0 '' -- "$cli" import-scene "$tmp/a/iceon.py" "$tmp/iceon.bwp"
expect "a scene without \"ice\" is iCE on" 0 '^ice: +on$' -- "$cli" info "$tmp/iceon.bwp"
expect "render the iCE-on project" 0 '' -- "$cli" render "$tmp/iceon.bwp" "$tmp/iceon.png"
differ "iCE on shows the blink as a bright background" "$tmp/iceon.png" "$tmp/flat.png"
sed 's/listing\["ice"\] = False/listing["ice"] = "no"/' "$tmp/a/iceoff.py" > "$tmp/a/badice.py"
sed -i 's/endswith("iceoff.py")/endswith("badice.py")/' "$tmp/a/badice.py"
expect "an \"ice\" that isn't true or false is refused" 1 '"ice" that is neither true nor false' \
  -- "$cli" import-scene "$tmp/a/badice.py" "$tmp/badice.bwp"
[[ ! -e $tmp/badice.bwp ]] && ok "a bad \"ice\" writes nothing" || bad "badice.bwp written"

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
