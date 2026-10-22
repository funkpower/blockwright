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
printf '\x05' | dd of="$tmp/tux-9px.ans" bs=1 seek=$(( $(stat -c %s "$piece") - 128 + 105 )) conv=notrunc status=none
expect "SAUCE 9 px flag" 0 'letter spacing 9 px' -- "$cli" info "$tmp/tux-9px.ans"
expect "render follows SAUCE 9 px" 0 '^$' -- "$cli" render "$tmp/tux-9px.ans" "$tmp/sauce9.png"
size_is "SAUCE 9 px renders 9 px wide" "$tmp/sauce9.png" 270x272
expect "--font-width 8 overrides SAUCE" 0 '^$' -- "$cli" render --font-width 8 "$tmp/tux-9px.ans" "$tmp/sauce8.png"
size_is "--font-width 8 renders 8 px wide" "$tmp/sauce8.png" 240x272

# Legacy aspect (TFlags bits 3-4 = 1) is noted, not applied
printf '\x09' | dd of="$tmp/tux-9px.ans" bs=1 seek=$(( $(stat -c %s "$piece") - 128 + 105 )) conv=notrunc status=none
expect "legacy aspect is noted" 0 'note: .*legacy \(stretched\) aspect ratio; the PNG has square pixels' -- "$cli" render "$tmp/tux-9px.ans" "$tmp/aspect.png"
size_is "legacy aspect keeps square pixels" "$tmp/aspect.png" 240x272

expect "--scale 0 refused" 2 '--scale must be 1-8' -- "$cli" render --scale 0 "$piece" "$tmp/bad.png"
expect "--scale 9 refused" 2 '--scale must be 1-8' -- "$cli" render --scale 9 "$piece" "$tmp/bad.png"
expect "--scale x refused" 2 "--scale must be 1-8, not 'x'" -- "$cli" render --scale x "$piece" "$tmp/bad.png"
expect "--font-width 7 refused" 2 '--font-width must be 8-9' -- "$cli" render --font-width 7 "$piece" "$tmp/bad.png"
expect "render needs two files" 2 'render takes an input' -- "$cli" render "$piece"
if [[ -e $tmp/bad.png ]]; then bad "a refused render left a file"; else ok "a refused render writes nothing"; fi
expect "missing input" 1 'nope\.ans: No such file' -- "$cli" render "$tmp/nope.ans" "$tmp/bad.png"
expect "unwritable output says why" 1 '^blockwright-cli: .*/no-dir/out\.png: No such file or directory$' -- "$cli" render "$piece" "$tmp/no-dir/out.png"
out=$("$cli" render "$piece" "$tmp/no-dir/out.png" 2>&1)
if [[ $(wc -l <<<"$out") -eq 1 && $out != *"Unknown error"* ]]; then ok "unwritable output is one line with a reason"; else bad "unwritable output printed: $out"; fi
if [[ -e $tmp/no-dir ]]; then bad "unwritable output made a directory"; else ok "unwritable output writes nothing"; fi
# A PNG write that fails partway (a 1 KiB file size limit) names the system's
# reason, not libpng's "Unknown error", and leaves no file: at 1x the PNG
# fits Qt's write buffer, at 8x libpng writes while it encodes
small() { (trap '' XFSZ; ulimit -f 1; "$@"); }
fsz=$tmp/fsz && mkdir -p "$fsz"
for sc in 1 8; do
  expect "short PNG write at ${sc}x says why" 1 '^blockwright-cli: .*/fsz/out\.png: write failed: File too large$' -- \
    small "$cli" render --scale "$sc" "$piece" "$fsz/out.png"
done
out=$(small "$cli" render --scale 8 "$piece" "$fsz/out.png" 2>&1)
if [[ $(wc -l <<<"$out") -eq 1 && $out != *"Unknown error"* && $out != *libpng* ]]; then ok "short PNG write is one line"; else bad "short PNG write printed: $out"; fi
if [[ -z $(ls -A "$fsz") ]]; then ok "short PNG write leaves no file"; else bad "short PNG write left: $(ls -A "$fsz")"; fi


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

exit $(( fails > 0 ))
