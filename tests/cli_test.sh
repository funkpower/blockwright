#!/usr/bin/env bash
# CLI tests: cli_test.sh <blockwright-cli> <tests/data> <scratch dir>
set -uo pipefail
cli=$1 data=$2 tmp=$3
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

piece=$data/golden/tux.ans

expect "--version prints version and short commit" 0 '^blockwright-cli [0-9]+\.[0-9]+\.[0-9]+ \(([0-9a-f]{7,}(-dirty)?|unknown)\)$' -- "$cli" --version
expect "--help" 0 'convert <in> <out>' -- "$cli" --help
expect "--help lists export" 0 'export --irc <in> <out>' -- "$cli" --help

expect "info: dimensions" 0 '^size: +30x17$' -- "$cli" info "$piece"
expect "info: SAUCE author" 0 '^author: +funkpower$' -- "$cli" info "$piece"
expect "info: FileSize checked" 0 '^filesize: +1948 \(matches content\)$' -- "$cli" info "$piece"
expect "info: flags" 0 '^flags: +0x1 \(iCE on' -- "$cli" info "$piece"

expect "convert" 0 '^$' -- "$cli" convert "$piece" "$tmp/tux.ans"
if cmp -s "$piece" "$tmp/tux.ans"; then ok "convert output is byte-identical"; else bad "convert output differs"; fi

# sauce <file> <iCE 0|1>: append a SAUCE record (80 wide, 1 high) to a body under 256 bytes
sauce() {
  local size; size=$(stat -c %s "$1")
  {
    printf '\x1aSAUCE00%-35s%-20s%-20s20261021' '' '' ''
    printf "\\x$(printf %02x "$size")\\x00\\x00\\x00"
    printf '\x01\x01\x50\x00\x01\x00\x00\x00\x00\x00\x00'
    printf "\\x0$2"
    printf '%022d' 0 | tr 0 '\0'
  } >> "$1"
}
printf '\033[5;41mA\033[0;104mB' > "$tmp/lossy.ans"; sauce "$tmp/lossy.ans" 0
printf '\033[5;41mA' > "$tmp/blink.ans"; sauce "$tmp/blink.ans" 0
expect "test SAUCE reads as iCE off" 0 '^flags: +0x0 \(iCE off' -- "$cli" info "$tmp/lossy.ans"
expect "lossy convert refused" 1 'warning: .*lossy\.ans: 1 blinking cell loses its blink' -- "$cli" convert "$tmp/lossy.ans" "$tmp/lossy-out.ans"
expect "lossy convert says why" 1 'lossy-out\.ans: not written: the conversion loses data \(--allow-lossy writes it anyway\)' -- "$cli" convert "$tmp/lossy.ans" "$tmp/lossy-out.ans"
if [[ -e $tmp/lossy-out.ans ]]; then bad "refused lossy convert left an output file"; else ok "refused lossy convert writes nothing"; fi
expect "--allow-lossy writes" 0 'warning: .*lossy\.ans: 1 blinking cell loses its blink' -- "$cli" convert --allow-lossy "$tmp/lossy.ans" "$tmp/lossy-out.ans"
expect "--allow-lossy output is iCE on" 0 '^flags: +0x1 \(iCE on' -- "$cli" info "$tmp/lossy-out.ans"
expect "blink alone is not lossy" 0 '^$' -- "$cli" convert "$tmp/blink.ans" "$tmp/blink-out.ans"
expect "blink alone stays iCE off" 0 '^flags: +0x0 \(iCE off' -- "$cli" info "$tmp/blink-out.ans"
# #42: blink on a space never shows with iCE off, so iCE on loses nothing
printf '\033[0;5;40m   \033[0;37;104mB\r\n\033[0;5;40m    ' > "$tmp/blank-blink.ans"; sauce "$tmp/blank-blink.ans" 0
expect "blink on blank glyphs is not lossy" 0 '^$' -- "$cli" convert "$tmp/blank-blink.ans" "$tmp/blank-blink-out.ans"
expect "blink on blank glyphs: output is iCE on" 0 '^flags: +0x1 \(iCE on' -- "$cli" info "$tmp/blank-blink-out.ans"

: > "$tmp/empty.ans"
printf '\033[0m\r\n' > "$tmp/nothing.ans"
mkdir -p "$tmp/dir.ans"
expect "missing file" 1 'nope\.ans: No such file' -- "$cli" info "$tmp/nope.ans"
expect "empty file" 1 'empty\.ans: empty file' -- "$cli" info "$tmp/empty.ans"
expect "nothing drawn" 1 'nothing\.ans: no ANSI content' -- "$cli" convert "$tmp/nothing.ans" "$tmp/x.ans"
expect "directory" 1 'dir\.ans: is a directory' -- "$cli" info "$tmp/dir.ans"
expect "unwritable output" 1 'no/such/dir/out\.ans' -- "$cli" convert "$piece" "$tmp/no/such/dir/out.ans"
# A write that fails partway (the file size limit stops it) leaves no partial
# output: no new file, an existing one untouched, and no temporary file.
small() { (trap '' XFSZ; ulimit -f 1; "$@"); }  # 1 KiB limit; tux.ans is 2 KiB
fsz=$tmp/fsz && mkdir -p "$fsz"
expect "short write fails" 1 'fsz/fresh\.ans: write failed: File too large' -- small "$cli" convert "$piece" "$fsz/fresh.ans"
if [[ -e $fsz/fresh.ans ]]; then bad "short write left a partial file"; else ok "short write leaves no file"; fi
printf 'old contents\n' > "$fsz/existing.ans"
expect "short write over a file fails" 1 'existing\.ans' -- small "$cli" convert "$piece" "$fsz/existing.ans"
if [[ $(<"$fsz/existing.ans") == "old contents" ]]; then ok "short write leaves the existing file"; else bad "short write changed the existing file"; fi
cp "$piece" "$fsz/inplace.ans"
expect "short in-place convert fails" 1 'inplace\.ans' -- small "$cli" convert "$fsz/inplace.ans" "$fsz/inplace.ans"
if cmp -s "$piece" "$fsz/inplace.ans"; then ok "short in-place convert leaves the source"; else bad "short in-place convert damaged the source"; fi
left=$(ls -A "$fsz" | grep -vxE 'existing\.ans|inplace\.ans')
if [[ -z $left ]]; then ok "short writes leave no temporary file"; else bad "short writes left: $left"; fi
cp "$piece" "$fsz/ok.ans"
expect "in-place convert" 0 '^$' -- "$cli" convert "$fsz/ok.ans" "$fsz/ok.ans"
if cmp -s "$piece" "$fsz/ok.ans"; then ok "in-place convert output is byte-identical"; else bad "in-place convert output differs"; fi
# convert may name its input as its output (export and render refuse): a
# non-canonical piece rewritten in place is what convert writes elsewhere
cp "$data/golden/darkmage.ans" "$tmp/darkmage.ans"
expect "convert a non-canonical piece" 0 '^$' -- "$cli" convert "$tmp/darkmage.ans" "$tmp/darkmage-out.ans"
if ! cmp -s "$data/golden/darkmage.ans" "$tmp/darkmage-out.ans"; then ok "the non-canonical piece is rewritten"; else bad "darkmage.ans came out unchanged: not a normalising test"; fi
expect "in-place convert of a non-canonical piece" 0 '^$' -- "$cli" convert "$tmp/darkmage.ans" "$tmp/darkmage.ans"
if cmp -s "$tmp/darkmage-out.ans" "$tmp/darkmage.ans"; then ok "in-place convert writes what convert writes elsewhere"; else bad "in-place convert differs from convert to another file"; fi
expect "in-place convert again" 0 '^$' -- "$cli" convert "$tmp/darkmage.ans" "$tmp/darkmage.ans"
if cmp -s "$tmp/darkmage-out.ans" "$tmp/darkmage.ans"; then ok "a second in-place convert changes nothing"; else bad "a second in-place convert changed the file"; fi
expect "--help says convert may rewrite in place" 0 '<out> may be' -- "$cli" --help
expect "--help says export and render refuse" 0 'render refuse to write over their input' -- "$cli" --help

# export --irc: per-line byte report, the budget, and the override
expect "export --irc" 0 '^17 lines, longest 80 bytes, budget 407: 0 over$' -- "$cli" export --irc "$piece" "$tmp/tux.txt"
if cmp -s "$data/golden-irc/tux.txt" "$tmp/tux.txt"; then ok "export --irc output is golden-irc's"; else bad "export --irc output differs from golden-irc/tux.txt"; fi
expect "export --irc reports every line" 0 '^ +13 +80$' -- "$cli" export --irc "$piece" "$tmp/tux.txt"
report=$("$cli" export --irc "$piece" "$tmp/tux.txt" 2>/dev/null)
if [[ $(grep -cE '^ +[0-9]+ +[0-9]+' <<<"$report") -eq 17 ]]; then ok "export --irc reports 17 lines"; else bad "export --irc report: $report"; fi
expect "over budget refused" 3 '^ +13 +80  over by 20$' -- "$cli" export --irc --budget 60 "$piece" "$tmp/over.txt"
expect "over budget listed" 3 'tux\.ans: line 13 is 80 bytes, over the 60-byte budget by 20' -- "$cli" export --irc --budget 60 "$piece" "$tmp/over.txt"
expect "over budget counted" 3 '^17 lines, longest 80 bytes, budget 60: 6 over$' -- "$cli" export --irc --budget 60 "$piece" "$tmp/over.txt"
expect "over budget says why" 3 'over\.txt: not written: lines over the budget \(--allow-over-budget writes it anyway\)' -- "$cli" export --irc --budget 60 "$piece" "$tmp/over.txt"
if [[ -e $tmp/over.txt ]]; then bad "refused export left an output file"; else ok "refused export writes nothing"; fi
expect "--allow-over-budget writes" 0 'line 13 is 80 bytes, over the 60-byte budget' -- "$cli" export --irc --budget 60 --allow-over-budget "$piece" "$tmp/over.txt"
if cmp -s "$data/golden-irc/tux.txt" "$tmp/over.txt"; then ok "--allow-over-budget writes the lines uncut"; else bad "--allow-over-budget output differs"; fi
# 80 cells, each a different colour from its neighbour: over 407 bytes
for i in $(seq 0 79); do printf '\033[%d;%dmx' $((30 + i % 8)) $((40 + (i + 3) % 8)); done > "$tmp/rainbow.ans"
expect "default budget is 407" 3 '^1 line, longest 4[0-9][0-9] bytes, budget 407: 1 over$' -- "$cli" export --irc "$tmp/rainbow.ans" "$tmp/rainbow.txt"
expect "--budget raises it" 0 'budget 600: 0 over$' -- "$cli" export --irc --budget 600 "$tmp/rainbow.ans" "$tmp/rainbow.txt"
expect "export needs --irc" 2 'export needs a format: --irc' -- "$cli" export "$piece" "$tmp/x.txt"
expect "export arity" 2 'export takes' -- "$cli" export --irc "$piece"
expect "--budget must be a number" 2 '--budget takes a positive number' -- "$cli" export --irc --budget lots "$piece" "$tmp/x.txt"
expect "--budget must be positive" 2 '--budget takes a positive number' -- "$cli" export --irc --budget 0 "$piece" "$tmp/x.txt"
# export never writes over its input, whatever name the output gives it
same=$tmp/same && mkdir -p "$same/sub"
cp "$piece" "$same/a.ans"
ln -s a.ans "$same/out-link.ans"   # the output is a symlink to the input
cp "$piece" "$same/real.ans"
ln -s real.ans "$same/in-link.ans" # the input is a symlink to the output
ln "$same/a.ans" "$same/hard.ans"  # a hard link to the input
expect "export onto the input refused" 1 'a\.ans: not written: it is the input file' -- "$cli" export --irc "$same/a.ans" "$same/a.ans"
expect "export onto ./input refused" 1 'not written: it is the input file' -- "$cli" export --irc "$same/a.ans" "$same/./a.ans"
expect "export onto sub/../input refused" 1 'not written: it is the input file' -- "$cli" export --irc "$same/a.ans" "$same/sub/../a.ans"
expect "export relative onto the input refused" 1 'not written: it is the input file' -- env -C "$same/sub" "$cli" export --irc ../a.ans ./../a.ans
expect "export onto a symlink to the input refused" 1 'out-link\.ans: not written: it is the input file' -- "$cli" export --irc "$same/a.ans" "$same/out-link.ans"
expect "export from a symlink onto its target refused" 1 'real\.ans: not written: it is the input file' -- "$cli" export --irc "$same/in-link.ans" "$same/real.ans"
expect "export onto a hard link to the input refused" 1 'hard\.ans: not written: it is the input file' -- "$cli" export --irc "$same/a.ans" "$same/hard.ans"
out=$("$cli" export --irc "$same/a.ans" "$same/a.ans" 2>&1)
if [[ $out == "blockwright-cli: $same/a.ans: not written: it is the input file" ]]; then ok "export onto the input prints only the refusal"; else bad "export onto the input printed: $out"; fi
: > "$same/empty.ans"  # refused before the input is read
expect "export onto the input refused before reading it" 1 'empty\.ans: not written: it is the input file' -- "$cli" export --irc "$same/empty.ans" "$same/empty.ans"
rm "$same/empty.ans"
if cmp -s "$piece" "$same/a.ans" && cmp -s "$piece" "$same/real.ans"; then ok "refused export leaves the input"; else bad "refused export changed the input"; fi
if [[ -L $same/out-link.ans && -L $same/in-link.ans ]]; then ok "refused export leaves the symlinks"; else bad "refused export replaced a symlink"; fi
left=$(ls -A "$same" | grep -vxE 'a\.ans|out-link\.ans|real\.ans|in-link\.ans|hard\.ans|sub')
if [[ -z $left && -z $(ls -A "$same/sub") ]]; then ok "refused export leaves no stray file"; else bad "refused export left: $left $(ls -A "$same/sub")"; fi
expect "export beside the input still writes" 0 '^17 lines' -- "$cli" export --irc "$same/a.ans" "$same/a.txt"
if cmp -s "$data/golden-irc/tux.txt" "$same/a.txt"; then ok "export beside the input writes the lines"; else bad "export beside the input differs"; fi

# shade: one pass of the ramp shade brush, then the ladder check
sphere=$data/brushes/sphere-steel.ans
expect "--help lists shade" 0 'shade --ramp R <in> <out>' -- "$cli" --help
expect "shade" 0 '^lightened 2411 cells 1 rung along steel in 0,0-71,33$' -- "$cli" shade --ramp steel "$sphere" "$tmp/sphere-up.ans"
expect "shade checks the ladder" 0 '^ladder: 2448 of 2448 cells on the ramp, [0-9]+ shaded, noise 0\.000: ' -- "$cli" shade --ramp steel "$sphere" "$tmp/sphere-up.ans"
expect "shade --darken all the way" 0 '^ladder: 2448 of 2448 cells on the ramp, 0 shaded, noise 0\.000: clean$' -- \
  "$cli" shade --ramp steel --darken --steps 12 "$sphere" "$tmp/sphere-black.ans"
expect "shade by palette indices" 0 '^darkened 1661 cells 12 rungs along 0,8,7,15 ' -- \
  "$cli" shade --ramp 0,8,7,15 --darken --steps 12 "$sphere" "$tmp/sphere-black2.ans"
if cmp -s "$tmp/sphere-black.ans" "$tmp/sphere-black2.ans"; then ok "a ramp by name and by indices shade alike"; else bad "steel and 0,8,7,15 shade differently"; fi
expect "shade a box" 0 '^lightened 16 cells 1 rung along steel in 0,0-3,3$' -- "$cli" shade --ramp steel --box 3,3,0,0 "$sphere" "$tmp/corner.ans"
# A flat field of K░k: no gradient, so no wide rung, but one glyph only.
expect "shade reports findings" 0 '^ladder: 16 of 16 cells on the ramp, 16 shaded, noise 0\.000: 1 finding$' -- \
  "$cli" shade --ramp steel --box 0,0,3,3 "$sphere" "$tmp/corner.ans"
expect "shade lists findings" 0 '^  the shading uses one glyph only$' -- "$cli" shade --ramp steel --box 0,0,3,3 "$sphere" "$tmp/corner.ans"
expect "shade a box clipped to the canvas" 0 ' in 50,20-71,33$' -- "$cli" shade --ramp steel --box 50,20,90,90 "$sphere" "$tmp/edge.ans"
expect "shade the box back down" 0 '^darkened 16 cells' -- "$cli" shade --ramp steel --darken --box 0,0,3,3 "$tmp/corner.ans" "$tmp/corner-back.ans"
if cmp -s "$sphere" "$tmp/corner-back.ans"; then ok "shade changes only the box"; else bad "lightening and darkening a black box changed the file"; fi
command cp -f "$sphere" "$tmp/inplace-shade.ans"
expect "shade in place" 0 '^lightened' -- "$cli" shade --ramp steel "$tmp/inplace-shade.ans" "$tmp/inplace-shade.ans"
if cmp -s "$tmp/sphere-up.ans" "$tmp/inplace-shade.ans"; then ok "shade in place writes what shade writes elsewhere"; else bad "shade in place differs"; fi
expect "shade with nothing on the ramp" 0 '^lightened 0 cells 1 rung along 1,2 ' -- "$cli" shade --ramp 1,2 "$sphere" "$tmp/none.ans"
expect "lossy shade refused" 1 'lossy-shade\.ans: not written: the conversion loses data' -- "$cli" shade --ramp fire "$tmp/lossy.ans" "$tmp/lossy-shade.ans"
if [[ -e $tmp/lossy-shade.ans ]]; then bad "refused lossy shade left an output file"; else ok "refused lossy shade writes nothing"; fi
expect "shade --allow-lossy writes" 0 '^lightened' -- "$cli" shade --ramp fire --allow-lossy "$tmp/lossy.ans" "$tmp/lossy-shade.ans"
# #42: blink on a blank glyph never showed, so opening it with iCE on loses
# nothing and shade writes it without --allow-lossy, as convert does
printf '\033[0;5;40m \033[0;37;104mB' > "$tmp/blank-blink-1.ans"; sauce "$tmp/blank-blink-1.ans" 0
expect "shade of blink on a blank glyph is not refused" 0 '^lightened' -- "$cli" shade --ramp fire "$tmp/blank-blink-1.ans" "$tmp/blank-blink-shade.ans"
if grep -q 'blinking cell' <<<"$("$cli" shade --ramp fire "$tmp/blank-blink-1.ans" "$tmp/blank-blink-shade.ans" 2>&1)"; then
  bad "shade of blink on a blank glyph warns of a blinking cell"; else ok "shade of blink on a blank glyph warns of no blinking cell"; fi
# A non-iCE piece with a blinking cell: steel's K/w shades keep a dark
# background, so the blink survives. Two bright colours need iCE.
printf '\033[5;31mA\033[0;1;30;47m\xb2\xb2\xb2' > "$tmp/nonice.ans"; sauce "$tmp/nonice.ans" 0
expect "shade a non-iCE piece over steel" 0 '^lightened 3 cells 1 rung along steel in 1,0-3,0$' -- "$cli" shade --ramp steel --box 1,0,3,0 "$tmp/nonice.ans" "$tmp/nonice-up.ans"
expect "shade keeps iCE off" 0 '^flags: +0x0 \(iCE off' -- "$cli" info "$tmp/nonice-up.ans"
expect "shade keeps the blink" 0 '^lightened 3 cells' -- "$cli" shade --ramp steel --box 1,0,3,0 "$tmp/nonice-up.ans" "$tmp/nonice-up2.ans"
# Past LightRed, fire's next rung is LightRed/Yellow, a bright background
# either way round: without iCE the brush stops short of it and the piece
# stays non-iCE, as the editor's ramp brush does; --ice turns iCE on.
printf '\033[0;1;31m\xdb\xdb' > "$tmp/nonice-pair.ans"; sauce "$tmp/nonice-pair.ans" 0
expect "shade without iCE stops short of a bright background" 0 '^lightened 0 cells 1 rung along fire in 0,0-1,0$' -- \
  "$cli" shade --ramp fire --box 0,0,1,0 "$tmp/nonice-pair.ans" "$tmp/nonice-pair-up.ans"
expect "shade without iCE keeps iCE off" 0 '^flags: +0x0 \(iCE off' -- "$cli" info "$tmp/nonice-pair-up.ans"
if "$cli" shade --ramp fire --box 0,0,1,0 "$tmp/nonice-pair.ans" "$tmp/nonice-pair-up.ans" 2>&1 | grep -q 'iCE'; then
  bad "shade without iCE mentions iCE"; else ok "shade without iCE says nothing about iCE"; fi
expect "shade --ice notes iCE on" 0 'note: .*nonice-pair-ice\.ans: written with iCE colours on; .*nonice-pair\.ans had them off' -- \
  "$cli" shade --ramp fire --box 0,0,1,0 --ice "$tmp/nonice-pair.ans" "$tmp/nonice-pair-ice.ans"
expect "shade --ice shades past it" 0 '^lightened 2 cells 1 rung along fire' -- \
  "$cli" shade --ramp fire --box 0,0,1,0 --ice "$tmp/nonice-pair.ans" "$tmp/nonice-pair-ice.ans"
expect "shade --ice writes iCE on" 0 '^flags: +0x1 \(iCE on' -- "$cli" info "$tmp/nonice-pair-ice.ans"
if grep -q $'\033\\[0;1;5;33;41m' "$tmp/nonice-pair-ice.ans"; then ok "shade --ice writes rung 9"; else bad "shade --ice didn't write ESC[0;1;5;33;41m"; fi
printf '\033[5;31mA\033[0;1;31m\xdb' > "$tmp/nonice-fire.ans"; sauce "$tmp/nonice-fire.ans" 0
expect "shade without iCE keeps a blinking piece" 0 '^lightened 0 cells' -- "$cli" shade --ramp fire --box 1,0,1,0 "$tmp/nonice-fire.ans" "$tmp/nonice-fire-up.ans"
expect "shade without iCE keeps a blinking piece iCE off" 0 '^flags: +0x0 \(iCE off' -- "$cli" info "$tmp/nonice-fire-up.ans"
if grep -q $'\033\\[0;5;31;40mA' "$tmp/nonice-fire-up.ans"; then ok "shade without iCE keeps the blink"; else bad "shade without iCE lost the blink"; fi
expect "shade --ice that drops a blink refused" 1 'nonice-fire-ice\.ans: not written: --ice turns iCE colours on, and 1 blinking cell would lose its blink \(--allow-lossy writes it anyway\)' -- \
  "$cli" shade --ramp fire --box 1,0,1,0 --ice "$tmp/nonice-fire.ans" "$tmp/nonice-fire-ice.ans"
if [[ -e $tmp/nonice-fire-ice.ans ]]; then bad "refused iCE shade left an output file"; else ok "refused iCE shade writes nothing"; fi
expect "shade --ice --allow-lossy writes" 0 'note: .*written with iCE colours on' -- \
  "$cli" shade --ramp fire --box 1,0,1,0 --ice --allow-lossy "$tmp/nonice-fire.ans" "$tmp/nonice-fire-ice.ans"
expect "shade --ice --allow-lossy writes iCE on" 0 '^flags: +0x1 \(iCE on' -- "$cli" info "$tmp/nonice-fire-ice.ans"
# A piece with iCE off that holds a bright background opens with iCE on, as
# the editor opens it: its blinking red block loses the blink there, so it
# is on the ramp, and that loss is counted at the save.
printf '\033[0;5;31;40m\xdb\033[0;104mA' > "$tmp/inferred-blink.ans"; sauce "$tmp/inferred-blink.ans" 0
expect "shade of an inferred-iCE piece refuses the blink lost on open" 1 'inferred-blink-up\.ans: not written: the conversion loses data \(--allow-lossy writes it anyway\)' -- \
  "$cli" shade --ramp fire --box 0,0,0,0 "$tmp/inferred-blink.ans" "$tmp/inferred-blink-up.ans"
expect "shade of an inferred-iCE piece shades its blinking block" 0 '^lightened 1 cell 1 rung along fire in 0,0-0,0$' -- \
  "$cli" shade --ramp fire --box 0,0,0,0 --allow-lossy "$tmp/inferred-blink.ans" "$tmp/inferred-blink-up.ans"
# Darkening its only bright background leaves none, and iCE stays on as the
# editor saves it, so two passes lightening reach rung 9 (LightRed/Yellow).
printf '\033[101m ' > "$tmp/inferred.ans"; sauce "$tmp/inferred.ans" 0
expect "shade darkens an inferred-iCE piece's last bright background" 0 '^darkened 1 cell 1 rung along fire in 0,0-0,0$' -- \
  "$cli" shade --ramp fire --darken --box 0,0,0,0 "$tmp/inferred.ans" "$tmp/inferred-down.ans"
expect "shade keeps inferred iCE on" 0 '^flags: +0x1 \(iCE on' -- "$cli" info "$tmp/inferred-down.ans"
"$cli" shade --ramp fire --box 0,0,0,0 "$tmp/inferred-down.ans" "$tmp/inferred-up.ans" > /dev/null 2>&1
"$cli" shade --ramp fire --box 0,0,0,0 "$tmp/inferred-up.ans" "$tmp/inferred-up2.ans" > /dev/null 2>&1
if grep -q $'\033\\[0;1;5;33;41m' "$tmp/inferred-up2.ans"; then ok "shade of inferred iCE lightens to rung 9"; else bad "shade of inferred iCE didn't reach rung 9 (ESC[0;1;5;33;41m)"; fi
expect "shade needs a ramp" 2 'shade needs a ramp' -- "$cli" shade "$sphere" "$tmp/x.ans"
expect "shade unknown ramp" 2 "--ramp takes a ramp name \(fire, .*\) or palette indices 0-15 dark to light, not 'blood'" -- \
  "$cli" shade --ramp blood "$sphere" "$tmp/x.ans"
expect "shade ramp out of the palette" 2 "not '0,16'" -- "$cli" shade --ramp 0,16 "$sphere" "$tmp/x.ans"
expect "shade ramp of one colour" 2 '--ramp 7: a ramp needs at least two colours' -- "$cli" shade --ramp 7 "$sphere" "$tmp/x.ans"
expect "shade ramp with a repeat" 2 '--ramp 0,8,0: colour 0 appears twice' -- "$cli" shade --ramp 0,8,0 "$sphere" "$tmp/x.ans"
expect "shade bad box" 2 "--box takes x0,y0,x1,y1 .*not '1,2,3'" -- "$cli" shade --ramp steel --box 1,2,3 "$sphere" "$tmp/x.ans"
expect "shade negative box" 2 "not '-1,0,3,3'" -- "$cli" shade --ramp steel --box=-1,0,3,3 "$sphere" "$tmp/x.ans"
expect "shade box off the canvas" 1 'the box 72,0,80,3 is off the 72x34 canvas' -- "$cli" shade --ramp steel --box 72,0,80,3 "$sphere" "$tmp/x.ans"
expect "shade bad steps" 2 "--steps must be 1-64, not '0'" -- "$cli" shade --ramp steel --steps 0 "$sphere" "$tmp/x.ans"
expect "shade arity" 2 'shade takes an input and an output file' -- "$cli" shade --ramp steel "$sphere"
expect "shade bad input" 1 'nope\.ans: No such file' -- "$cli" shade --ramp steel "$tmp/nope.ans" "$tmp/x.ans"
if [[ -e $tmp/x.ans ]]; then bad "a refused shade wrote $tmp/x.ans"; else ok "refused shades write nothing"; fi

# Every command refuses every option it doesn't use: exit 2, a usage message
# naming the option and the command, and nothing written.
declare -A owner=([--allow-lossy]="convert shade" [--scale]=render [--font-width]=render
                  [--irc]=export [--budget]=export [--allow-over-budget]=export
                  [--ramp]=shade [--darken]=shade [--steps]=shade [--box]=shade [--ice]=shade)
declare -A value=([--scale]=2 [--font-width]=9 [--budget]=60 [--ramp]=steel [--steps]=2 [--box]=0,0,1,1)
foreign=$tmp/foreign && mkdir -p "$foreign"
for cmd in info convert render export shade; do
  for opt in --allow-lossy --scale --font-width --irc --budget --allow-over-budget --ramp --darken --steps --box --ice; do
    [[ " ${owner[$opt]} " == *" $cmd "* ]] && continue
    out=$foreign/$cmd${opt}.out
    case $cmd in
      info) argv=(info "$piece") ;;
      export) argv=(export --irc "$piece" "$out") ;;
      shade) argv=(shade --ramp steel "$piece" "$out") ;;
      *) argv=("$cmd" "$piece" "$out") ;;
    esac
    a=${owner[$opt]// / or }; [[ $a == export ]] && a="an $a" || a="a $a"
    expect "$cmd rejects $opt" 2 "^blockwright-cli: $opt is $a option, not a $cmd option$" -- \
      "$cli" "${argv[@]:0:1}" "$opt" ${value[$opt]:+"${value[$opt]}"} "${argv[@]:1}"
    if [[ -e $out ]]; then bad "$cmd with $opt wrote $out"; fi
  done
done
out=$("$cli" info --scale 2 "$piece" 2>/dev/null)
if [[ -z $out ]]; then ok "info with a foreign option prints no info"; else bad "info with a foreign option printed: $out"; fi
if [[ -z $(ls -A "$foreign") ]]; then ok "commands with a foreign option write nothing"; else bad "foreign options wrote: $(ls -A "$foreign")"; fi
expect "options after the files are checked too" 2 '^blockwright-cli: --allow-lossy is a convert or shade option, not a render option$' -- \
  "$cli" render "$piece" "$foreign/late.png" --allow-lossy

expect "export bad input" 1 'nope\.ans: No such file' -- "$cli" export --irc "$tmp/nope.ans" "$tmp/x.txt"
expect "export unwritable output" 1 'no/such/dir/out\.txt' -- "$cli" export --irc "$piece" "$tmp/no/such/dir/out.txt"

# Out of memory is a one-line error and exit 1, not std::terminate: a 4 GiB
# sparse input read under a 512 MiB address-space limit
truncate -s 4G "$tmp/huge.ans"
expect "out of memory" 1 '^blockwright-cli: out of memory$' -- bash -c 'ulimit -v 524288 && exec "$@"' _ "$cli" info "$tmp/huge.ans"
out=$(bash -c 'ulimit -v 524288 && exec "$@"' _ "$cli" info "$tmp/huge.ans" 2>&1)
if [[ $(wc -l <<<"$out") -eq 1 ]]; then ok "out of memory prints one line"; else bad "out of memory printed: $out"; fi
rm -f "$tmp/huge.ans"

expect "no command" 2 'no command' -- "$cli"
expect "unknown command" 2 "unknown command 'frobnicate'" -- "$cli" frobnicate
expect "convert arity" 2 'convert takes' -- "$cli" convert "$piece"
expect "info arity" 2 'info takes' -- "$cli" info
expect "unknown option" 2 'Unknown option' -- "$cli" --bogus

# library list / check
lib=$data/library
expect "library check: a good library" 0 '^2 stamp\(s\), 0 problem\(s\)$' -- "$cli" library check "$lib"
expect "library list: tab-separated" 0 $'^sky\tSmall star\t5x3\t16\tstar,night\t.*/sky/small-star\\.ans$' -- "$cli" library list "$lib"
expect "library list: sorted by path" 0 $'^figures\tTux\t30x17\t80\tpenguin,linux\t' -- bash -c '"$1" library list "$2" | head -1' _ "$cli" "$lib"
expect "library list: no dir lists the bundled and user libraries" 0 $'^sky\tSmall star\t' -- env BLOCKWRIGHT_BUNDLED_LIBRARY="$lib" BLOCKWRIGHT_USER_LIBRARY="$tmp/no-user-lib" "$cli" library list
expect "library check: missing dir" 1 'nope: not a directory' -- "$cli" library check "$tmp/nope"
expect "library check: needs a dir" 2 'library check takes one directory' -- "$cli" library check
expect "library: needs a subcommand" 2 'library needs a subcommand' -- "$cli" library
expect "library: unknown subcommand" 2 "unknown library subcommand 'frob'" -- "$cli" library frob
expect "library: refuses other commands' options" 2 '--scale is a render option, not a library option' -- "$cli" library list --scale 2
bl=$tmp/badlib; mkdir -p "$bl"
cp "$lib/sky/small-star.ans" "$bl/bytes.ans"; sed 's/"ircBytes": 16/"ircBytes": 17/' "$lib/sky/small-star.json" > "$bl/bytes.json"
cp "$lib/sky/small-star.ans" "$bl/key.ans"; sed 's/"tags"/"tag"/' "$lib/sky/small-star.json" > "$bl/key.json"
cp "$lib/sky/small-star.ans" "$bl/nosidecar.ans"
cp "$lib/sky/small-star.json" "$bl/orphan.json"
printf '\033[0;33mx\033[0m\r\n' > "$bl/nosauce.ans"; cp "$lib/sky/small-star.json" "$bl/nosauce.json"
expect "library check: wrong byte count" 1 'bytes\.ans: sidecar says the widest row is 17 IRC bytes; the art.s is 16' -- "$cli" library check "$bl"
expect "library check: unknown key" 1 'key\.ans: sidecar has an unknown key "tag"' -- "$cli" library check "$bl"
expect "library check: no sidecar" 1 'nosidecar\.ans: no sidecar nosidecar\.json' -- "$cli" library check "$bl"
expect "library check: orphan sidecar" 1 'orphan\.json: sidecar with no orphan\.ans' -- "$cli" library check "$bl"
expect "library check: no SAUCE" 1 'nosauce\.ans: \.ans has no SAUCE record' -- "$cli" library check "$bl"
sed 's/"night"/"night,day"/' "$lib/sky/small-star.json" > "$bl/comma.json"; cp "$lib/sky/small-star.ans" "$bl/comma.ans"
# blank.ans: three blank cells under a SAUCE titled "Blank", 3 wide
{
  printf '\033[0m   \033[0m\x1aSAUCE00%-35s%-20s%-20s20261023' 'Blank' '' ''
  printf '\x0c\x00\x00\x00\x01\x01\x03\x00\x01\x00\x00\x00\x00\x00\x00\x01'
  printf '%022d' 0 | tr 0 '\0'
} > "$bl/blank.ans"
printf '{"category": "x", "tags": [], "ircBytes": 0}\n' > "$bl/blank.json"
mkdir -p "$tmp/shared"; ln -s "$tmp/shared" "$bl/linked"; ln -s "$bl/gone.ans" "$bl/dangling.ans"
expect "library check: a comma in a tag" 1 'comma\.ans: a sidecar tag holds a comma' -- "$cli" library check "$bl"
expect "library check: an all-blank stamp" 1 'blank\.ans: \.ans has no drawn cell' -- "$cli" library check "$bl"
expect "library check: a linked folder" 1 'linked: symbolic link to a folder, not followed' -- "$cli" library check "$bl"
expect "library check: a broken link" 1 'dangling\.ans: broken symbolic link, skipped' -- "$cli" library check "$bl"
expect "library list: a linked folder is a warning" 0 'warning: .*linked: symbolic link to a folder' -- "$cli" library list "$bl"
expect "library check: counts" 1 '^0 stamp\(s\), 9 problem\(s\)$' -- "$cli" library check "$bl"
expect "library list: problems are warnings" 0 'warning: .*orphan\.json: sidecar with no orphan\.ans' -- "$cli" library list "$bl"
b2=$tmp/badlib2; mkdir -p "$b2"
cp "$lib/sky/small-star.ans" "$b2/dup.ans"
printf '{"category":7,"category":"sky","tags":[],"ircBytes":9999,"ircBytes":16}\n' > "$b2/dup.json"
mkfifo "$b2/pipe.ans"
expect "library check: a repeated key" 1 'dup\.ans: sidecar has the key "category" more than once' -- timeout 10 "$cli" library check "$b2"
expect "library check: a FIFO is never opened" 1 'pipe\.ans: not a regular file, skipped' -- timeout 10 "$cli" library check "$b2"
if [[ $(id -u) -ne 0 ]]; then
  b3=$tmp/badlib3; mkdir -p "$b3/locked"; cp "$lib/sky/small-star."* "$b3/"
  chmod 000 "$b3/locked"
  expect "library check: an unreadable folder" 1 'locked: folder can.t be read' -- "$cli" library check "$b3"
  expect "library list: an unreadable folder is a warning" 0 'warning: .*locked: folder can.t be read' -- "$cli" library list "$b3"
  chmod 700 "$b3/locked"
else
  echo "skip library check: an unreadable folder (running as root)"
fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
