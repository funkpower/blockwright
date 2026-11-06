#!/bin/sh
# blockwright --screenshot (#295) with no display: app_screenshot_test.sh <blockwright> <test data> <scratch dir>
# The pictures a steps file asks for are written, one PNG each, the size of
# what they show; a dock the window doesn't have, a tool it doesn't have or
# a Tool Option out of range names the step and writes nothing; usage errors
# exit 2. POSIX sh and tools only.
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

# A steps file that can't be read is an error, not a crash.
out=$("$bin" --screenshot "$tmp/missing.json" -o "$tmp/missing" 2>&1); st=$?
if [ "$st" -eq 1 ]; then ok "a missing steps file exits 1"; else bad "a missing steps file (exit $st): $out"; fi

# Usage.
for args in "--screenshot" "--screenshot $tmp/nope.json" "-o $tmp/x" "--screenshot $tmp/nope.json -o $tmp/x a.ans" \
            "--screenshot $tmp/nope.json -o $tmp/x -o $tmp/y" "--screenshot $tmp/nope.json -o $tmp/x --list-actions"; do
  # shellcheck disable=SC2086
  out=$("$bin" $args 2>&1); st=$?
  if [ "$st" -eq 2 ]; then ok "usage: $args"; else bad "usage: $args (exit $st): $out"; fi
done
out=$("$bin" --help 2>&1)
case $out in
  *'--screenshot'*) ok "--help lists --screenshot" ;;
  *) bad "--help has no --screenshot: $out" ;;
esac

rm -rf "$tmp"
[ "$fails" -eq 0 ]
