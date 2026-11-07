#!/usr/bin/env bash
# CLI render tests: cli_render_test.sh <blockwright-cli> <tests/data> <scratch dir>
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

# png_size <file>: "WxH" from the IHDR chunk, or nothing if it isn't a PNG
png_size() {
  [[ $(od -An -tx1 -N8 "$1" | tr -d ' \n') == 89504e470d0a1a0a ]] || return 1
  local -a b
  read -ra b <<<"$(od -An -tu1 -j16 -N8 "$1")"
  echo "$(( (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3] ))x$(( (b[4] << 24) | (b[5] << 16) | (b[6] << 8) | b[7] ))"
}

# size_is <name> <file> <WxH>
size_is() {
  local got; got=$(png_size "$2")
  if [[ $got == "$3" ]]; then ok "$1"; else bad "$1: got '${got:-not a PNG}', wanted $3"; fi
}

piece=$data/golden/tux.ans  # 30x17, SAUCE 8 px

expect "--help lists render" 0 'render <in> <out>' -- "$cli" --help
expect "--help says render refuses a .bwp" 0 'IBM VGA font; a \.bwp$' -- "$cli" --help
expect "render" 0 '^$' -- "$cli" render "$piece" "$tmp/tux.png"
size_is "render is 8 px x 16 px per cell" "$tmp/tux.png" 240x272
expect "render --scale 2" 0 '^$' -- "$cli" render --scale 2 "$piece" "$tmp/tux2.png"
size_is "--scale 2 doubles both sides" "$tmp/tux2.png" 480x544
expect "render --font-width 9" 0 '^$' -- "$cli" render --font-width 9 "$piece" "$tmp/tux9.png"
size_is "--font-width 9 is 9 px per cell" "$tmp/tux9.png" 270x272
expect "options after the files" 0 '^$' -- "$cli" render "$piece" "$tmp/tux93.png" --font-width 9 --scale 3
size_is "--font-width 9 --scale 3" "$tmp/tux93.png" 810x816

# A SAUCE with 9 px letter spacing (TFlags 0x04) renders 9 px wide unless told otherwise
cp "$piece" "$tmp/tux-9px.ans"
printf '\x05' | dd of="$tmp/tux-9px.ans" bs=1 seek=$(( $(wc -c < "$piece") - 128 + 105 )) conv=notrunc status=none
expect "SAUCE 9 px flag" 0 'letter spacing 9 px' -- "$cli" info "$tmp/tux-9px.ans"
expect "render follows SAUCE 9 px" 0 '^$' -- "$cli" render "$tmp/tux-9px.ans" "$tmp/sauce9.png"
size_is "SAUCE 9 px renders 9 px wide" "$tmp/sauce9.png" 270x272
expect "--font-width 8 overrides SAUCE" 0 '^$' -- "$cli" render --font-width 8 "$tmp/tux-9px.ans" "$tmp/sauce8.png"
size_is "--font-width 8 renders 8 px wide" "$tmp/sauce8.png" 240x272

# Legacy aspect (TFlags bits 3-4 = 1) is noted, not applied
printf '\x09' | dd of="$tmp/tux-9px.ans" bs=1 seek=$(( $(wc -c < "$piece") - 128 + 105 )) conv=notrunc status=none
expect "legacy aspect is noted" 0 'note: .*legacy \(stretched\) aspect ratio; the PNG has square pixels' -- "$cli" render "$tmp/tux-9px.ans" "$tmp/aspect.png"
size_is "legacy aspect keeps square pixels" "$tmp/aspect.png" 240x272

expect "--scale 0 refused" 2 '--scale must be 1-8' -- "$cli" render --scale 0 "$piece" "$tmp/bad.png"
expect "--scale 9 refused" 2 '--scale must be 1-8' -- "$cli" render --scale 9 "$piece" "$tmp/bad.png"
expect "--scale x refused" 2 "--scale must be 1-8, not 'x'" -- "$cli" render --scale x "$piece" "$tmp/bad.png"
expect "--font-width 7 refused" 2 '--font-width must be 8-9' -- "$cli" render --font-width 7 "$piece" "$tmp/bad.png"
expect "render needs two files" 2 'render takes an input' -- "$cli" render "$piece"
if [[ -e $tmp/bad.png ]]; then bad "a refused render left a file"; else ok "a refused render writes nothing"; fi
expect "missing input" 1 'nope\.ans: No such file' -- "$cli" render "$tmp/nope.ans" "$tmp/bad.png"
nodir='No such file or directory'
[[ -n ${BW_CLI_WINDOWS:-} ]] && nodir='Path not found\.'  # Windows' own text
expect "unwritable output says why" 1 "^blockwright-cli: .*/no-dir/out\\.png: $nodir\$" -- "$cli" render "$piece" "$tmp/no-dir/out.png"
out=$("$cli" render "$piece" "$tmp/no-dir/out.png" 2>&1)
if [[ $(wc -l <<<"$out") -eq 1 && $out != *"Unknown error"* ]]; then ok "unwritable output is one line with a reason"; else bad "unwritable output printed: $out"; fi
if [[ -e $tmp/no-dir ]]; then bad "unwritable output made a directory"; else ok "unwritable output writes nothing"; fi
# A PNG write that fails partway (a 1 KiB file size limit) names the system's
# reason, not libpng's "Unknown error", and leaves no file: at 1x the PNG
# fits Qt's write buffer, at 8x libpng writes while it encodes
# (Windows has no file size limit to set.)
if [[ -z ${BW_CLI_WINDOWS:-} ]]; then
  small() { (trap '' XFSZ; ulimit -f 1; "$@"); }
  fsz=$tmp/fsz && mkdir -p "$fsz"
  for sc in 1 8; do
    expect "short PNG write at ${sc}x says why" 1 '^blockwright-cli: .*/fsz/out\.png: write failed: File too large$' -- \
      small "$cli" render --scale "$sc" "$piece" "$fsz/out.png"
  done
  out=$(small "$cli" render --scale 8 "$piece" "$fsz/out.png" 2>&1)
  if [[ $(wc -l <<<"$out") -eq 1 && $out != *"Unknown error"* && $out != *libpng* ]]; then ok "short PNG write is one line"; else bad "short PNG write printed: $out"; fi
  if [[ -z $(ls -A "$fsz") ]]; then ok "short PNG write leaves no file"; else bad "short PNG write left: $(ls -A "$fsz")"; fi
fi


# render --irc: the IRC view, the mIRC palette and each line cut at the budget
big=$data/golden/cocktober.ans  # 120x120
expect "--help lists render --irc" 0 'render --irc <in> <out\.png>' -- "$cli" --help
expect "--help gives render --irc's budget default and range" 0 "512 \\(default: a project's own, else 407\\)" -- "$cli" --help
expect "--help says --fit, --pad and --swap-shades apply to render --irc" 0 '--pad and --swap-shades apply as in export --irc' -- "$cli" --help
expect "render --irc: a 120-row piece fits 407 bytes" 0 '^IRC view: 0 rows cut at 407 bytes$' -- "$cli" render --irc "$big" "$tmp/irc.png"
size_is "render --irc is the canvas's size" "$tmp/irc.png" 960x1920
"$cli" render "$big" "$tmp/plain.png"
if cmp -s "$tmp/irc.png" "$tmp/plain.png"; then bad "render --irc drew the plain render"; else ok "render --irc draws the IRC view, not the canvas"; fi
expect "render --irc --budget cuts rows" 0 '^IRC view: 116 rows cut at 150 bytes$' -- "$cli" render --irc --budget 150 "$big" "$tmp/irc150.png"
if cmp -s "$tmp/irc.png" "$tmp/irc150.png"; then bad "render --irc --budget 150 drew no cuts"; else ok "render --irc --budget draws the cuts"; fi
# the widest line as the ascii script sends it fits, and a byte less cuts
# the lines export --irc says are over it (#364)
sizes=$("$cli" export --irc "$big" "$tmp/irc.txt" | awk 'NR > 1 { print $3 }' | sort -n)
widest=$(tail -1 <<<"$sizes")
at=$("$cli" export --irc --budget $((widest - 1)) "$big" "$tmp/irc-w1.txt" 2>/dev/null | sed -n 's/^.*: \([0-9][0-9]*\) over$/\1/p')
expect "render --irc at the widest line's size" 0 "^IRC view: 0 rows cut at $widest bytes\$" -- "$cli" render --irc --budget "$widest" "$big" "$tmp/irc-w.png"
expect "render --irc a byte under it cuts the lines export says are over" 0 "^IRC view: $at rows? cut at $((widest - 1)) bytes\$" -- "$cli" render --irc --budget $((widest - 1)) "$big" "$tmp/irc-w1.png"
at300=$("$cli" export --irc --budget 300 "$big" "$tmp/irc-300.txt" 2>/dev/null | sed -n 's/^.*: \([0-9][0-9]*\) over$/\1/p')
if [[ ${at300:-0} -gt 0 ]]; then ok "export --irc --budget 300 has lines over"; else bad "export --irc --budget 300: ${at300:-no} lines over"; fi
expect "render --irc at 300 cuts the lines export says are over" 0 "^IRC view: ${at300:-x} rows? cut at 300 bytes\$" -- "$cli" render --irc --budget 300 "$big" "$tmp/irc-300.png"
expect "render --irc --scale --font-width" 0 '^IRC view: 0 rows cut' -- "$cli" render --irc --scale 2 --font-width 9 "$piece" "$tmp/irc-tux.png"
size_is "render --irc --scale 2 at 9 px" "$tmp/irc-tux.png" 540x544
expect "render --budget needs --irc" 2 '^blockwright-cli: --budget is a render --irc option$' -- "$cli" render --budget 60 "$piece" "$tmp/x.png"
expect "render --irc bad budget" 2 '--budget takes 1 to 512 bytes' -- "$cli" render --irc --budget 0 "$piece" "$tmp/x.png"
expect "render --irc bad budget text" 2 '--budget takes 1 to 512 bytes' -- "$cli" render --irc --budget lots "$piece" "$tmp/x.png"
expect "render --irc onto its input refused" 1 'not written: it is the input file' -- "$cli" render --irc "$piece" "$piece"
if [[ -e $tmp/x.png ]]; then bad "a refused render --irc wrote $tmp/x.png"; else ok "refused renders --irc write nothing"; fi

# render --irc takes export --irc's options and draws what export sends (#297)
cat >"$tmp/mk_over.py" <<'EOF'
import sys
row = "".join(f"\x1b[0;{30+(x%7)+1};{40+(x*3)%8}m\xdb" if x%2 else f"\x1b[0;{30+(x%6)};4{(x+1)%8}m\xdf" for x in range(80))
open(sys.argv[1],"wb").write((row+"\x1b[0m\r\n"+"\x1b[0;37;40m"+"A"*80+"\x1b[0m\r\n").encode("latin1"))
EOF
over=$tmp/over.ans
python3 -I "$tmp/mk_over.py" "$over"
expect "export --irc: the line is over" 3 '420 bytes' -- "$cli" export --irc "$over" "$tmp/over.txt"
expect "render --irc cuts the line export sends" 0 '^IRC view: 1 row cut at 407 bytes$' -- "$cli" render --irc "$over" "$tmp/over.png"
expect "render --irc --fit draws it fit" 0 '^IRC view: 0 rows cut at 407 bytes$' -- "$cli" render --irc --fit "$over" "$tmp/over-fit.png"
if cmp -s "$tmp/over.png" "$tmp/over-fit.png"; then bad "render --irc --fit drew the unfit line"; else ok "render --irc --fit draws the fit line"; fi
expect "render --irc --fit at a budget" 0 '^IRC view: 0 rows cut at 300 bytes$' -- "$cli" render --irc --fit --budget 300 "$over" "$tmp/over-300.png"
expect "render --irc --pad --swap-shades" 0 '^IRC view: 1 row cut at 407 bytes$' -- "$cli" render --irc --pad --swap-shades "$over" "$tmp/over-pad.png"
for opt in --fit --pad --swap-shades; do
  expect "render $opt needs --irc" 2 "^blockwright-cli: $opt is a render --irc option\$" -- "$cli" render "$opt" "$over" "$tmp/x.png"
done
if [[ -e $tmp/x.png ]]; then bad "a refused render wrote $tmp/x.png"; else ok "refused render options write nothing"; fi

# render never writes over its input, under any name; nothing is written
same=$tmp/same && mkdir -p "$same/sub"
cp "$piece" "$same/in.ans"
ln -s in.ans "$same/link.ans"
ln "$same/in.ans" "$same/hard.ans"
expect "render onto its input refused" 1 'in\.ans: not written: it is the input file' -- "$cli" render "$same/in.ans" "$same/in.ans"
expect "render onto ./ form refused" 1 'not written: it is the input file' -- "$cli" render "$same/in.ans" "$same/./in.ans"
expect "render onto ../ form refused" 1 'not written: it is the input file' -- "$cli" render "$same/in.ans" "$same/sub/../in.ans"
expect "render onto a symlink to the input refused" 1 'link\.ans: not written: it is the input file' -- "$cli" render "$same/in.ans" "$same/link.ans"
expect "render from a symlink onto its target refused" 1 'not written: it is the input file' -- "$cli" render "$same/link.ans" "$same/in.ans"
expect "render onto a hard link to the input refused" 1 'hard\.ans: not written: it is the input file' -- "$cli" render "$same/in.ans" "$same/hard.ans"
expect "render onto a relative form refused" 1 'not written: it is the input file' -- bash -c 'cd "$1" && "$2" render in.ans "$1/in.ans"' _ "$same" "$cli"
if cmp -s "$piece" "$same/in.ans"; then ok "refused renders leave the input"; else bad "a refused render changed the input"; fi
left=$(ls -A "$same" | grep -vxE 'in\.ans|link\.ans|hard\.ans|sub')
if [[ -z $left ]]; then ok "refused renders leave no file"; else bad "refused renders left: $left"; fi
expect "render beside its input still works" 0 '^$' -- "$cli" render "$same/in.ans" "$same/in.png"

# a .bwp is a project, which the app opens as a zip: render writes a PNG, so
# it refuses one and points to pack, as convert does (#316)
mkdir -p "$tmp/bwp/sub"
for out in y.bwp Y.BwP sub/x.tar.bwp; do
  expect "render to $out is refused" 1 "${out//./\\.}: not written: render writes a PNG; use pack for a project" -- "$cli" render "$piece" "$tmp/bwp/$out"
  expect "render --irc to $out is refused" 1 "${out//./\\.}: not written: render --irc writes a PNG; use pack for a project" -- "$cli" render --irc "$piece" "$tmp/bwp/$out"
  if [[ -e $tmp/bwp/$out ]]; then bad "render to $out wrote a file"; else ok "render to $out writes nothing"; fi
done
expect "render refuses a .bwp before reading the input" 1 'y\.bwp: not written: render writes a PNG' -- "$cli" render "$tmp/bwp/missing.ans" "$tmp/bwp/y.bwp"
expect "render still writes a .bwp.png" 0 '^$' -- "$cli" render "$piece" "$tmp/bwp/y.bwp.png"
size_is "the .bwp.png is the render" "$tmp/bwp/y.bwp.png" 240x272

exit $(( fails > 0 ))
