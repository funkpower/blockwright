#!/bin/sh
# blockwright --screenshot (#295) with no display: app_screenshot_test.sh <blockwright> <test data> <scratch dir>
# The pictures a steps file asks for are written, one PNG each, the size of
# what they show; a dock the window doesn't have, a tool it doesn't have or
# a Tool Option out of range names the step and writes nothing, and so does
# a core error a step raises; a start whose read changed it says so as
# blockwright-cli replay does; usage errors exit 2. POSIX sh and tools only.
bin=$1
data=$2
tmp=$3
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

rm -rf "$tmp"
mkdir -p "$tmp" || exit 1

# A PNG's width and height from its IHDR: bytes 16..23, big-endian.
png_size() {
  od -An -tu1 -j16 -N8 "$1" | tr -s ' \n' '  ' | awk '{ printf "%d %d\n", (($1*256+$2)*256+$3)*256+$4, (($5*256+$6)*256+$7)*256+$8 }'
}
is_png() {
  [ "$(od -An -c -j1 -N3 "$1" | tr -d ' \n')" = PNG ]
}

out=$("$bin" --screenshot "$data/screenshot/tutorial.json" -o "$tmp/shots" 2>&1); st=$?
if [ "$st" -eq 0 ]; then ok "--screenshot exits 0"; else bad "--screenshot (exit $st): $out"; fi
for stem in 01-pick-ramp 02-stroke-before 02-stroke-after 03-ink 04-box 06-corner; do
  f=$tmp/shots/$stem.png
  if [ -f "$f" ] && is_png "$f"; then ok "$stem.png written"; else bad "no $stem.png"; fi
done
n=$(ls "$tmp/shots" | wc -l | tr -d ' ')
if [ "$n" = 6 ]; then ok "six pictures, none for the step with no screenshot"; else bad "$n files: $(ls "$tmp/shots")"; fi

# The window and the pixel crop at the window's size (the offscreen screen
# has a device pixel ratio of 1); the cell crop six cells by three at zoom 2.
size=$(png_size "$tmp/shots/02-stroke-after.png" 2>/dev/null)
if [ "$size" = "1024 720" ]; then ok "the window is 1024 x 720"; else bad "the window is $size"; fi
size=$(png_size "$tmp/shots/06-corner.png" 2>/dev/null)
if [ "$size" = "120 40" ]; then ok "the pixel crop is 120 x 40"; else bad "the pixel crop is $size"; fi
size=$(png_size "$tmp/shots/04-box.png" 2>/dev/null)
if [ "$size" = "96 96" ]; then ok "the cell crop is 96 x 96"; else bad "the cell crop is $size"; fi

# A dock the window doesn't have: exit 1, the step named, nothing written.
cat >"$tmp/nope.json" <<'EOF'
{"version": 1, "start": {"blank": [10, 4]}, "steps": [
  {"name": "first", "action": "selectAll", "screenshot": {}},
  {"name": "look", "action": "deselect", "screenshot": {"of": "nope"}}]}
EOF
out=$("$bin" --screenshot "$tmp/nope.json" -o "$tmp/nope" 2>&1); st=$?
case $out in
  *'step 2 (look): the window has no dock or widget "nope"'*) named=1 ;;
  *) named=0 ;;
esac
if [ "$st" -eq 1 ] && [ "$named" = 1 ] && [ ! -e "$tmp/nope/01-first.png" ]; then
  ok "an unknown dock fails at its step and writes nothing"
else
  bad "an unknown dock (exit $st): $out"
fi

# A tool the editor doesn't have, or a Tool Option out of its range: the
# steps file is refused before the window opens, exit 1, naming the step
# and what is wrong, and nothing written.
cat >"$tmp/tool.json" <<'EOF2'
{"version": 1, "start": {"blank": [10, 4]}, "steps": [
  {"name": "first", "action": "selectAll", "screenshot": {}},
  {"name": "paint", "action": "airbrush", "size": 3, "screenshot": {"of": "toolOptions"}}]}
EOF2
out=$("$bin" --screenshot "$tmp/tool.json" -o "$tmp/tool" 2>&1); st=$?
case $out in
  *'step 2 (paint): unknown action "airbrush"'*) named=1 ;;
  *) named=0 ;;
esac
if [ "$st" -eq 1 ] && [ "$named" = 1 ] && [ ! -e "$tmp/tool" ]; then
  ok "an unknown tool is refused at its step and nothing is written"
else
  bad "an unknown tool (exit $st): $out"
fi
cat >"$tmp/strength.json" <<'EOF2'
{"version": 1, "start": {"blank": [10, 4]}, "steps": [
  {"name": "first", "action": "selectAll", "screenshot": {}},
  {"name": "too-strong", "action": "rampShade", "strength": 2, "screenshot": {"of": "toolOptions"}}]}
EOF2
out=$("$bin" --screenshot "$tmp/strength.json" -o "$tmp/strength" 2>&1); st=$?
case $out in
  *'step 2 (too-strong): '*'"strength"'*) named=1 ;;
  *) named=0 ;;
esac
if [ "$st" -eq 1 ] && [ "$named" = 1 ] && [ ! -e "$tmp/strength" ]; then
  ok "a Tool Option out of range is refused at its step and nothing is written"
else
  bad "a Tool Option out of range (exit $st): $out"
fi

# What reading the start changed is said as blockwright-cli replay says it
# (#383): data/projects/stray-attrs is a 4.0 project with stray attribute
# bits in two cells, so the note is printed once, on stderr, before any
# step (here before the step that fails); a 5.0 project says nothing.
mkdir -p "$tmp/notes"
cp -R "$data/projects/stray-attrs" "$tmp/notes/stray"
cp -R "$data/projects/formatted" "$tmp/notes/clean"
for p in stray clean; do
  printf '{"version": 1, "start": {"file": "%s"}, "steps": [{"name": "sel", "action": "selectAll", "screenshot": {"of": "window"}}]}\n' \
    "$p" >"$tmp/notes/$p.json"
done
note="blockwright: note: $tmp/notes/stray.json: start: $tmp/notes/stray: cleared attribute bits no earlier build wrote from 2 cells"
"$bin" --screenshot "$tmp/notes/stray.json" -o "$tmp/notes/stray-shots" >"$tmp/notes/out" 2>"$tmp/notes/err"; st=$?
n=$(grep -c 'note:' "$tmp/notes/err")
if [ "$st" -eq 0 ] && [ "$n" = 1 ] && grep -qxF "$note" "$tmp/notes/err" && [ ! -s "$tmp/notes/out" ] &&
   [ -f "$tmp/notes/stray-shots/01-sel.png" ]; then
  ok "a start before 5.0 with stray bits says its read note once, on stderr"
else
  bad "the stray start's note (exit $st, $n notes): $(cat "$tmp/notes/err") / stdout: $(cat "$tmp/notes/out")"
fi
"$bin" --screenshot "$tmp/notes/clean.json" -o "$tmp/notes/clean-shots" >"$tmp/notes/out" 2>"$tmp/notes/err"; st=$?
if [ "$st" -eq 0 ] && ! grep -q 'note:' "$tmp/notes/err" && [ -f "$tmp/notes/clean-shots/01-sel.png" ]; then
  ok "a clean start says no note"
else
  bad "the clean start (exit $st): $(cat "$tmp/notes/err")"
fi

# A core error a step raises (#383): the start's next layer id is 0, so its
# ids are exhausted and New Cells Layer can't make one. The note first, then
# the step named with the core's message; exit 1, nothing written, and the
# "before" picture the step asks for never reaches the disk.
sed 's/"nextLayerId": 2/"nextLayerId": 0/' "$data/projects/stray-attrs/manifest.json" >"$tmp/notes/stray/manifest.json"
printf '{"version": 1, "start": {"file": "stray"}, "steps": [{"name": "add", "action": "newCellsLayer", "screenshot": {"of": "window", "when": "both"}}]}\n' \
  >"$tmp/notes/exh.json"
"$bin" --screenshot "$tmp/notes/exh.json" -o "$tmp/notes/exh-shots" >"$tmp/notes/out" 2>"$tmp/notes/err"; st=$?
want=$(printf '%s\n%s' "$note" "blockwright: step 1 (add): layer ids exhausted" | sed "s|/stray.json:|/exh.json:|")
got=$(grep '^blockwright: ' "$tmp/notes/err")
if [ "$st" -eq 1 ] && [ "$got" = "$want" ] && [ ! -e "$tmp/notes/exh-shots" ]; then
  ok "a core error at a step fails naming the step, exit 1, nothing written"
else
  bad "a core error at a step (exit $st): $(cat "$tmp/notes/err")"
fi

# Paths (#422): the window reads a steps file's paths as blockwright-cli
# replay does, so an absolute start is refused, naming the start, when the
# file is there, and nothing is written.
printf '{"version": 1, "start": {"file": "%s"}, "steps": [{"name": "sel", "action": "selectAll", "screenshot": {"of": "window"}}]}\n' \
  "$tmp/notes/clean" >"$tmp/notes/abs.json"
"$bin" --screenshot "$tmp/notes/abs.json" -o "$tmp/notes/abs-shots" >"$tmp/notes/out" 2>"$tmp/notes/err"; st=$?
want="blockwright: $tmp/notes/abs.json: start: \"file\" \"$tmp/notes/clean\" is absolute; a steps file names its files relative to its own directory"
got=$(grep '^blockwright: ' "$tmp/notes/err")
if [ "$st" -eq 1 ] && [ "$got" = "$want" ] && [ ! -e "$tmp/notes/abs-shots" ]; then
  ok "an absolute start is refused, naming the start, nothing written"
else
  bad "an absolute start (exit $st): $(cat "$tmp/notes/err")"
fi

# "scale": 2 (#295): the same pictures drawn at device pixel ratio 2, twice
# the pixels each way, from the same 1024 x 720 window.
sed -e 's/"of": "toolOptions"}/"of": "toolOptions", "scale": 2}/' \
    -e 's/"when": "both"}/"when": "both", "scale": 2}/' \
    -e 's/"cells": \[1, 1, 6, 3\]}}/"cells": [1, 1, 6, 3]}, "scale": 2}/' \
    -e 's/"pixels": \[0, 0, 120, 40\]}}/"pixels": [0, 0, 120, 40]}, "scale": 2}/' \
    "$data/screenshot/tutorial.json" >"$tmp/twice.json"
n=$(grep -c '"scale": 2' "$tmp/twice.json")
if [ "$n" = 4 ]; then ok "the tutorial at scale 2"; else bad "the tutorial at scale 2 has $n scaled pictures"; fi
out=$("$bin" --screenshot "$tmp/twice.json" -o "$tmp/twice" 2>&1); st=$?
if [ "$st" -eq 0 ]; then ok "--screenshot at scale 2 exits 0"; else bad "--screenshot at scale 2 (exit $st): $out"; fi
size=$(png_size "$tmp/twice/02-stroke-after.png" 2>/dev/null)
if [ "$size" = "2048 1440" ]; then ok "the window at scale 2 is 2048 x 1440"; else bad "the window at scale 2 is $size"; fi
size=$(png_size "$tmp/twice/06-corner.png" 2>/dev/null)
if [ "$size" = "240 80" ]; then ok "the pixel crop at scale 2 is 240 x 80"; else bad "the pixel crop at scale 2 is $size"; fi
size=$(png_size "$tmp/twice/04-box.png" 2>/dev/null)
if [ "$size" = "192 192" ]; then ok "the cell crop at scale 2 is 192 x 192"; else bad "the cell crop at scale 2 is $size"; fi
# the Layers dock wasn't asked for at 2: as at 1, byte for byte
if cmp -s "$tmp/twice/03-ink.png" "$tmp/shots/03-ink.png"; then
  ok "a picture at scale 1 beside them is the one the tutorial takes"
else
  bad "03-ink.png differs between the tutorial and its scale 2 copy"
fi
# the same file twice, the same bytes
out=$("$bin" --screenshot "$tmp/twice.json" -o "$tmp/again" 2>&1); st=$?
same=1
for f in "$tmp"/twice/*.png; do
  cmp -s "$f" "$tmp/again/$(basename "$f")" || same=0
done
if [ "$st" -eq 0 ] && [ "$same" = 1 ]; then ok "the scale 2 pictures are the same bytes every run"; else bad "a second run at scale 2 (exit $st) differs: $out"; fi

# The screens are fixed: what the environment says about scaling the
# screens or their fonts changes nothing, at scale 1 or 2.
out=$(QT_SCALE_FACTOR=1.5 QT_SCREEN_SCALE_FACTORS=2 QT_FONT_DPI=144 QT_USE_PHYSICAL_DPI=1 \
      QT_QPA_PLATFORM=offscreen:configfile="$tmp/nothing.json" \
      "$bin" --screenshot "$tmp/twice.json" -o "$tmp/scaled" 2>&1); st=$?
same=1
for f in "$tmp"/twice/*.png; do
  cmp -s "$f" "$tmp/scaled/$(basename "$f")" || same=0
done
if [ "$st" -eq 0 ] && [ "$same" = 1 ]; then
  ok "the environment's scale and font dpi change no picture"
else
  bad "under the environment's scale (exit $st) the pictures differ: $out $(png_size "$tmp/scaled/02-stroke-after.png" 2>/dev/null)"
fi

# The screens' file goes in TMPDIR, or in /tmp when the platform's arguments
# can't name it there (they are split at ':' and ';'). It is never left behind.
for d in 'semi;dir' 'col:dir' 'plain dir'; do
  mkdir -p "$tmp/tmpdir/$d"
  rm -f "$tmp/tmpdir-out/02-stroke-after.png"
  out=$(TMPDIR="$tmp/tmpdir/$d" "$bin" --screenshot "$data/screenshot/tutorial.json" -o "$tmp/tmpdir-out" 2>&1); st=$?
  if [ "$st" -eq 0 ] && [ -f "$tmp/tmpdir-out/02-stroke-after.png" ]; then
    ok "a TMPDIR of '$d' takes the pictures"
  else
    bad "a TMPDIR of '$d' (exit $st): $out"
  fi
  left=$(ls -A "$tmp/tmpdir/$d")
  if [ -z "$left" ]; then ok "a TMPDIR of '$d' is left empty"; else bad "a TMPDIR of '$d' holds $left"; fi
done

# A pointer (#295): "pointer": false is no pointer at all, the same bytes;
# true draws one, and a step the pointer doesn't make is refused by name.
cat >"$tmp/pointer.json" <<'EOF'
{"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720], "steps": [
  {"name": "stroke", "action": "glyph", "glyph": "#", "points": [[2, 2], [3, 2], [4, 2]],
   "screenshot": {"crop": {"cells": [0, 0, 8, 4]}}}]}
EOF
sed 's/"crop"/"pointer": false, "crop"/' "$tmp/pointer.json" >"$tmp/pointer-off.json"
sed 's/"crop"/"pointer": true, "crop"/' "$tmp/pointer.json" >"$tmp/pointer-on.json"
"$bin" --screenshot "$tmp/pointer.json" -o "$tmp/pointer-none" >/dev/null 2>&1
"$bin" --screenshot "$tmp/pointer-off.json" -o "$tmp/pointer-off" >/dev/null 2>&1
out=$("$bin" --screenshot "$tmp/pointer-on.json" -o "$tmp/pointer-on" 2>&1); st=$?
f=01-stroke.png
if [ -f "$tmp/pointer-none/$f" ] && cmp -s "$tmp/pointer-none/$f" "$tmp/pointer-off/$f"; then
  ok "\"pointer\": false gives the same bytes as no \"pointer\""
else
  bad "\"pointer\": false differs from no \"pointer\""
fi
if [ "$st" -eq 0 ] && [ -f "$tmp/pointer-on/$f" ] && ! cmp -s "$tmp/pointer-none/$f" "$tmp/pointer-on/$f" \
   && [ "$(png_size "$tmp/pointer-on/$f")" = "$(png_size "$tmp/pointer-none/$f")" ]; then
  ok "\"pointer\": true draws a pointer in a picture of the same size"
else
  bad "\"pointer\": true (exit $st): $out"
fi
cat >"$tmp/pointer-bad.json" <<'EOF'
{"version": 1, "start": {"blank": [20, 8]}, "steps": [
  {"name": "all", "action": "selectAll", "screenshot": {"pointer": true}}]}
EOF
out=$("$bin" --screenshot "$tmp/pointer-bad.json" -o "$tmp/pointer-bad" 2>&1); st=$?
case $out in
  *'(all)'*'"screenshot pointer" needs a step the pointer makes'*) named=1 ;;
  *) named=0 ;;
esac
if [ "$st" -eq 1 ] && [ "$named" = 1 ] && [ ! -e "$tmp/pointer-bad" ]; then
  ok "a pointer on a step the pointer doesn't make is refused by name"
else
  bad "a pointer on a selectAll step (exit $st): $out"
fi

# The window's font is the bundled one (#295): with no fonts from the
# machine at all, the Layers dock's title, header and rows are drawn the
# same, byte for byte. Only those: the status bar and the Glyphs bar draw
# block glyphs Noto Sans lacks in a system font (PLAN §9), whose height can
# move the docks' lower edges.
printf '<?xml version="1.0"?>\n<fontconfig></fontconfig>\n' >"$tmp/no-fonts.conf"
cat >"$tmp/layers.json" <<'EOF'
{"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720], "steps": [
  {"name": "ink", "action": "newCellsLayer", "rename": "ink",
   "screenshot": {"crop": {"pixels": [762, 200, 244, 90]}}}]}
EOF
"$bin" --screenshot "$tmp/layers.json" -o "$tmp/layers-fonts" >/dev/null 2>&1
out=$(FONTCONFIG_FILE="$tmp/no-fonts.conf" "$bin" --screenshot "$tmp/layers.json" -o "$tmp/layers-none" 2>&1); st=$?
if [ "$st" -eq 0 ] && [ -f "$tmp/layers-fonts/01-ink.png" ] && cmp -s "$tmp/layers-fonts/01-ink.png" "$tmp/layers-none/01-ink.png"; then
  ok "the Layers dock's rows are the same picture without the machine's fonts"
else
  bad "without the machine's fonts (exit $st) the Layers dock's rows differ: $out"
fi

# --user-library (#431): the user Library a saveToLibrary step saves to, made
# if it isn't there, and a placeStamp step finds the stamp in
cat > "$tmp/library.json" <<'EOF2'
{"version": 1, "start": {"blank": [20, 8]}, "window": [1024, 720], "steps": [
 {"name": "ink", "action": "keyboard", "at": [1, 1], "text": "hi"},
 {"name": "pick", "action": "select", "rect": [1, 1, 2, 1]},
 {"name": "save", "action": "saveToLibrary", "stamp": "Hi", "category": "words", "screenshot": {"of": "library"}},
 {"name": "put", "action": "placeStamp", "stamp": "Hi", "at": [8, 4], "screenshot": {}},
 {"name": "guides", "action": "view", "guides": "80x25", "screenshot": {}}]}
EOF2
out=$("$bin" --screenshot "$tmp/library.json" -o "$tmp/library" --user-library "$tmp/user-lib/new" 2>&1); st=$?
if [ "$st" -eq 0 ]; then ok "--screenshot with --user-library exits 0"; else bad "--screenshot with --user-library (exit $st): $out"; fi
for stem in 03-save 04-put 05-guides; do
  if [ -f "$tmp/library/$stem.png" ]; then ok "library: $stem.png written"; else bad "library: no $stem.png"; fi
done
n=$(ls "$tmp/user-lib/new" 2>/dev/null | grep -c '\.ans$')
j=$(ls "$tmp/user-lib/new" 2>/dev/null | grep -c '\.json$')
if [ "$n" -eq 1 ] && [ "$j" -eq 1 ]; then ok "--user-library holds the saved stamp"; else bad "--user-library holds: $(ls "$tmp/user-lib/new" 2>&1)"; fi
# without it the stamp goes to a scratch library, and the placeStamp still finds it
out=$("$bin" --screenshot "$tmp/library.json" -o "$tmp/library-scratch" 2>&1); st=$?
if [ "$st" -eq 0 ] && [ -f "$tmp/library-scratch/04-put.png" ]; then ok "--screenshot without --user-library saves to a scratch library"; else bad "without --user-library (exit $st): $out"; fi

# the stamps in --user-library are the dock's and placeStamp's in a later run
cat > "$tmp/library-put.json" <<'EOF2'
{"version": 1, "start": {"blank": [20, 8]}, "steps": [
 {"name": "put", "action": "placeStamp", "stamp": "Hi", "at": [8, 4], "screenshot": {"of": "library"}}]}
EOF2
out=$("$bin" --screenshot "$tmp/library-put.json" -o "$tmp/library-put" --user-library "$tmp/user-lib/new" 2>&1); st=$?
if [ "$st" -eq 0 ] && [ -f "$tmp/library-put/01-put.png" ]; then ok "--user-library's stamps are placed in a later run"; else bad "a later run placing from --user-library (exit $st): $out"; fi
# a run that fails after a save writes nothing to --user-library, as
# blockwright-cli replay doesn't (#431)
cat > "$tmp/library-fails.json" <<'EOF2'
{"version": 1, "start": {"blank": [20, 8]}, "steps": [
 {"name": "ink", "action": "keyboard", "at": [1, 1], "text": "hi"},
 {"name": "pick", "action": "select", "rect": [1, 1, 2, 1]},
 {"name": "save", "action": "saveToLibrary", "stamp": "Hi", "category": "words", "screenshot": {"of": "library"}},
 {"name": "bad", "action": "placeStamp", "stamp": "Nope", "at": [1, 1]}]}
EOF2
out=$("$bin" --screenshot "$tmp/library-fails.json" -o "$tmp/library-fails" --user-library "$tmp/user-lib/fails" 2>&1); st=$?
if [ "$st" -eq 1 ]; then ok "--screenshot: a run that fails after a save exits 1"; else bad "a run that fails after a save (exit $st): $out"; fi
if [ ! -e "$tmp/user-lib/fails" ] && [ ! -e "$tmp/library-fails" ]; then ok "--screenshot: a failed run writes nothing to --user-library"; else bad "--screenshot: a failed run wrote: $(ls "$tmp/user-lib/fails" "$tmp/library-fails" 2>&1)"; fi
# --user-library that is a regular file is refused, naming the flag
printf 'x' > "$tmp/user-lib/plain"
out=$("$bin" --screenshot "$tmp/library.json" -o "$tmp/library-plain" --user-library "$tmp/user-lib/plain" 2>&1); st=$?
if [ "$st" -eq 1 ] && [ "$out" = "blockwright: --user-library $tmp/user-lib/plain: not a directory" ] && [ ! -e "$tmp/library-plain" ]; then
  ok "--screenshot: a --user-library that is a file is refused before any step"
else
  bad "--screenshot with a file for --user-library (exit $st): $out"
fi

# a file in --user-library the window's copy of it can't read fails the run
# before any picture, naming that file (#506); root reads it anyway
if [ "$(id -u)" -ne 0 ]; then
  mkdir -p "$tmp/user-lib/locked"
  cp "$tmp/user-lib/new/"* "$tmp/user-lib/locked/"
  printf 'x' > "$tmp/user-lib/locked/notes.txt"
  chmod 000 "$tmp/user-lib/locked/notes.txt"
  out=$("$bin" --screenshot "$tmp/library-put.json" -o "$tmp/library-locked" --user-library "$tmp/user-lib/locked" 2>&1); st=$?
  chmod 644 "$tmp/user-lib/locked/notes.txt"
  case $out in
    *"--user-library $tmp/user-lib/locked: $tmp/user-lib/locked/notes.txt: can't be copied for the window: "*) named=1 ;;
    *) named=0 ;;
  esac
  if [ "$st" -eq 1 ] && [ "$named" -eq 1 ] && [ ! -e "$tmp/library-locked" ]; then
    ok "--screenshot: a --user-library file that can't be copied fails the run, naming it"
  else
    bad "--screenshot with an unreadable file in --user-library (exit $st): $out"
  fi
fi

# a relative link in --user-library that leads out of it still names its
# stamp in the window's copy, so the dock shows that stamp as in a library
# holding the file itself (#506); nothing outside the library changes
cat > "$tmp/save-outside.json" <<'EOF2'
{"version": 1, "start": {"blank": [20, 8]}, "steps": [
 {"name": "ink", "action": "keyboard", "at": [1, 1], "text": "ox"},
 {"name": "pick", "action": "select", "rect": [1, 1, 2, 1]},
 {"name": "save", "action": "saveToLibrary", "stamp": "Outside", "category": "out"}]}
EOF2
cat > "$tmp/save-own.json" <<'EOF2'
{"version": 1, "start": {"blank": [20, 8]}, "steps": [
 {"name": "ink", "action": "keyboard", "at": [1, 1], "text": "ow"},
 {"name": "pick", "action": "select", "rect": [1, 1, 2, 1]},
 {"name": "save", "action": "saveToLibrary", "stamp": "Own", "category": "own"}]}
EOF2
cat > "$tmp/put-outside.json" <<'EOF2'
{"version": 1, "start": {"blank": [20, 8]}, "steps": [
 {"name": "put", "action": "placeStamp", "stamp": "Outside", "at": [5, 5], "screenshot": {"of": "library"}}]}
EOF2
links=$tmp/user-lib/links
mkdir -p "$links"
"$bin" --screenshot "$tmp/save-outside.json" -o "$tmp/links-made" --user-library "$links/shared" >/dev/null 2>&1
"$bin" --screenshot "$tmp/save-own.json" -o "$tmp/links-made" --user-library "$links/own" >/dev/null 2>&1
mkdir -p "$links/relL" "$links/copyL"
cp "$links/own/"* "$links/relL/"
cp "$links/own/"* "$links/copyL/"
made=0
for f in "$links/shared/"*.ans "$links/shared/"*.json; do
  [ -f "$f" ] || continue
  ext=${f##*.}
  ln -s "../shared/${f##*/}" "$links/relL/rel.$ext"
  cp "$f" "$links/copyL/rel.$ext"
  made=$((made + 1))
done
if [ "$made" -eq 2 ]; then ok "--user-library links: the stamps are made"; else bad "--user-library links: shared holds $(ls "$links/shared" 2>&1)"; fi
(cd "$links" && ls -lR shared relL | sed 's/.*:[0-9][0-9] //') > "$tmp/links-before"
cp -R "$links/shared" "$tmp/shared-before"
out=$("$bin" --screenshot "$tmp/put-outside.json" -o "$tmp/links-rel" --user-library "$links/relL" 2>&1); st=$?
out2=$("$bin" --screenshot "$tmp/put-outside.json" -o "$tmp/links-copy" --user-library "$links/copyL" 2>&1); st2=$?
if [ "$st" -eq 0 ] && [ "$st2" -eq 0 ] && [ -f "$tmp/links-rel/01-put.png" ] && cmp -s "$tmp/links-rel/01-put.png" "$tmp/links-copy/01-put.png"; then
  ok "--screenshot: a relative link out of --user-library shows its stamp in the dock"
else
  bad "--screenshot with a relative link out of --user-library (exit $st, $st2), the dock differs from the copied files': $out $out2"
fi
(cd "$links" && ls -lR shared relL | sed 's/.*:[0-9][0-9] //') > "$tmp/links-after"
if cmp -s "$tmp/links-before" "$tmp/links-after" && diff -r "$tmp/shared-before" "$links/shared" >/dev/null; then
  ok "--screenshot: a library with links leaves what they name alone"
else
  bad "--screenshot with links changed them or what they name: $(diff "$tmp/links-before" "$tmp/links-after")"
fi

# A steps file that can't be read is an error, not a crash.
out=$("$bin" --screenshot "$tmp/missing.json" -o "$tmp/missing" 2>&1); st=$?
if [ "$st" -eq 1 ]; then ok "a missing steps file exits 1"; else bad "a missing steps file (exit $st): $out"; fi

# Usage.
for args in "--screenshot" "--screenshot $tmp/nope.json" "-o $tmp/x" "--screenshot $tmp/nope.json -o $tmp/x a.ans" \
            "--screenshot $tmp/nope.json -o $tmp/x -o $tmp/y" "--screenshot $tmp/nope.json -o $tmp/x --list-actions" \
            "--user-library $tmp/u" "--screenshot $tmp/nope.json -o $tmp/x --user-library $tmp/u --user-library $tmp/v" \
            "--screenshot $tmp/nope.json -o $tmp/x --user-library"; do
  # shellcheck disable=SC2086
  out=$("$bin" $args 2>&1); st=$?
  if [ "$st" -eq 2 ]; then ok "usage: $args"; else bad "usage: $args (exit $st): $out"; fi
done
out=$("$bin" --help 2>&1)
case $out in
  *'--screenshot'*) ok "--help lists --screenshot" ;;
  *) bad "--help has no --screenshot: $out" ;;
esac
case $out in
  *'and a screenshot'"'"'s "scale": 2 draws that picture at'*) ok "--help gives \"scale\" as a screenshot's" ;;
  *) bad "--help doesn't give \"scale\" as a screenshot's: $out" ;;
esac
case $out in
  *'"pointer": true draws a mouse pointer at the step'"'"'s'*) ok "--help gives \"pointer\" as a screenshot's" ;;
  *) bad "--help doesn't give \"pointer\" as a screenshot's: $out" ;;
esac

rm -rf "$tmp"
[ "$fails" -eq 0 ]
