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

# stock macOS has no timeout(1)
command -v timeout >/dev/null || timeout() { perl -e 'alarm shift; exec @ARGV or die "$ARGV[0]: $!\n"' "$@"; }

piece=$data/golden/tux.ans

expect "--version prints version and short commit" 0 '^blockwright-cli [0-9]+\.[0-9]+\.[0-9]+ \(([0-9a-f]{7,}(-dirty)?|unknown)\)$' -- "$cli" --version
expect "--help" 0 'convert <in> <out>' -- "$cli" --help
# BW_LOGO: branding/logo.txt, which --version prints under the version line
# and --help above the usage
if [[ -n ${BW_LOGO:-} ]]; then
  logo=$(tr -d '\r' <"$BW_LOGO")
  out=$("$cli" --version 2>&1 | tr -d '\r')
  if [[ $(head -n1 <<<"$out") == blockwright-cli\ * && $out == *"$logo"* ]]; then ok "--version: version line, then the logo"; else bad "--version has no logo under the version: $out"; fi
  out=$("$cli" --help 2>&1 | tr -d '\r')
  if [[ $out == "$logo"* ]]; then ok "--help starts with the logo"; else bad "--help doesn't start with the logo: $out"; fi
else
  bad "BW_LOGO isn't set"
fi
expect "--help lists export" 0 'export --irc <in> <out>' -- "$cli" --help
expect "--help says convert, shade, export --irc and render refuse a .bwp" 0 'convert, shade, export --irc and render refuse an' -- "$cli" --help

expect "info: dimensions" 0 '^size: +30x17$' -- "$cli" info "$piece"
expect "info: SAUCE author" 0 '^author: +funkpower$' -- "$cli" info "$piece"
expect "info: FileSize checked" 0 '^filesize: +1948 \(matches content\)$' -- "$cli" info "$piece"
expect "info: flags" 0 '^flags: +0x1 \(iCE on' -- "$cli" info "$piece"

expect "convert" 0 '^$' -- "$cli" convert "$piece" "$tmp/tux.ans"
if cmp -s "$piece" "$tmp/tux.ans"; then ok "convert output is byte-identical"; else bad "convert output differs"; fi
# a .bwp is a project, which the app opens as a zip: convert and shade write
# art, so they refuse one and point to pack (#298)
for out in tux.bwp TUX.BWP; do
  expect "convert to $out is refused" 1 "$out: not written: convert writes \\.ans, \\.xb or \\.bin; use pack for a project" -- "$cli" convert "$piece" "$tmp/$out"
  if [[ -e $tmp/$out ]]; then bad "convert to $out wrote a file"; else ok "convert to $out writes nothing"; fi
done
expect "shade to a .bwp is refused" 1 'tux\.bwp: not written: shade writes \.ans, \.xb or \.bin; use pack for a project' -- "$cli" shade --ramp steel "$piece" "$tmp/tux.bwp"
if [[ -e $tmp/tux.bwp ]]; then bad "shade to a .bwp wrote a file"; else ok "shade to a .bwp writes nothing"; fi
# export --irc writes IRC text, so it refuses a .bwp the same way (#316)
mkdir -p "$tmp/sub"
for out in y.bwp Y.BwP sub/x.tar.bwp; do
  expect "export --irc to $out is refused" 1 "${out//./\\.}: not written: export --irc writes IRC text; use pack for a project" -- "$cli" export --irc "$piece" "$tmp/$out"
  if [[ -e $tmp/$out ]]; then bad "export --irc to $out wrote a file"; else ok "export --irc to $out writes nothing"; fi
done
expect "export --irc refuses a .bwp before reading the input" 1 'y\.bwp: not written: export --irc writes IRC text' -- "$cli" export --irc "$tmp/missing.ans" "$tmp/y.bwp"
expect "export --irc still writes a .bwp.txt" 0 'budget 407: 0 over$' -- "$cli" export --irc "$piece" "$tmp/y.bwp.txt"
expect "pack writes the .bwp convert refuses" 0 '^$' -- "$cli" pack "$piece" "$tmp/tux.bwp"
expect "info reads it as a project" 0 '^project: ' -- "$cli" info "$tmp/tux.bwp"

# sauce <file> <iCE 0|1>: append a SAUCE record (80 wide, 1 high) to a body under 256 bytes
sauce() {
  local size; size=$(($(wc -c < "$1")))
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
# (Windows has no file size limit to set.)
fsz=$tmp/fsz && mkdir -p "$fsz"
if [[ -z ${BW_CLI_WINDOWS:-} ]]; then
  small() { (trap '' XFSZ; ulimit -f 1; "$@"); }  # 1 KiB limit; tux.ans is 2 KiB
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
fi
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

# XBIN and BIN (#21): the format follows <out>'s name, and reading goes by the
# XBIN header first, then the name
xb=$data/xbin
expect "convert to .xb" 0 '^$' -- "$cli" convert "$piece" "$tmp/tux.xb"
if cmp -s "$xb/tux.xb" "$tmp/tux.xb"; then ok "convert to .xb writes the fixture"; else bad "convert to .xb differs from xbin/tux.xb"; fi
expect "convert to .bin" 0 '^$' -- "$cli" convert "$piece" "$tmp/tux.bin"
if cmp -s "$xb/tux.bin" "$tmp/tux.bin"; then ok "convert to .bin writes the fixture"; else bad "convert to .bin differs from xbin/tux.bin"; fi
expect "info: XBIN format" 0 '^format: +XBIN, compressed$' -- "$cli" info "$tmp/tux.xb"
expect "info: XBIN uncompressed" 0 '^format: +XBIN$' -- "$cli" info "$xb/tux-plain.xb"
expect "info: XBIN size" 0 '^size: +30x17$' -- "$cli" info "$tmp/tux.xb"
expect "info: XBIN FileSize checked" 0 '^filesize: +443 \(matches content\)$' -- "$cli" info "$tmp/tux.xb"
expect "info: XBIN SAUCE" 0 '^datatype: +6$' -- "$cli" info "$tmp/tux.xb"
expect "info: BIN format" 0 '^format: +BIN$' -- "$cli" info "$tmp/tux.bin"
expect "info: BIN FileSize checked" 0 '^filesize: +1020 \(matches content\)$' -- "$cli" info "$tmp/tux.bin"
expect "info: BIN size from SAUCE" 0 '^size: +30x17$' -- "$cli" info "$tmp/tux.bin"
expect "info: .ans has no format line" 1 '' -- grep -q '^format:' <("$cli" info "$piece")
# UTF-8 .ans (#302): found by its bytes, read into the same cells, written as CP437
u8=$data/utf8/tux-utf8.ans
expect "info: a CP437 .ans says so" 0 '^encoding: +CP437$' -- "$cli" info "$piece"
expect "info: a UTF-8 .ans says so" 0 '^encoding: +UTF-8$' -- "$cli" info "$u8"
expect "info: a UTF-8 .ans keeps its SAUCE" 0 '^author: +funkpower$' -- "$cli" info "$u8"
expect "info: a UTF-8 .ans's FileSize is its UTF-8 length" 0 '^filesize: +2492 \(matches content\)$' -- "$cli" info "$u8"
expect "info --encoding cp437 reads it as CP437" 0 '^encoding: +CP437$' -- "$cli" info --encoding cp437 "$u8"
expect "info --encoding UTF-8 reads a CP437 file as UTF-8" 0 '^encoding: +UTF-8$' -- "$cli" info --encoding UTF-8 "$piece"
expect "info --encoding auto" 0 '^encoding: +UTF-8$' -- "$cli" info --encoding auto "$u8"
expect "--encoding with a bad value is refused" 2 "--encoding takes auto, cp437 or utf-8, not 'latin1'" -- "$cli" info --encoding latin1 "$u8"
expect "--encoding on library is refused" 2 '--encoding is an info, convert, render, export, shade, lint, pack or unpack option, not a library option' -- "$cli" library --encoding cp437
expect "--help lists --encoding" 0 '^  --encoding <enc> +info, convert, render, export, shade, lint, pack,$' -- "$cli" --help
expect "info: an XBIN has no encoding line" 1 '' -- grep -q '^encoding:' <("$cli" info "$tmp/tux.xb")
expect "convert a UTF-8 .ans" 0 '^$' -- "$cli" convert "$u8" "$tmp/from-u8.ans"
if cmp -s "$piece" "$tmp/from-u8.ans"; then ok "a UTF-8 .ans converts to the CP437 piece, byte for byte"; else bad "convert of the UTF-8 piece differs from the CP437 piece"; fi
"$cli" render "$piece" "$tmp/tux.png" && "$cli" render "$u8" "$tmp/tux-u8.png"
if cmp -s "$tmp/tux.png" "$tmp/tux-u8.png"; then ok "a UTF-8 .ans renders as the CP437 piece"; else bad "the UTF-8 piece renders differently"; fi
"$cli" unpack "$piece" "$tmp/unpack-cp" > /dev/null && "$cli" unpack "$u8" "$tmp/unpack-u8" > /dev/null
if diff -r -x manifest.json "$tmp/unpack-cp" "$tmp/unpack-u8" > /dev/null; then ok "a UTF-8 .ans unpacks as the CP437 piece"; else bad "unpack of the UTF-8 piece differs"; fi
if diff <(grep -v '"ansEncoding"' "$tmp/unpack-u8/manifest.json") "$tmp/unpack-cp/manifest.json" > /dev/null; then ok "its manifest differs only by ansEncoding"; else bad "the UTF-8 piece's manifest differs by more than ansEncoding"; fi
expect "info: a project unpacked from a UTF-8 .ans says so" 0 '^encoding: +UTF-8$' -- "$cli" info "$tmp/unpack-u8"
expect "info: a project unpacked from a CP437 .ans says so" 0 '^encoding: +CP437$' -- "$cli" info "$tmp/unpack-cp"
expect "a CP437 project's manifest has no ansEncoding" 1 '' -- grep -q '"ansEncoding"' "$tmp/unpack-cp/manifest.json"
printf '\xe2\x96\x88\r\n' > "$tmp/block-u8.ans"
expect "export --irc of a UTF-8 full block" 0 '^1 line, longest 6 bytes' -- "$cli" export --irc "$tmp/block-u8.ans" "$tmp/block-u8.txt"
if [[ $(od -An -tx1 "$tmp/block-u8.txt" | tr -d ' \n') == *e29688* && $(od -An -tx1 "$tmp/block-u8.txt" | tr -d ' \n') != *ce93* ]]; then ok "it sends one █, not Γûê"; else bad "export --irc of a UTF-8 block: $(od -An -tx1 "$tmp/block-u8.txt")"; fi
"$cli" export --irc --encoding cp437 "$tmp/block-u8.ans" "$tmp/block-cp.txt"
if [[ $(od -An -tx1 "$tmp/block-cp.txt" | tr -d ' \n') == *ce93c3bbc3aa* ]]; then ok "export --irc --encoding cp437 sends Γûê"; else bad "export --irc --encoding cp437: $(od -An -tx1 "$tmp/block-cp.txt")"; fi
expect "convert a UTF-8 full block" 0 '^$' -- "$cli" convert "$tmp/block-u8.ans" "$tmp/block-cp.ans"
if [[ $(od -An -tx1 "$tmp/block-cp.ans" | tr -d ' \n') == *db* && $(od -An -tx1 "$tmp/block-cp.ans" | tr -d ' \n') != *e29688* ]]; then ok "it is written as the CP437 byte DB"; else bad "convert of a UTF-8 block: $(od -An -tx1 "$tmp/block-cp.ans")"; fi
printf '\xef\xbb\xbfA\xc3\x87\r\n' > "$tmp/bom.ans"
expect "info: a BOM says UTF-8" 0 '^encoding: +UTF-8$' -- "$cli" info "$tmp/bom.ans"
"$cli" convert "$tmp/bom.ans" "$tmp/bom-cp.ans"
if [[ $(od -An -tx1 "$tmp/bom-cp.ans" | tr -d ' \n') == *4180* && $(od -An -tx1 "$tmp/bom-cp.ans" | tr -d ' \n') != *efbbbf* ]]; then ok "the BOM is dropped and Ç is written as 80"; else bad "convert of a BOM file: $(od -An -tx1 "$tmp/bom-cp.ans")"; fi
printf '\xc9\xbb\r\n\xc8\xbc\r\n' > "$tmp/corners.ans"
expect "info: CP437 corners that look like UTF-8 stay CP437" 0 '^encoding: +CP437$' -- "$cli" info "$tmp/corners.ans"
# writing UTF-8 (#302 part 2): convert, shade and replay take --output-encoding
printf 'A\xc2\x9b31mB\r\n' > "$tmp/c1-u8.ans"
expect "convert --output-encoding utf-8 refuses a C1 control" 1 'warning: .*c1-u8\.ans: 1 cell holds a control character a UTF-8 \.ans can.t hold.*U\+009B' -- \
  "$cli" convert --encoding utf-8 --output-encoding utf-8 "$tmp/c1-u8.ans" "$tmp/c1-out.ans"
"$cli" convert --allow-lossy --encoding utf-8 --output-encoding utf-8 "$tmp/c1-u8.ans" "$tmp/c1-out.ans" 2> /dev/null
if [[ $(od -An -tx1 "$tmp/c1-out.ans" | tr -d ' \n') == *413f33316d42* ]]; then ok "--allow-lossy writes it as '?'"; else bad "a C1 control written as UTF-8: $(od -An -tx1 "$tmp/c1-out.ans")"; fi
expect "convert --output-encoding utf-8" 0 '^$' -- "$cli" convert --output-encoding utf-8 "$piece" "$tmp/to-u8.ans"
if cmp -s "$u8" "$tmp/to-u8.ans"; then ok "the CP437 piece written as UTF-8 is the UTF-8 piece, byte for byte"; else bad "convert --output-encoding utf-8 differs from tux-utf8.ans"; fi
expect "info: what convert wrote as UTF-8 says so" 0 '^encoding: +UTF-8$' -- "$cli" info "$tmp/to-u8.ans"
expect "info: its FileSize is its UTF-8 length" 0 '^filesize: +2492 \(matches content\)$' -- "$cli" info "$tmp/to-u8.ans"
"$cli" convert "$tmp/to-u8.ans" "$tmp/back-cp.ans"
if cmp -s "$piece" "$tmp/back-cp.ans"; then ok "and converts back to the CP437 piece"; else bad "UTF-8 round trip differs from the CP437 piece"; fi
"$cli" convert --output-encoding cp437 "$u8" "$tmp/u8-cp.ans"
if cmp -s "$piece" "$tmp/u8-cp.ans"; then ok "--output-encoding cp437 writes the CP437 piece"; else bad "--output-encoding cp437 differs"; fi
for enc in UTF-8 utf8 Utf8; do
  "$cli" convert --output-encoding "$enc" "$piece" "$tmp/to-u8-$enc.ans"
  if cmp -s "$u8" "$tmp/to-u8-$enc.ans"; then ok "--output-encoding $enc is UTF-8"; else bad "--output-encoding $enc isn't UTF-8"; fi
done
expect "--output-encoding with a bad value is refused" 2 "--output-encoding takes cp437 or utf-8, not 'auto'" -- "$cli" convert --output-encoding auto "$piece" "$tmp/bad-enc.ans"
if [[ -e $tmp/bad-enc.ans ]]; then bad "a bad --output-encoding wrote a file"; else ok "a bad --output-encoding writes nothing"; fi
for cmd in info lint render export pack; do
  expect "--output-encoding on $cmd is refused" 2 "--output-encoding is a convert, shade, snapshot or replay option, not an? $cmd option" -- "$cli" "$cmd" --output-encoding utf-8 "$piece" "$tmp/x"
done
for out in tux-u8.xb tux-u8.XBIN tux-u8.bin; do
  expect "--output-encoding utf-8 to $out is refused" 2 "--output-encoding utf-8 writes an \.ans; .*${out//./\\.} is XBIN or BIN, which are CP437" -- "$cli" convert --output-encoding utf-8 "$piece" "$tmp/$out"
  if [[ -e $tmp/$out ]]; then bad "--output-encoding utf-8 to $out wrote a file"; else ok "--output-encoding utf-8 to $out writes nothing"; fi
done
expect "--output-encoding cp437 to an XBIN is fine" 0 '^$' -- "$cli" convert --output-encoding cp437 "$piece" "$tmp/tux-cp.xb"
# a byte order mark only where the bytes don't say UTF-8: é is two bytes
printf 'caf\x82\r\n' > "$tmp/cafe-cp.ans"
"$cli" convert --encoding cp437 --output-encoding utf-8 "$tmp/cafe-cp.ans" "$tmp/cafe-u8.ans"
hex=$(od -An -tx1 "$tmp/cafe-u8.ans" | tr -d ' \n')
if [[ $hex == efbbbf* && $hex == *636166c3a9* ]]; then ok "é-only UTF-8 starts with a byte order mark"; else bad "é-only UTF-8: $hex"; fi
expect "info: and reads back as UTF-8" 0 '^encoding: +UTF-8$' -- "$cli" info "$tmp/cafe-u8.ans"
if [[ $(head -c 3 "$tmp/to-u8.ans" | od -An -tx1 | tr -d ' \n') != efbbbf ]]; then ok "no byte order mark when a glyph is three bytes"; else bad "tux as UTF-8 has a byte order mark"; fi
# ○ ← ◙ survive as UTF-8, where a CP437 .ans loses them (#300)
printf 'XBIN\x1a\x04\x00\x01\x00\x10\x00\x09\x07\x1b\x07\x0a\x07\x41\x07' > "$tmp/ctl.xb"
expect "control-byte glyphs convert to UTF-8 with no warning" 0 '^$' -- "$cli" convert --output-encoding utf-8 "$tmp/ctl.xb" "$tmp/ctl-u8.ans"
hex=$(od -An -tx1 "$tmp/ctl-u8.ans" | tr -d ' \n')
if [[ $hex == *e2978be28690e2979941* ]]; then ok "○ ← ◙ kept as UTF-8"; else bad "○ ← ◙ as UTF-8: $hex"; fi
# a glyph with no CP437 code is kept too
expect "a glyph with no CP437 code converts to UTF-8 with no warning" 0 '^$' -- "$cli" convert --output-encoding utf-8 "$data/projects/unwritable.bwp" "$tmp/unwritable-u8.ans"
if [[ $(od -An -tx1 "$tmp/unwritable-u8.ans" | tr -d ' \n') == *e29885* ]]; then ok "★ kept as UTF-8"; else bad "★ lost in UTF-8"; fi
expect "shade --output-encoding utf-8" 0 '' -- "$cli" shade --ramp steel --output-encoding utf-8 "$piece" "$tmp/shade-u8.ans"
expect "info: shade wrote UTF-8" 0 '^encoding: +UTF-8$' -- "$cli" info "$tmp/shade-u8.ans"
"$cli" shade --ramp steel "$piece" "$tmp/shade-cp.ans"
"$cli" convert "$tmp/shade-u8.ans" "$tmp/shade-u8-cp.ans"
if cmp -s "$tmp/shade-cp.ans" "$tmp/shade-u8-cp.ans"; then ok "shade's UTF-8 output is its CP437 output's cells"; else bad "shade UTF-8 and CP437 outputs differ"; fi
expect "shade --output-encoding utf-8 to a .bin is refused" 2 'is XBIN or BIN, which are CP437' -- "$cli" shade --ramp steel --output-encoding utf-8 "$piece" "$tmp/shade.bin"
expect "--help lists --output-encoding" 0 '^  --output-encoding <enc> +convert, shade, snapshot, replay: write an \.ans$' -- "$cli" --help
expect "--help says convert, shade and replay write UTF-8 with it" 0 '^and replay write an \.ans as CP437, or as UTF-8 with --output-encoding$' -- "$cli" --help
expect "--help says snapshot writes the project's encoding" 0 "^sequence of three bytes or more\\)\\. snapshot writes the project's own$" -- "$cli" --help
expect "the usage says convert, shade, snapshot and replay take it" 2 '^ +convert, shade, snapshot and replay take --output-encoding cp437\|utf-8$' -- "$cli" convert
cp "$tmp/tux.xb" "$tmp/tux-xb.ans"
expect "an XBIN is read by its header whatever its name" 0 '^format: +XBIN' -- "$cli" info "$tmp/tux-xb.ans"
expect "convert .xb to .ans" 0 '^$' -- "$cli" convert "$tmp/tux.xb" "$tmp/from-xb.ans"
expect "convert .bin to .ans" 0 '^$' -- "$cli" convert "$tmp/tux.bin" "$tmp/from-bin.ans"
if cmp -s "$piece" "$tmp/from-bin.ans"; then ok ".ans -> .bin -> .ans is the same file"; else bad ".bin round trip through .ans differs"; fi
# XBin's SAUCE has no font name (TInfoS), so only that is gone
expect ".ans from XBIN: no SAUCE font name" 0 '^font: *$' -- "$cli" info "$tmp/from-xb.ans"
expect "convert .ans from XBIN back to .xb" 0 '^$' -- "$cli" convert "$tmp/from-xb.ans" "$tmp/again.xb"
if cmp -s "$xb/tux.xb" "$tmp/again.xb"; then ok ".ans -> .xb -> .ans -> .xb is the same file"; else bad ".xb round trip through .ans differs"; fi
expect "info: XBIN palette" 0 '^palette: +custom$' -- "$cli" info "$xb/tux-palette-font.xb"
expect "info: XBIN font" 0 '^font: +8x8$' -- "$cli" info "$xb/tux-palette-font.xb"
expect "info: no XBIN palette" 0 '^palette: +none$' -- "$cli" info "$tmp/tux.xb"
expect "XBIN palette lost as .ans: refused" 1 'tux-palette-font\.xb: its palette is left out: only XBIN holds one' -- \
  "$cli" convert "$xb/tux-palette-font.xb" "$tmp/pf.ans"
expect "XBIN font lost as .bin: refused" 1 'tux-palette-font\.xb: its font is left out: only XBIN holds one' -- \
  "$cli" convert "$xb/tux-palette-font.xb" "$tmp/pf.bin"
if [[ -e $tmp/pf.ans || -e $tmp/pf.bin ]]; then bad "refused XBIN convert left a file"; else ok "refused XBIN convert writes nothing"; fi
expect "XBIN palette and font: --allow-lossy writes" 0 'its palette is left out' -- \
  "$cli" convert --allow-lossy "$xb/tux-palette-font.xb" "$tmp/pf.ans"
expect "XBIN to XBIN keeps the palette and font" 0 '^$' -- "$cli" convert "$xb/tux-palette-font.xb" "$tmp/pf.xb"
if cmp -s "$xb/tux-palette-font.xb" "$tmp/pf.xb"; then ok "XBIN with a palette and font rewrites byte for byte"; else bad "XBIN with a palette and font differs rewritten"; fi
# a project holds no palette or font either: pack and unpack refuse to drop them
expect "pack: XBIN palette and font lost: refused" 1 \
  'pf\.bwp: not written: the project loses data \(--allow-lossy writes it anyway\)' -- \
  "$cli" pack "$xb/tux-palette-font.xb" "$tmp/pf.bwp"
expect "unpack: XBIN palette and font lost: refused" 1 \
  'pf-dir: not written: the project loses data \(--allow-lossy writes it anyway\)' -- \
  "$cli" unpack "$xb/tux-palette-font.xb" "$tmp/pf-dir"
if [[ -e $tmp/pf.bwp || -e $tmp/pf-dir ]]; then bad "refused pack or unpack left a project"; else ok "refused pack and unpack write nothing"; fi
expect "pack --allow-lossy writes without the palette" 0 'its palette is left out' -- \
  "$cli" pack --allow-lossy "$xb/tux-palette-font.xb" "$tmp/pf.bwp"
expect "unpack --allow-lossy writes without the font" 0 'its font is left out' -- \
  "$cli" unpack --allow-lossy "$xb/tux-palette-font.xb" "$tmp/pf-dir"
expect "the packed project is the XBIN's cells" 0 '^size: +30x17$' -- "$cli" info "$tmp/pf.bwp"
[[ -f $tmp/pf-dir/manifest.json ]] && ok "unpack --allow-lossy wrote the project" || bad "unpack --allow-lossy wrote no project"
expect "pack: an XBIN without a palette or font is not lossy" 0 '^$' -- "$cli" pack "$tmp/tux.xb" "$tmp/tux-xb.bwp"
# #299: XBIN has no place for SAUCE's 9 px letter spacing, aspect ratio or
# font name (TFlags and TInfoS are 0 for XBin); BIN keeps them. tux.ans with
# its record's TFlags 0x15 (iCE, 9 px, square) and TInfoS "Amiga Topaz 1"
size=$(wc -c <"$piece")
{ head -c $((size - 23)) "$piece"; printf '\x15Amiga Topaz 1'; head -c 9 /dev/zero; } > "$tmp/flags.ans"
expect "info: the patched flags" 0 '^flags: +0x15 \(iCE on, letter spacing 9 px, aspect square\)$' -- "$cli" info "$tmp/flags.ans"
expect "XBIN drops 9 px letter spacing: refused" 1 \
  'flags\.ans: its 9 px letter spacing is left out: XBIN has no letter spacing and draws 8 px$' -- \
  "$cli" convert "$tmp/flags.ans" "$tmp/flags.xb"
expect "XBIN drops the aspect ratio: says so" 1 'flags\.ans: its square aspect ratio is left out: XBIN has no aspect ratio$' -- \
  "$cli" convert "$tmp/flags.ans" "$tmp/flags.xb"
expect "XBIN drops the font name: says so" 1 \
  'flags\.ans: its SAUCE font name \(Amiga Topaz 1\) is left out: XBIN names no font$' -- \
  "$cli" convert "$tmp/flags.ans" "$tmp/flags.xb"
if [[ -e $tmp/flags.xb ]]; then bad "refused XBIN convert of SAUCE flags left a file"; else ok "refused XBIN convert of SAUCE flags writes nothing"; fi
expect "XBIN SAUCE flags: --allow-lossy writes" 0 '9 px letter spacing is left out' -- \
  "$cli" convert --allow-lossy "$tmp/flags.ans" "$tmp/flags.xb"
expect "info: the XBIN keeps iCE" 0 '^ice: +on$' -- "$cli" info "$tmp/flags.xb"
expect "info: the XBIN's SAUCE has no flags" 0 '^flags: +0x0 ' -- "$cli" info "$tmp/flags.xb"
expect "BIN keeps the SAUCE flags" 0 '^$' -- "$cli" convert "$tmp/flags.ans" "$tmp/flags.bin"
expect "info: the BIN's flags" 0 '^flags: +0x15 ' -- "$cli" info "$tmp/flags.bin"
expect "info: the BIN's font name" 0 '^font: +Amiga Topaz 1$' -- "$cli" info "$tmp/flags.bin"

# #300: ○ ← ◙ are CP437 0x09 0x1B 0x0A, which XBIN and BIN keep and .ans reads as TAB, ESC and LF
printf 'XBIN\x1a\x04\x00\x01\x00\x10\x00\x09\x07\x1b\x07\x0a\x07\x41\x07' > "$tmp/ctl.xb"
expect "control bytes lost as .ans: refused, and says which" 1 \
  "^blockwright-cli: warning: .*ctl\.xb: 3 glyphs have CP437 bytes \.ans reads as control codes \(TAB, ESC, LF\) and are written as '\?' \(first: U\+25CB '○' at column 1, row 1\); XBIN and BIN keep them$" -- \
  "$cli" convert "$tmp/ctl.xb" "$tmp/ctl.ans"
if "$cli" convert "$tmp/ctl.xb" "$tmp/ctl.ans" 2>&1 | grep -q 'no CP437'; then bad "control bytes called glyphs with no CP437 code"; else ok "control bytes not called glyphs with no CP437 code"; fi
expect "control bytes: --allow-lossy writes '?'" 0 'XBIN and BIN keep them' -- "$cli" convert --allow-lossy "$tmp/ctl.xb" "$tmp/ctl.ans"
if [[ $(head -c 32 "$tmp/ctl.ans") == *'???A'* ]]; then ok "control bytes written as '?' in .ans"; else bad "control bytes not written as '?': $(head -c 32 "$tmp/ctl.ans" | od -c)"; fi
expect "control bytes kept as .bin" 0 '^$' -- "$cli" convert "$tmp/ctl.xb" "$tmp/ctl.bin"
if [[ $(head -c 8 "$tmp/ctl.bin" | od -An -tx1 | tr -d ' \n') == 09071b070a074107 ]]; then ok "BIN keeps the control bytes"; else bad "BIN lost the control bytes: $(head -c 8 "$tmp/ctl.bin" | od -An -tx1)"; fi
expect "lint: control bytes are no-cp437, with why" 4 "^0,0 no-cp437 - U\+25CB ○ is CP437 0x09, which \.ans reads as TAB: an \.ans saves it as '\?' \(XBIN and BIN keep it\)$" -- \
  "$cli" lint "$tmp/ctl.xb"
expect "lint: each control byte is found" 4 '^3 findings \(3 no-cp437\)' -- "$cli" lint "$tmp/ctl.xb"
# a C1 CSI (U+009B) and a right-to-left override (U+202E) from a UTF-8 file
# are named by their code points, never echoed raw (#327 finding 3); the
# U+202E is zero-width, so it joins the J before it
printf '\xe2\x96\x88\xc2\x9b2J\xe2\x80\xae!\r\n' > "$tmp/c1-bidi.ans"
hexout() { "$@" 2>&1 | od -An -v -tx1 | tr -s ' \n' '  '; }
for json in "" --json; do
  out=$("$cli" lint $json "$tmp/c1-bidi.ans" 2>&1); st=$?
  hex=$(hexout "$cli" lint $json "$tmp/c1-bidi.ans")
  if [[ $st -eq 4 && $out == *"U+009B has no CP437 code"* && $out == *"U+004A U+202E J U+202E has no CP437 code"* && $out == *"holds the bidi control U+202E"* ]]; then ok "lint $json: U+009B and U+202E named"; else bad "lint $json of C1 and bidi controls (exit $st): $out"; fi
  if [[ $hex != *" c2 9b "* && $hex != *" e2 80 ae "* ]]; then ok "lint $json: no raw U+009B or U+202E"; else bad "lint $json echoes a raw control: $hex"; fi
done
expect "convert: a C1 control is named by its code point alone" 1 \
  "warning: .*c1-bidi\.ans: 1 glyph has no CP437 code and is written as '\?' \(first: U\+009B at column 2, row 1\)$" -- \
  "$cli" convert "$tmp/c1-bidi.ans" "$tmp/c1-bidi-cp.ans"
hex=$(hexout "$cli" convert "$tmp/c1-bidi.ans" "$tmp/c1-bidi-cp.ans")
if [[ $hex != *" c2 9b "* && $hex != *" e2 80 ae "* ]]; then ok "convert: no raw U+009B or U+202E in the warning"; else bad "convert echoes a raw control: $hex"; fi
# ❤️ (U+2764 U+FE0F), U+0301, the Hangul filler U+3164 and the
# noncharacter U+FFFE show nothing on their own: lint gives their
# code alone (review of #332, N1 and N2). The three zero-width ones
# join the ❤ in one cluster cell; U+FFFE takes a cell of its own
printf '\xe2\x96\x88\xe2\x9d\xa4\xef\xb8\x8f\xcc\x81\xe3\x85\xa4\xef\xbf\xbe\r\n' > "$tmp/invisible.ans"
for json in "" --json; do
  out=$("$cli" lint $json "$tmp/invisible.ans" 2>&1); st=$?
  hex=$(hexout "$cli" lint $json "$tmp/invisible.ans")
  if [[ $st -eq 4 && $out == *"U+2764 U+FE0F U+0301 U+3164 ❤ U+FE0F U+0301 U+3164 has no CP437 code"* && $out == *"U+FFFE has no CP437 code"* ]]; then ok "lint $json: invisible glyphs named by code"; else bad "lint $json of invisible glyphs (exit $st): $out"; fi
  if [[ $hex != *" ef b8 8f "* && $hex != *" cc 81 "* && $hex != *" e3 85 a4 "* && $hex != *" ef bf be "* ]]; then ok "lint $json: no raw invisible glyph"; else bad "lint $json echoes an invisible glyph: $hex"; fi
done
# #334 slice 6: a cluster keeps 16 code points; reading says what it dropped
{ printf '   \xe2\x96\x88a'; for i in $(seq 18); do printf '\xcc\x81'; done; printf '\r\n'; } > "$tmp/marks.ans"
expect "reading: marks past 16 are reported" 0 \
  "^blockwright-cli: warning: .*marks\.ans: 3 combining code points past 16 dropped at 4,0$" -- "$cli" info "$tmp/marks.ans"
# review of #348, M1 and M2: a lone mark after a cell (here after a cursor
# move) and a lone wide cell are written as '?' to UTF-8, with a warning,
# so the row keeps its columns
printf '\xe2\x96\x88b\x1b[D\x1b[C\xcc\x81c\r\n' > "$tmp/lone-mark.ans"
expect "convert --output-encoding utf-8 warns of a lone mark" 1 \
  "warning: .*lone-mark\.ans: 1 cell holds a zero-width glyph a terminal would draw on the cell before it and is written as '\?', so the row keeps its columns \(first: U\+0301 at column 3, row 1\)$" -- \
  "$cli" convert --output-encoding utf-8 "$tmp/lone-mark.ans" "$tmp/lone-mark-u8.ans"
"$cli" convert --allow-lossy --output-encoding utf-8 "$tmp/lone-mark.ans" "$tmp/lone-mark-u8.ans" 2> /dev/null
hex=$(od -An -v -tx1 "$tmp/lone-mark-u8.ans" | tr -s ' \n' '  ')
if [[ $hex == *" e2 96 88 62 3f 63 "* ]]; then ok "the lone mark written as '?' keeps the c in column 4"; else bad "lone mark written as: $hex"; fi
expect "shade --output-encoding utf-8 warns of a lone mark" 1 "lone-mark\.ans: 1 cell holds a zero-width glyph" -- \
  "$cli" shade --ramp steel --output-encoding utf-8 "$tmp/lone-mark.ans" "$tmp/lone-mark-shade.ans"
expect "a glyph with no CP437 code says so" 1 \
  "warning: .*unwritable\.bwp: 1 glyph has no CP437 code and is written as '\?' \(first: U\+2605 '★' at column 1, row 1\)$" -- \
  "$cli" convert "$data/projects/unwritable.bwp" "$tmp/unwritable.ans"
# SAUCE is only what follows the cells: cells that spell it are cells, and
# a last 0x1A attribute right before the record is a cell, not its EOF
{ printf 'XBIN\x1a\x40\x00\x01\x00\x10\x08SAUCE00'; head -c 121 /dev/zero; } > "$tmp/spelled.xb"
expect "info: cells that spell SAUCE are no SAUCE" 0 '^sauce: +none$' -- "$cli" info "$tmp/spelled.xb"
{
  printf 'XBIN\x1a\x01\x00\x01\x00\x10\x08A\x1a'
  printf 'SAUCE00%-35s%-20s%-20s20261030' '' '' ''
  printf '\x0d\x00\x00\x00\x06\x00\x01\x00\x01\x00\x00\x00\x00\x00\x00\x00'
  head -c 22 /dev/zero
} > "$tmp/last-1a.xb"
expect "info: a last 0x1A attribute is content" 0 '^filesize: +13 \(matches content\)$' -- "$cli" info "$tmp/last-1a.xb"
printf 'XBIN\x1a\x00\x00\x01\x00\x10\x00' > "$tmp/zero.xb"
expect "XBIN of width 0 refused" 1 'zero\.xb: .*width 0' -- "$cli" info "$tmp/zero.xb"
expect "--help names XBIN and BIN" 0 'XBIN \(\.xb\) or BIN \(\.bin\)' -- "$cli" --help
expect "--help says export and render refuse" 0 'render refuse to write over their input' -- "$cli" --help

# export --irc: per-line byte report, the budget, and the override
expect "export --irc" 0 '^17 lines, longest 79 bytes, 84 sent, budget 407: 0 over$' -- "$cli" export --irc "$piece" "$tmp/tux.txt"
if cmp -s "$data/golden-irc/tux.txt" "$tmp/tux.txt"; then ok "export --irc output is golden-irc's"; else bad "export --irc output differs from golden-irc/tux.txt"; fi
expect "export --irc reports every line as written and as sent" 0 '^ +13 +79 +84$' -- "$cli" export --irc "$piece" "$tmp/tux.txt"
report=$("$cli" export --irc "$piece" "$tmp/tux.txt" 2>/dev/null)
if [[ $(grep -cE '^ +[0-9]+ +[0-9]+' <<<"$report") -eq 17 ]]; then ok "export --irc reports 17 lines"; else bad "export --irc report: $report"; fi
expect "over budget refused" 3 '^ +13 +79 +84  over by 24$' -- "$cli" export --irc --budget 60 "$piece" "$tmp/over.txt"
expect "over budget listed" 3 "tux\\.ans: line 13 is 84 bytes as WeeChat's ascii script sends it, over the 60-byte budget by 24" -- "$cli" export --irc --budget 60 "$piece" "$tmp/over.txt"
expect "over budget counted" 3 '^17 lines, longest 79 bytes, 84 sent, budget 60: 8 over$' -- "$cli" export --irc --budget 60 "$piece" "$tmp/over.txt"
expect "over budget says why" 3 'over\.txt: not written: lines over the budget \(--allow-over-budget writes it anyway\)' -- "$cli" export --irc --budget 60 "$piece" "$tmp/over.txt"
if [[ -e $tmp/over.txt ]]; then bad "refused export left an output file"; else ok "refused export writes nothing"; fi
expect "--allow-over-budget writes" 0 'line 13 is 84 bytes .*, over the 60-byte budget' -- "$cli" export --irc --budget 60 --allow-over-budget "$piece" "$tmp/over.txt"
if cmp -s "$data/golden-irc/tux.txt" "$tmp/over.txt"; then ok "--allow-over-budget writes the lines uncut"; else bad "--allow-over-budget output differs"; fi
# 64 cells, each a different colour from its neighbour: 352 bytes written,
# over 407 as the ascii script sends it, with two-digit colour codes
for i in $(seq 0 63); do printf '\033[%d;%dmx' $((30 + i % 8)) $((40 + (i + 3) % 8)); done > "$tmp/rainbow.ans"
expect "default budget is 407" 3 '^1 line, longest 352 bytes, 448 sent, budget 407: 1 over$' -- "$cli" export --irc "$tmp/rainbow.ans" "$tmp/rainbow.txt"
expect "--budget raises it" 0 'budget 512: 0 over$' -- "$cli" export --irc --budget 512 "$tmp/rainbow.ans" "$tmp/rainbow.txt"
# --fit: colours merged until every line is in the budget; text left alone
for i in $(seq 0 79); do printf '\033[%d;%dm\xdf' $((30 + i % 8)) $((40 + (i + 3) % 8)); done > "$tmp/halves.ans"
expect "half blocks over the budget" 3 '^1 line, longest 600 bytes, 720 sent, budget 407: 1 over$' -- "$cli" export --irc "$tmp/halves.ans" "$tmp/halves.txt"
expect "--fit fits them" 0 '^1 line, longest [0-9]+ bytes, [0-9]+ sent, budget 407: 0 over$' -- "$cli" export --irc --fit "$tmp/halves.ans" "$tmp/halves.txt"
expect "--fit marks the line it merged" 0 '^ +1 +[0-9]+ +[0-9]+  fit: [1-9][0-9]* cells merged$' -- "$cli" export --irc --fit "$tmp/halves.ans" "$tmp/halves.txt"
expect "--fit counts what it merged" 0 '^fit: [1-9][0-9]* cells merged in 1 line$' -- "$cli" export --irc --fit "$tmp/halves.ans" "$tmp/halves.txt"
if [[ $(wc -c < "$tmp/halves.txt") -le 408 ]]; then ok "--fit writes the fitted line"; else bad "--fit wrote $(wc -c < "$tmp/halves.txt") bytes"; fi
expect "--fit takes the budget" 0 'budget 300: 0 over$' -- "$cli" export --irc --fit --budget 300 "$tmp/halves.ans" "$tmp/halves.txt"
expect "--fit never changes text" 3 '^fit: 0 cells merged in 0 lines$' -- "$cli" export --irc --fit "$tmp/rainbow.ans" "$tmp/rainbow.txt"
expect "--fit leaves a piece in budget alone" 0 '^fit: 0 cells merged in 0 lines$' -- "$cli" export --irc --fit "$piece" "$tmp/tux-fit.txt"
if cmp -s "$data/golden-irc/tux.txt" "$tmp/tux-fit.txt"; then ok "--fit of a piece in budget is golden-irc's"; else bad "--fit changed a piece in budget"; fi
# 80 and 67 ★ in bright red and red by turns, on black: 402 and 337 bytes
# written, which the ascii script sends in 483 and 405 (#364)
for n in 80 67; do
  for i in $(seq 1 $n); do
    if [ $((i % 2)) -eq 1 ]; then printf '\033[1;31;40m\342\230\205'; else printf '\033[0;31;40m\342\230\205'; fi
  done > "$tmp/stars$n.ans"
done
expect "80 stars written in budget are sent over it" 3 '^ +1 +402 +483  over by 76$' -- "$cli" export --irc "$tmp/stars80.ans" "$tmp/stars80.txt"
expect "80 stars refused" 3 "stars80\\.ans: line 1 is 483 bytes as WeeChat's ascii script sends it, over the 407-byte budget by 76" -- "$cli" export --irc "$tmp/stars80.ans" "$tmp/stars80.txt"
if [[ -e $tmp/stars80.txt ]]; then bad "80 stars: refused export left a file"; else ok "80 stars: nothing written"; fi
expect "67 stars are sent in budget" 0 '^1 line, longest 337 bytes, 405 sent, budget 407: 0 over$' -- "$cli" export --irc "$tmp/stars67.ans" "$tmp/stars67.txt"
expect "--fit leaves 80 stars over: their text can't change" 3 '^fit: 0 cells merged in 0 lines$' -- "$cli" export --irc --fit "$tmp/stars80.ans" "$tmp/stars80.txt"
# --pad: blank rows and trailing black kept, each line ended on a black block
printf '\r\n\033[31mx\r\n' > "$tmp/edge.ans"
expect "export drops a blank top row" 0 '^1 line, longest 5 bytes' -- "$cli" export --irc "$tmp/edge.ans" "$tmp/edge.txt"
expect "--pad keeps it" 0 '^2 lines, longest 88 bytes' -- "$cli" export --irc --pad "$tmp/edge.ans" "$tmp/edge.txt"
if [[ $(grep -c $'\xe2\x96\x88$' "$tmp/edge.txt") -eq 2 ]]; then ok "--pad ends every line on a block"; else bad "--pad lines: $(cat -A "$tmp/edge.txt")"; fi
# --swap-shades: ▓ on red as ░ the other way round, one colour code for the row
for i in $(seq 0 39); do printf '\033[34;41m\xb2\033[31;44m\xb0'; done > "$tmp/shade.ans"
expect "shades over the budget" 3 '^1 line, longest 560 bytes' -- "$cli" export --irc "$tmp/shade.ans" "$tmp/shade.txt"
expect "--swap-shades" 0 '^1 line, longest 244 bytes, 246 sent, budget 407: 0 over$' -- "$cli" export --irc --swap-shades "$tmp/shade.ans" "$tmp/shade.txt"
expect "export needs --irc" 2 'export needs a format: --irc' -- "$cli" export "$piece" "$tmp/x.txt"
expect "export arity" 2 'export takes' -- "$cli" export --irc "$piece"
expect "--budget must be a number" 2 '--budget takes 1 to 512 bytes' -- "$cli" export --irc --budget lots "$piece" "$tmp/x.txt"
expect "--budget must be positive" 2 '--budget takes 1 to 512 bytes' -- "$cli" export --irc --budget 0 "$piece" "$tmp/x.txt"
# the range the app's Budget dialog and a project take: an IRC line is 512
# bytes, CR LF included (#303)
expect "--budget 1 is taken" 3 'budget 1: [0-9]+ over$' -- "$cli" export --irc --budget 1 "$piece" "$tmp/x.txt"
for b in 513 600 99999 -1 18446744073709551617; do
  expect "export --budget $b is refused" 2 '--budget takes 1 to 512 bytes' -- "$cli" export --irc --budget "$b" "$piece" "$tmp/x.txt"
  expect "render --irc --budget $b is refused" 2 '--budget takes 1 to 512 bytes' -- "$cli" render --irc --budget "$b" "$piece" "$tmp/x.png"
done
if [[ -e $tmp/x.txt || -e $tmp/x.png ]]; then bad "a refused --budget wrote a file"; else ok "a refused --budget writes nothing"; fi
expect "render --irc --budget 512 is taken" 0 '' -- "$cli" render --irc --budget 512 "$piece" "$tmp/x.png"
expect "render --irc --budget 0 is refused" 2 '--budget takes 1 to 512 bytes' -- "$cli" render --irc --budget 0 "$piece" "$tmp/x.png"
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
# the editor opens it: its blinking red blocks lose the blink there, so they
# are on the ramp, and the save counts the ones the shade leaves as they were.
# A shaded block is written afresh as a rung, so it no longer counts.
printf '\033[0;5;31;40m\xdb\xdb\033[0;104mA' > "$tmp/inferred-blink.ans"; sauce "$tmp/inferred-blink.ans" 0
expect "shade of an inferred-iCE piece refuses the blink lost on open" 1 'inferred-blink-up\.ans: not written: the conversion loses data \(--allow-lossy writes it anyway\)' -- \
  "$cli" shade --ramp fire --box 0,0,0,0 "$tmp/inferred-blink.ans" "$tmp/inferred-blink-up.ans"
expect "shade of an inferred-iCE piece shades its blinking block" 0 '^lightened 1 cell 1 rung along fire in 0,0-0,0$' -- \
  "$cli" shade --ramp fire --box 0,0,0,0 --allow-lossy "$tmp/inferred-blink.ans" "$tmp/inferred-blink-up.ans"
expect "shade of every lost cell of an inferred-iCE piece is not refused" 0 '^lightened 2 cells 1 rung along fire in 0,0-1,0$' -- \
  "$cli" shade --ramp fire --box 0,0,1,0 "$tmp/inferred-blink.ans" "$tmp/inferred-blink-all.ans"
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

# shade modes (#107): the form, despeckle and rim-light brushes over a box or
# a mask. black.ans is 80x12 of black; block.ans the same with a w block at
# 10,2-29,9, which is also its mask.
for y in $(seq 12); do printf '\033[0m%80s' ''; done > "$tmp/black.ans"
for y in $(seq 0 11); do
  if (( y >= 2 && y <= 9 )); then printf '\033[0m%10s\033[0;37m' ''; printf '\xdb%.0s' $(seq 20); printf '\033[0m%50s' ''
  else printf '\033[0m%80s' ''; fi
done > "$tmp/block.ans"
expect "--help lists the shade modes" 0 'shade --form dome\|cylinder\|plane' -- "$cli" --help
expect "shade --form dome" 0 '^shaded [1-9][0-9]* cells as a dome lit from NW along steel in 10,2-29,9$' -- \
  "$cli" shade --form dome --ramp steel --mask "$tmp/block.ans" "$tmp/black.ans" "$tmp/dome.ans"
expect "shade --form adds no finding" 0 '^ladder: [0-9]+ of 160 cells on the ramp, [0-9]+ shaded, noise [0-9.]+: clean$' -- \
  "$cli" shade --form dome --ramp steel --mask "$tmp/block.ans" "$tmp/black.ans" "$tmp/dome.ans"
expect "shade --form by box" 0 '^shaded [1-9][0-9]* cells as a cylinder lit from SE along steel in 10,2-29,9$' -- \
  "$cli" shade --form cylinder --light se --ramp steel --box 10,2,29,9 "$tmp/black.ans" "$tmp/cyl.ans"
expect "shade --form plane --glint" 0 '^shaded [1-9][0-9]* cells as a plane lit from N along steel in 12,2-29,5$' -- \
  "$cli" shade --form plane --glint --light N --ramp steel --mask "$tmp/block.ans" --box 12,0,40,5 "$tmp/black.ans" "$tmp/plane.ans"
expect "shade --despeckle" 0 '^despeckled [0-9]+ cells? along steel in 0,0-71,33$' -- \
  "$cli" shade --despeckle --ramp steel --box 0,0,100,100 "$sphere" "$tmp/despeckled.ans"
expect "shade --rim" 0 '^rim-lit [1-9][0-9]* cells from NW along steel in 10,2-29,9$' -- \
  "$cli" shade --rim --ramp steel --mask "$tmp/block.ans" "$tmp/block.ans" "$tmp/rim.ans"
expect "shade --rim from SE" 0 '^rim-lit [1-9][0-9]* cells from SE along steel in 10,2-29,9$' -- \
  "$cli" shade --rim --light SE --ramp steel --mask "$tmp/block.ans" "$tmp/block.ans" "$tmp/rim-se.ans"
if cmp -s "$tmp/rim.ans" "$tmp/rim-se.ans"; then bad "shade --rim from NW and SE wrote the same"; else ok "shade --rim follows --light"; fi
expect "shade mask of the wrong size" 2 "--mask .*tux\.ans is 30x17, .*sphere-steel\.ans is 72x34" -- \
  "$cli" shade --form dome --ramp steel --mask "$piece" "$sphere" "$tmp/x.ans"
expect "shade two modes" 2 'shade takes one mode: the rung step, --form, --despeckle or --rim' -- \
  "$cli" shade --form dome --rim --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --form with --darken" 2 'shade takes one mode: the rung step, --form, --despeckle or --rim' -- \
  "$cli" shade --form dome --darken --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --despeckle with --steps" 2 'shade takes one mode' -- "$cli" shade --despeckle --steps 2 --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --despeckle with --rim" 2 'shade takes one mode' -- "$cli" shade --despeckle --rim --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --form needs a selection" 2 'shade --form needs a selection: --box, --mask or both' -- \
  "$cli" shade --form dome --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --despeckle needs a selection" 2 'shade --despeckle needs a selection' -- "$cli" shade --despeckle --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --rim needs a selection" 2 'shade --rim needs a selection' -- "$cli" shade --rim --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --form needs a ramp" 2 'shade needs a ramp' -- "$cli" shade --form dome "$sphere" "$tmp/x.ans"
expect "shade --despeckle needs a ramp" 2 'shade needs a ramp' -- "$cli" shade --despeckle "$sphere" "$tmp/x.ans"
expect "shade --rim needs a ramp" 2 'shade needs a ramp' -- "$cli" shade --rim "$sphere" "$tmp/x.ans"
expect "shade bad form" 2 "--form takes dome, cylinder or plane, not 'cone'" -- "$cli" shade --form cone --ramp steel "$sphere" "$tmp/x.ans"
expect "shade bad light" 2 "--light takes a compass point \(N, NE, E, SE, S, SW, W, NW\), not 'up'" -- \
  "$cli" shade --form dome --light up --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --glint without --form" 2 '--glint is a shade --form option' -- "$cli" shade --rim --glint --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --light without --form or --rim" 2 '--light is a shade --form or --rim option' -- \
  "$cli" shade --despeckle --light N --ramp steel "$sphere" "$tmp/x.ans"
expect "shade --light on the rung step" 2 '--light is a shade --form or --rim option' -- "$cli" shade --light N --ramp steel "$sphere" "$tmp/x.ans"
expect "--mask is a shade option" 2 '--mask is a shade option, not a lint option' -- "$cli" lint --mask "$sphere" "$sphere"
expect "shade missing mask" 1 'nope\.ans: No such file' -- "$cli" shade --rim --ramp steel --mask "$tmp/nope.ans" "$sphere" "$tmp/x.ans"
if [[ -e $tmp/x.ans ]]; then bad "a refused shade mode wrote $tmp/x.ans"; else ok "refused shade modes write nothing"; fi

# lint: the whole lint, one planted fault a piece (#106)
expect "--help lists lint" 0 '^  lint <in> +the ladder check' -- "$cli" --help
expect "usage lists lint" 2 'lint \[--ramp R\]\.\.\. \[--box x0,y0,x1,y1\] \[--target ansi\|irc\] \[--json\] <in>' -- "$cli" lint
k='\033[0;40m '; K='\033[0;1;30;40m\xdb'; w='\033[0;37;40m\xdb'
s1='\033[0;1;30;40m\xb0'; s2='\033[0;1;30;40m\xb1'; s3='\033[0;1;30;40m\xb2'; Wk='\033[0;1;37;40m\xb1'
cells() { local o=; for c in "$@"; do o+=$c; done; printf "$o"; }
cells "$s1" "$s2" "$s3" "$K" "$w" > "$tmp/lint-skip.ans"
for y in 0 1 2 3; do cells "$k" "$s1" "$s2" "$s2" "$s2" "$s2" "$s2" "$s2" "$s3" "$K"; printf '\r\n'; done > "$tmp/lint-wide.ans"
{ cells "$K" "$K" "$K" "$K" "$K"; printf '\r\n'; cells "$K" "$K" "$w" "$K" "$K"; printf '\r\n'; cells "$K" "$K" "$K" "$K" "$K"; } > "$tmp/lint-speck.ans"
cells "$k" "$Wk" "$k" > "$tmp/lint-shade.ans"
cells "$K" "$K" "$K" "$K" > "$tmp/lint-clean.ans"
expect "lint: a skip" 4 '^3,0 rung-skip steel cell 3,0: rung 4 next to rung 8 at cell 4,0 \(more than one rung apart\)$' -- "$cli" lint --ramp steel "$tmp/lint-skip.ans"
expect "lint: a wide rung" 4 '^2,3 wide-rung steel cell 2,3: a rung 4 cells wide \(at most 3\)$' -- "$cli" lint --ramp steel "$tmp/lint-wide.ans"
expect "lint: a speck" 4 '^2,1 speckle steel cell 2,1: a speck, lighter than every neighbour$' -- "$cli" lint --ramp steel "$tmp/lint-speck.ans"
expect "lint: a non-neighbour shade" 4 "^1,0 non-neighbour-shade steel cell 1,0: ▒ of 15 on 0, colours no ramp linted has as neighbours$" -- "$cli" lint --ramp steel "$tmp/lint-shade.ans"
expect "lint: the summary" 4 '^4 findings \(4 wide-rung\), noise 0\.0000, iCE on$' -- "$cli" lint --ramp steel "$tmp/lint-wide.ans"
expect "lint: a skip as JSON" 4 '^\{"findings":\[\{"kind":"rung-skip","severity":"warning","ramp":"steel","x":3,"y":0,"x2":4,"y2":0,"message":"cell 3,0: rung 4 next to rung 8 at cell 4,0 \(more than one rung apart\)"\}\],"noise":0\.0000,"ice":true\}$' -- \
  "$cli" lint --json --ramp steel "$tmp/lint-skip.ans"
expect "lint: a wide rung as JSON" 4 '"kind":"wide-rung","severity":"warning","ramp":"steel","x":2,"y":3,"x2":7,"y2":3,' -- "$cli" lint --json --ramp steel "$tmp/lint-wide.ans"
expect "lint: a speck as JSON" 4 '^\{"findings":\[\{"kind":"speckle","severity":"warning","ramp":"steel","x":2,"y":1,"x2":2,"y2":1,' -- "$cli" lint --json --ramp steel "$tmp/lint-speck.ans"
expect "lint: a non-neighbour shade as JSON" 4 '^\{"findings":\[\{"kind":"non-neighbour-shade","severity":"warning","ramp":"steel","x":1,"y":0,"x2":1,"y2":0,' -- "$cli" lint --json --ramp steel "$tmp/lint-shade.ans"
# a shade of neighbours on one named ramp is no fault under the others: LightRed over Red is fire's
cells '\033[0;1;31;41m\xb0' '\033[0;1;31;41m\xb0' '\033[0;1;31;41m\xb1' '\033[0;1;31;41m\xb1' '\033[0;1;31;41m\xb2' '\033[0;1;31;41m\xb2' > "$tmp/lint-fire.ans"
expect "lint: a fire pair under every ramp" 0 '^clean' -- "$cli" lint "$tmp/lint-fire.ans"
expect "lint: a fire pair under fire and skin" 0 '^clean' -- "$cli" lint --ramp skin --ramp fire "$tmp/lint-fire.ans"
expect "lint: a fire pair under skin alone" 4 '^0,0 non-neighbour-shade skin cell 0,0: ░ of 12 on 4, colours no ramp linted has as neighbours$' -- "$cli" lint --ramp skin "$tmp/lint-fire.ans"
# hand-shading ladders: brown into black (warm skin), a hero colour into black (Y▓k)
printf '\033[0;33;40m\260\260\261\261\262\262\033[0m\r\n' > "$tmp/lint-brown-k.ans"
printf '\033[1;33;40m\260\260\261\261\262\262\033[0m\r\n' > "$tmp/lint-yellow-k.ans"
expect "lint: brown into black by default" 0 '^clean' -- "$cli" lint "$tmp/lint-brown-k.ans"
expect "lint: yellow into black by default" 0 '^clean' -- "$cli" lint "$tmp/lint-yellow-k.ans"
expect "lint: brown into black under skin alone" 4 '^0,0 non-neighbour-shade skin cell 0,0: ░ of 6 on 0, ' -- "$cli" lint --ramp skin "$tmp/lint-brown-k.ans"
expect "lint: clean" 0 '^clean, noise 0\.0000, iCE on$' -- "$cli" lint "$tmp/lint-clean.ans"
expect "lint: clean as JSON" 0 '^\{"findings":\[\],"noise":0\.0000,"ice":true\}$' -- "$cli" lint --json "$tmp/lint-clean.ans"
# every named ramp by default; a finding under two of them is reported once, under the first
expect "lint: every ramp by default" 4 "^1,0 non-neighbour-shade fire " -- "$cli" lint "$tmp/lint-shade.ans"
expect "lint: once under two ramps" 4 '^1 finding \(1 non-neighbour-shade\)' -- "$cli" lint "$tmp/lint-shade.ans"
expect "lint: once under a repeated ramp" 4 '^1 finding \(1 speckle\)' -- "$cli" lint --ramp steel --ramp 0,8,7,15 "$tmp/lint-speck.ans"
expect "lint: --ramp repeats" 4 '^1,0 non-neighbour-shade 0,8,15 ' -- "$cli" lint --ramp 0,8,15 --ramp steel "$tmp/lint-shade.ans"
expect "lint: other rules have no ramp" 4 '^[0-9]+,[0-9]+ trailing-black - ' -- "$cli" lint --ramp steel "$piece"
# --box: only findings with a cell in the box, the whole canvas still read
expect "lint: --box keeps a finding in it" 4 '^2,1 speckle steel ' -- "$cli" lint --ramp steel --box 2,1,4,2 "$tmp/lint-speck.ans"
expect "lint: --box drops one outside it" 0 '^clean' -- "$cli" lint --ramp steel --box 0,0,1,2 "$tmp/lint-speck.ans"
expect "lint: --box corners either way" 4 '^2,1 speckle steel ' -- "$cli" lint --ramp steel --box 4,2,2,1 "$tmp/lint-speck.ans"
expect "lint: --box past the edge is clipped" 4 '^2,1 speckle steel ' -- "$cli" lint --ramp steel --box 2,1,90,90 "$tmp/lint-speck.ans"
expect "lint: a speck at the box's edge is read with what is outside" 4 '^2,1 speckle steel ' -- "$cli" lint --ramp steel --box 2,1,2,1 "$tmp/lint-speck.ans"
expect "lint: --box off the canvas" 1 'the box 0,3,9,9 is off the 80x3 canvas' -- "$cli" lint --box 0,3,9,9 "$tmp/lint-speck.ans"
# iCE comes from SAUCE
cells "$k" "$Wk" "$k" > "$tmp/lint-noice.ans"; sauce "$tmp/lint-noice.ans" 0
expect "lint: iCE off from SAUCE" 4 'iCE off$' -- "$cli" lint "$tmp/lint-noice.ans"
expect "lint: iCE off as JSON" 4 '"ice":false\}$' -- "$cli" lint --json "$tmp/lint-noice.ans"
# the errors are shade's
expect "lint unknown ramp" 2 "--ramp takes a ramp name \(fire, .*\) or palette indices 0-15 dark to light, not 'blood'" -- "$cli" lint --ramp blood "$piece"
expect "lint ramp out of the palette" 2 "not '0,16'" -- "$cli" lint --ramp 0,16 "$piece"
expect "lint ramp of one colour" 2 '--ramp 7: a ramp needs at least two colours' -- "$cli" lint --ramp 7 "$piece"
expect "lint ramp with a repeat" 2 '--ramp 0,8,0: colour 0 appears twice' -- "$cli" lint --ramp 0,8,0 "$piece"
expect "lint bad box" 2 "--box takes x0,y0,x1,y1 .*not '1,2,3'" -- "$cli" lint --box 1,2,3 "$piece"
expect "lint arity" 2 'lint takes one file' -- "$cli" lint "$piece" "$piece"
expect "lint needs a file" 2 'lint takes one file' -- "$cli" lint
expect "lint bad input" 1 'nope\.ans: No such file' -- "$cli" lint "$tmp/nope.ans"

# Every command refuses every option it doesn't use: exit 2, a usage message
# naming the option and the command, and nothing written.
declare -A owner=([--allow-lossy]="convert shade pack unpack snapshot replay" [--scale]="render replay"
                  [--font-width]="render replay"
                  [--irc]="export render" [--budget]="export render" [--allow-over-budget]=export
                  [--fit]="export render" [--pad]="export render" [--swap-shades]="export render"
                  [--ramp]="shade lint" [--darken]=shade [--steps]=shade [--box]="shade lint" [--ice]=shade
                  [--json]=lint)
declare -A value=([--scale]=2 [--font-width]=9 [--budget]=60 [--ramp]=steel [--steps]=2 [--box]=0,0,1,1)
foreign=$tmp/foreign && mkdir -p "$foreign"
for cmd in info convert render export shade lint pack unpack snapshot replay; do
  for opt in --allow-lossy --scale --font-width --irc --budget --allow-over-budget --fit --pad --swap-shades \
             --ramp --darken --steps --box --ice --json; do
    [[ " ${owner[$opt]} " == *" $cmd "* ]] && continue
    out=$foreign/$cmd${opt}.out
    case $cmd in
      info) argv=(info "$piece") ;;
      snapshot) argv=(snapshot "$piece") ;;
      export) argv=(export --irc "$piece" "$out") ;;
      shade) argv=(shade --ramp steel "$piece" "$out") ;;
      lint) argv=(lint "$piece") ;;
      replay) argv=(replay "$data/replay/tour.json" -o "$out") ;;
      *) argv=("$cmd" "$piece" "$out") ;;
    esac
    read -ra o <<<"${owner[$opt]}"; a=${o[-1]}
    (( ${#o[@]} > 1 )) && a="$(IFS=,; echo "${o[*]:0:${#o[@]}-1}" | sed 's/,/, /g') or $a"
    [[ $a == export* ]] && a="an $a" || a="a $a"
    expect "$cmd rejects $opt" 2 "^blockwright-cli: $opt is $a option, not a $cmd option$" -- \
      "$cli" "${argv[@]:0:1}" "$opt" ${value[$opt]:+"${value[$opt]}"} "${argv[@]:1}"
    if [[ -e $out ]]; then bad "$cmd with $opt wrote $out"; fi
  done
done
out=$("$cli" info --scale 2 "$piece" 2>/dev/null)
if [[ -z $out ]]; then ok "info with a foreign option prints no info"; else bad "info with a foreign option printed: $out"; fi
if [[ -z $(ls -A "$foreign") ]]; then ok "commands with a foreign option write nothing"; else bad "foreign options wrote: $(ls -A "$foreign")"; fi
expect "options after the files are checked too" 2 '^blockwright-cli: --allow-lossy is a convert, shade, pack, unpack, snapshot or replay option, not a render option$' -- \
  "$cli" render "$piece" "$foreign/late.png" --allow-lossy

expect "export bad input" 1 'nope\.ans: No such file' -- "$cli" export --irc "$tmp/nope.ans" "$tmp/x.txt"
expect "export unwritable output" 1 'no/such/dir/out\.txt' -- "$cli" export --irc "$piece" "$tmp/no/such/dir/out.txt"

# Out of memory is a one-line error and exit 1, not std::terminate: a 4 GiB
# sparse input read under a 512 MiB address-space limit. Skipped for a
# Windows build (ulimit -v would limit wine, not a Windows program's memory)
# and on macOS, whose kernel refuses RLIMIT_AS, RLIMIT_DATA and RLIMIT_RSS.
if [[ -z ${BW_CLI_WINDOWS:-} ]] && bash -c 'ulimit -v 524288' 2>/dev/null; then
  truncate -s 4G "$tmp/huge.ans"
  expect "out of memory" 1 '^blockwright-cli: out of memory$' -- bash -c 'ulimit -v 524288 && exec "$@"' _ "$cli" info "$tmp/huge.ans"
  out=$(bash -c 'ulimit -v 524288 && exec "$@"' _ "$cli" info "$tmp/huge.ans" 2>&1)
  if [[ $(wc -l <<<"$out") -eq 1 ]]; then ok "out of memory prints one line"; else bad "out of memory printed: $out"; fi
  rm -f "$tmp/huge.ans"
else
  echo "skip out of memory: no address-space limit can be set here"
fi

expect "no command" 2 'no command' -- "$cli"
expect "unknown command" 2 "unknown command 'frobnicate'" -- "$cli" frobnicate
expect "convert arity" 2 'convert takes' -- "$cli" convert "$piece"
expect "info arity" 2 'info takes' -- "$cli" info
expect "unknown option" 2 'Unknown option' -- "$cli" --bogus

# library list / check
lib=$data/library
expect "library check: a good library" 0 '^2 stamps, 0 text styles, 0 problems$' -- "$cli" library check "$lib"
expect "library list: tab-separated" 0 $'^stamp\tsky\tSmall star\t5x3\t14\t-\tstar,night\t.*[/\\\\]sky[/\\\\]small-star\\.ans$' -- "$cli" library list "$lib"
expect "library list: sorted by path" 0 $'^stamp\tfigures\tTux\t30x17\t79\t-\tpenguin,linux\t' -- bash -c '"$1" library list "$2" | head -1' _ "$cli" "$lib"
expect "library list: no dir lists the bundled and user libraries" 0 $'^stamp\tsky\tSmall star\t' -- env BLOCKWRIGHT_BUNDLED_LIBRARY="$lib" BLOCKWRIGHT_USER_LIBRARY="$tmp/no-user-lib" "$cli" library list
expect "library check: missing dir" 1 'nope: not a directory' -- "$cli" library check "$tmp/nope"
expect "library check: needs a dir" 2 'library check takes one directory' -- "$cli" library check
expect "library: needs a subcommand" 2 'library needs a subcommand' -- "$cli" library
expect "library: unknown subcommand" 2 "unknown library subcommand 'frob'" -- "$cli" library frob
expect "library: refuses other commands' options" 2 '--scale is a render or replay option, not a library option' -- "$cli" library list --scale 2
bl=$tmp/badlib; mkdir -p "$bl"
cp "$lib/sky/small-star.ans" "$bl/bytes.ans"; sed 's/"ircBytes": 14/"ircBytes": 15/' "$lib/sky/small-star.json" > "$bl/bytes.json"
cp "$lib/sky/small-star.ans" "$bl/key.ans"; sed 's/"tags"/"tag"/' "$lib/sky/small-star.json" > "$bl/key.json"
cp "$lib/sky/small-star.ans" "$bl/nosidecar.ans"
cp "$lib/sky/small-star.json" "$bl/orphan.json"
printf '\033[0;33mx\033[0m\r\n' > "$bl/nosauce.ans"; cp "$lib/sky/small-star.json" "$bl/nosauce.json"
expect "library check: wrong byte count" 1 'bytes\.ans: sidecar says the widest row is 15 IRC bytes; the art.s is 14' -- "$cli" library check "$bl"
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
# bom.ans: a byte order mark (a UTF-8 stamp) under a sidecar with no "encoding"
{ printf '\357\273\277'; cat "$lib/sky/small-star.ans"; } > "$bl/bom.ans"; cp "$lib/sky/small-star.json" "$bl/bom.json"
problems=8
if [[ -z ${BW_CLI_WINDOWS:-} ]]; then  # (a Windows program sees no Linux link)
  mkdir -p "$tmp/shared"; ln -s "$tmp/shared" "$bl/linked"; ln -s "$bl/gone.ans" "$bl/dangling.ans"
  problems=10
fi
expect "library check: a comma in a tag" 1 'comma\.ans: a sidecar tag holds a comma' -- "$cli" library check "$bl"
expect "library check: an all-blank stamp" 1 'blank\.ans: \.ans has no drawn cell' -- "$cli" library check "$bl"
expect "library check: a byte order mark with no encoding key" 1 'bom\.ans: \.ans starts with a byte order mark, but its sidecar has no "encoding": "utf-8"' -- "$cli" library check "$bl"
expect "library list: a byte order mark with no encoding key is a warning" 0 'warning: .*bom\.ans: \.ans starts with a byte order mark' -- "$cli" library list "$bl"
if [[ $problems == 10 ]]; then
  expect "library check: a linked folder" 1 'linked: symbolic link to a folder, not followed' -- "$cli" library check "$bl"
  expect "library check: a broken link" 1 'dangling\.ans: broken symbolic link, skipped' -- "$cli" library check "$bl"
  expect "library list: a linked folder is a warning" 0 'warning: .*linked: symbolic link to a folder' -- "$cli" library list "$bl"
fi
expect "library check: counts" 1 "^0 stamps, 0 text styles, $problems problems\$" -- "$cli" library check "$bl"
expect "library list: problems are warnings" 0 'warning: .*orphan\.json: sidecar with no orphan\.ans' -- "$cli" library list "$bl"
b2=$tmp/badlib2; mkdir -p "$b2"
cp "$lib/sky/small-star.ans" "$b2/dup.ans"
printf '{"category":7,"category":"sky","tags":[],"ircBytes":9999,"ircBytes":14}\n' > "$b2/dup.json"
mkfifo "$b2/pipe.ans"
expect "library check: a repeated key" 1 'dup\.ans: sidecar has the key "category" more than once' -- timeout 10 "$cli" library check "$b2"
expect "library check: a FIFO is never opened" 1 'pipe\.ans: not a regular file, skipped' -- timeout 10 "$cli" library check "$b2"
# a UTF-8 stamp of 80 ★ in bright red and red by turns, on black, 80x1: its
# row is written in 402 bytes, which its sidecar records, but WeeChat's
# ascii script sends it in 483 (#364)
u8l=$tmp/u8lib; mkdir -p "$u8l"
{
  printf '\357\273\277\033[0m'
  i=0
  while [ "$i" -lt 40 ]; do printf '\033[0;1;31m\342\230\205\033[0;31m\342\230\205'; i=$((i + 1)); done
  printf '\033[0m\032SAUCE00%-35s%-20s%-20s20261105' 'Stars' '' ''
  printf '\000\000\000\000\001\001\120\000\001\000\000\000\000\000\000\001'
  printf '%022d' 0 | tr 0 '\0'
} > "$u8l/stars.ans"
printf '{"category": "sky", "tags": [], "ircBytes": 402, "encoding": "utf-8"}\n' > "$u8l/stars.json"
expect "library list: a UTF-8 stamp written in 402 bytes" 0 $'^stamp\tsky\tStars\t80x1\t402\t' -- "$cli" library list "$u8l"
expect "library check: a stamp sent over the budget" 1 'stars\.ans: the widest row is 483 IRC bytes as WeeChat.s ascii script sends it \(402 as written\), over the 407-byte budget' -- "$cli" library check "$u8l"
expect "library check: a stamp sent over the budget is one problem" 1 '^1 stamp, 0 text styles, 1 problem$' -- "$cli" library check "$u8l"
# text styles: <slug>.style.json, by its name and its kind
sl=$tmp/stylelib; mkdir -p "$sl/text"
cp "$lib/sky/small-star."* "$sl/"
cat > "$sl/text/gold.style.json" <<'JSON'
{
  "kind": "text-style",
  "name": "Gold",
  "category": "classic",
  "tags": ["gold", "3d"],
  "effects": [
    {"kind": "shadow", "enabled": true, "colour": 4, "direction": "se", "distance": 1, "form": "extrude"},
    {"kind": "fill", "enabled": true, "colour": 14, "mode": "flat", "ramp": "fire"}
  ]
}
JSON
cat > "$sl/text/hot.style.json" <<'JSON'
{"kind": "text-style", "name": "Hot", "category": "classic", "tags": ["fire"], "ice": "required",
 "effects": [{"kind": "fill", "enabled": true, "colour": 7, "mode": "rows", "ramp": "fire"}]}
JSON
expect "library check: text styles" 0 '^1 stamp, 2 text styles, 0 problems$' -- "$cli" library check "$sl"
s1=$tmp/stylelib1; mkdir -p "$s1"
cp "$sl/text/gold.style.json" "$s1/"; cp "$lib/sky/small-star.json" "$s1/orphan.json"
expect "library check: one of each in the singular" 1 '^0 stamps, 1 text style, 1 problem$' -- "$cli" library check "$s1"
expect "library list: a text style's kind and sample" 0 $'^text-style\tclassic\tGold\t[0-9]+x[0-9]+\t[0-9]+\t-\tgold,3d\t.*[/\\\\]text[/\\\\]gold\\.style\\.json$' -- "$cli" library list "$sl"
expect "library list: a text style that needs iCE colours" 0 $'^text-style\tclassic\tHot\t[0-9]+x[0-9]+\t[0-9]+\tice\tfire\t' -- "$cli" library list "$sl"
expect "library list: stamps first" 0 $'^stamp\tsky\tSmall star\t' -- bash -c '"$1" library list "$2" | head -1' _ "$cli" "$sl"
s2=$tmp/stylelib2; mkdir -p "$s2"
sed 's/"enabled": true, "colour": 4/"enabled": true, "blur": 2, "colour": 4/' "$sl/text/gold.style.json" > "$s2/typo.style.json"
sed 's/"ramp": "fire"/"ramp": "lava"/' "$sl/text/hot.style.json" > "$s2/ramp.style.json"
sed 's/"kind": "text-style",//' "$sl/text/gold.style.json" > "$s2/nokind.style.json"
sed 's/"flat"/"tiled"/' "$sl/text/gold.style.json" > "$s2/mode.style.json"
cp "$lib/sky/small-star.json" "$s2/star.style.json"; cp "$lib/sky/small-star.ans" "$s2/star.style.ans"
cat > "$s2/dupeffect.style.json" <<'JSON'
{"kind":"text-style","name":"Duplicate","category":"test","tags":[],"effects":[{"kind":"fill","enabled":true,"mode":"flat","colour":4,"colour":7,"ramp":"steel"}]}
JSON
cat > "$s2/escdup.style.json" <<'JSON'
{"kind":"text-style","name":"Escaped","category":"test","tags":[],"effects":[{"kind":"fill","enabled":true,"mode":"flat","colour":4,"\u0063olour":7,"ramp":"steel"}]}
JSON
cat > "$s2/flatramp.style.json" <<'JSON'
{"kind":"text-style","name":"Bad ramp","category":"test","tags":[],"effects":[{"kind":"fill","enabled":true,"mode":"flat","colour":7,"ramp":"does-not-exist"}]}
JSON
expect "library check: an unknown key in an effect" 1 'typo\.style\.json: text style effect 1 \(shadow\) has an unknown key "blur"' -- "$cli" library check "$s2"
expect "library check: a ramp that doesn't exist" 1 'ramp\.style\.json: text style: fill ramp "lava" is not a named ramp' -- "$cli" library check "$s2"
expect "library check: a text style without its kind" 1 'nokind\.style\.json: text style has no "kind"' -- "$cli" library check "$s2"
expect "library check: an unknown setting" 1 'mode\.style\.json: text style effect 2: unknown mode "tiled"' -- "$cli" library check "$s2"
expect "library check: a stamp named as a text style" 1 'star\.style\.ans: a stamp can.t be named star\.style\.ans' -- "$cli" library check "$s2"
expect "library check: a sidecar named as a text style is no sidecar" 1 'star\.style\.json: text style has no "kind"' -- "$cli" library check "$s2"
expect "library check: a key repeated in an effect" 1 'dupeffect\.style\.json: text style has the key "colour" more than once' -- "$cli" library check "$s2"
expect "library check: a key repeated in an effect, escaped" 1 'escdup\.style\.json: text style has the key "colour" more than once' -- "$cli" library check "$s2"
expect "library check: a flat fill's ramp that doesn't exist" 1 'flatramp\.style\.json: text style: fill ramp "does-not-exist" is not a named ramp' -- "$cli" library check "$s2"
expect "library check: text style counts" 1 '^0 stamps, 0 text styles, 9 problems$' -- "$cli" library check "$s2"
if [[ $(id -u) -ne 0 ]]; then
  b3=$tmp/badlib3; mkdir -p "$b3/locked"; cp "$lib/sky/small-star."* "$b3/"
  chmod 000 "$b3/locked"
  expect "library check: an unreadable folder" 1 'locked: folder can.t be read' -- "$cli" library check "$b3"
  expect "library list: an unreadable folder is a warning" 0 'warning: .*locked: folder can.t be read' -- "$cli" library list "$b3"
  chmod 700 "$b3/locked"
else
  echo "skip library check: an unreadable folder (running as root)"
fi

# projects (PLAN §6): pack, unpack, snapshot, and every reader taking one
proj=$tmp/proj; mkdir -p "$proj"
expect "pack an .ans" 0 '^$' -- "$cli" pack "$piece" "$proj/tux.bwp"
expect "info: a project zip" 0 '^project: +zip$' -- "$cli" info "$proj/tux.bwp"
expect "info: project size" 0 '^size: +30x17$' -- "$cli" info "$proj/tux.bwp"
expect "info: project layers" 0 '^  1  cells  30x17 at 0,0  normal  visible yes, locked no, alpha locked no  Background$' -- "$cli" info "$proj/tux.bwp"
expect "info: project SAUCE" 0 '^author: +funkpower$' -- "$cli" info "$proj/tux.bwp"
expect "convert flattens a project" 0 '^$' -- "$cli" convert "$proj/tux.bwp" "$proj/flat.ans"
if cmp -s "$piece" "$proj/flat.ans"; then ok "an .ans through a project is byte-identical"; else bad "an .ans through a project differs"; fi
expect "unpack a zip" 0 '^$' -- "$cli" unpack "$proj/tux.bwp" "$proj/tux"
if [[ -f $proj/tux/manifest.json && -f $proj/tux/layers/1.json && -f $proj/tux/layers/1.cells ]]; then
  ok "unpack writes the layout"; else bad "unpack layout: $(cd "$proj/tux" && find . | sort | tr '\n' ' ')"; fi
expect "info: a project directory" 0 '^project: +directory$' -- "$cli" info "$proj/tux"
expect "pack a directory" 0 '^$' -- "$cli" pack "$proj/tux" "$proj/again.bwp"
if cmp -s "$proj/tux.bwp" "$proj/again.bwp"; then ok "zip, directory, zip is byte-identical"; else bad "zip, directory, zip differs"; fi
"$cli" export --irc "$piece" "$proj/ans.txt" > /dev/null
expect "export reads a project" 0 ' 0 over$' -- "$cli" export --irc "$proj/tux" "$proj/proj.txt"
if cmp -s "$proj/ans.txt" "$proj/proj.txt"; then ok "export of a project matches its .ans"; else bad "export of a project differs"; fi
# a project's own byte budget is the default; --budget overrides it
cp -r "$proj/tux" "$proj/tight"
sed 's/"bytes": 407/"bytes": 60/' "$proj/tight/manifest.json" > "$proj/tight/manifest.json.new" &&
  mv "$proj/tight/manifest.json.new" "$proj/tight/manifest.json"
expect "export takes the project's budget" 3 '^17 lines, longest 79 bytes, 84 sent, budget 60: 8 over$' -- "$cli" export --irc "$proj/tight" "$proj/tight.txt"
expect "--budget overrides the project's" 0 '^17 lines, longest 79 bytes, 84 sent, budget 407: 0 over$' -- "$cli" export --irc --budget 407 "$proj/tight" "$proj/tight.txt"
expect "render reads a project" 0 '^$' -- "$cli" render "$proj/tux.bwp" "$proj/tux.png"
expect "snapshot a directory" 0 '^r1$' -- "$cli" snapshot "$proj/tux"
expect "snapshot again" 0 '^r2$' -- "$cli" snapshot "$proj/tux"
if cmp -s "$piece" "$proj/tux/snapshots/r2.ans"; then ok "a snapshot is the flattened .ans"; else bad "snapshot r2 differs from the piece"; fi
expect "snapshot a zip" 0 '^r1$' -- "$cli" snapshot "$proj/tux.bwp"
expect "info: snapshots" 0 '^snapshots: +1 \(last r1\)$' -- "$cli" info "$proj/tux.bwp"
expect "snapshot takes a project" 1 'tux\.ans: not a project' -- "$cli" snapshot "$piece"
# a project whose last round is r999999999 has no rounds left: snapshot says
# so for the project, exit 1, and the project is left as it was (#380; it
# aborted on the core error)
"$cli" unpack "$piece" "$proj/full" >/dev/null 2>&1
mkdir -p "$proj/full/snapshots" && cp "$piece" "$proj/full/snapshots/r999999999.ans"
before=$(find "$proj/full" -type f -exec cksum {} + | sort)
out=$("$cli" snapshot "$proj/full" 2>&1); st=$?
if [[ $st -eq 1 && $out == "blockwright-cli: $proj/full: no rounds left after r999999999" ]]; then
  ok "snapshot with no rounds left: exit 1, one line naming the project"
else
  bad "snapshot with no rounds left: exit $st: $out"
fi
if [[ $(find "$proj/full" -type f -exec cksum {} + | sort) == "$before" ]]; then ok "snapshot with no rounds left writes nothing"; else bad "snapshot with no rounds left changed the project"; fi
# a UTF-8 project's snapshots are UTF-8 unless --output-encoding says (#336)
"$cli" unpack "$u8" "$proj/u8" > /dev/null
"$cli" convert --output-encoding utf-8 "$proj/u8" "$proj/u8-flat.ans"
expect "snapshot a UTF-8 project" 0 '^r1$' -- "$cli" snapshot "$proj/u8"
if cmp -s "$proj/u8-flat.ans" "$proj/u8/snapshots/r1.ans"; then ok "a UTF-8 project's snapshot is UTF-8"; else bad "a UTF-8 project's snapshot isn't its UTF-8 .ans"; fi
expect "snapshot --output-encoding cp437" 0 '^r2$' -- "$cli" snapshot --output-encoding cp437 "$proj/u8"
if cmp -s "$piece" "$proj/u8/snapshots/r2.ans"; then ok "--output-encoding cp437 snapshots CP437"; else bad "snapshot --output-encoding cp437 isn't CP437"; fi
expect "snapshot --output-encoding utf-8 of a CP437 project" 0 '^r3$' -- "$cli" snapshot --output-encoding utf-8 "$proj/tux"
if cmp -s "$u8" "$proj/tux/snapshots/r3.ans"; then ok "--output-encoding utf-8 snapshots UTF-8"; else bad "snapshot --output-encoding utf-8 isn't UTF-8"; fi
expect "snapshot with a bad --output-encoding is refused" 2 "--output-encoding takes cp437 or utf-8, not 'auto'" -- "$cli" snapshot --output-encoding auto "$proj/u8"
if [[ -e $proj/u8/snapshots/r3.ans ]]; then bad "a bad --output-encoding took a snapshot"; else ok "a bad --output-encoding takes no snapshot"; fi
expect "pack takes --allow-lossy" 0 '^$' -- "$cli" pack --allow-lossy "$piece" "$proj/x.bwp"
cp "$proj/tux.bwp" "$proj/keep.bwp"
expect "a flat file never replaces a project" 1 'keep\.bwp: not written: convert writes \.ans, \.xb or \.bin; use pack for a project' -- "$cli" convert "$piece" "$proj/keep.bwp"
if cmp -s "$proj/tux.bwp" "$proj/keep.bwp"; then ok "the project is untouched"; else bad "convert wrote over a project"; fi
# by any name: a project zip named .ans, and a project directory
cp "$proj/tux.bwp" "$proj/keep.ans"
expect "a flat file never replaces a project named .ans" 1 'keep\.ans: not written: it is a project' -- "$cli" convert "$piece" "$proj/keep.ans"
if cmp -s "$proj/tux.bwp" "$proj/keep.ans"; then ok "the project named .ans is untouched"; else bad "convert wrote over a project named .ans"; fi
expect "a flat file never replaces a project directory" 1 'tux: not written: it is a project' -- "$cli" convert "$piece" "$proj/tux"
mkdir -p "$proj/photos"; echo precious > "$proj/photos/cat.jpg"
expect "unpack refuses a directory that isn't a project" 1 'photos: .*not empty and holds no project' -- "$cli" unpack "$piece" "$proj/photos"
if [[ $(ls "$proj/photos") == cat.jpg ]]; then ok "the directory is untouched"; else bad "unpack wrote into a non-project directory"; fi
printf 'PK\x03\x04 not really' > "$proj/broken.bwp"
expect "a damaged project is an error, not an .ans" 1 'broken\.bwp: .*not a project zip' -- "$cli" info "$proj/broken.bwp"
# an .ans opens as the editor opens it: bright backgrounds turn iCE on, and blink is lost then
expect "pack: blink lost on opening" 0 'warning: .*lossy\.ans: 1 blinking cell loses its blink' -- "$cli" pack "$tmp/lossy.ans" "$proj/lossy.bwp"
"$cli" convert --allow-lossy "$tmp/lossy.ans" "$proj/lossy-direct.ans" 2> /dev/null
"$cli" convert "$proj/lossy.bwp" "$proj/lossy-via.ans"
if cmp -s "$proj/lossy-direct.ans" "$proj/lossy-via.ans"; then ok "a project writes the .ans convert writes"; else bad "pack then convert differs from convert"; fi
expect "pack: a project with a glyph with no CP437 code" 0 '^$' -- "$cli" pack "$data/projects/unwritable.bwp" "$proj/glyph.bwp"
expect "lossy snapshot refused" 1 'glyph\.bwp: r1 not saved: the snapshot loses data' -- "$cli" snapshot "$proj/glyph.bwp"
expect "lossy snapshot saved nothing" 0 '^snapshots: +0$' -- "$cli" info "$proj/glyph.bwp"
expect "lossy snapshot with --allow-lossy" 0 '^r1$' -- "$cli" snapshot --allow-lossy "$proj/glyph.bwp"
"$cli" unpack "$piece" "$proj/linked" > /dev/null
echo precious > "$proj/victim"
ln -s ../victim "$proj/linked/.manifest.json.bw-tmp"
# a hidden entry isn't the project's, whatever it is: skipped, never written through
expect "snapshot skips a hidden link in a project" 0 '^r1$' -- "$cli" snapshot "$proj/linked"
if [[ $(cat "$proj/victim") == precious && -L $proj/linked/.manifest.json.bw-tmp && -f $proj/linked/snapshots/r1.ans ]]; then ok "the link's target is untouched"; else bad "snapshot wrote through a link"; fi

# the target (#350): a project keeps it, pack and unpack set it, lint
# follows it, and a colour an .ans can't hold is a loss under either
rgb=$tmp/rgb
cp -R "$data/irc-target/rgb" "$rgb"
expect "info: a project without a target is ANSI" 0 '^target: +ansi$' -- "$cli" info "$rgb"
expect "info: a flat file has no target" 0 '^size: ' -- "$cli" info "$piece"
if "$cli" info "$piece" | grep -q '^target:'; then bad "info printed a target for a flat file"; else ok "info prints no target for a flat file"; fi
expect "pack --target irc" 0 '^$' -- "$cli" pack --target irc "$rgb" "$tmp/rgb-irc.bwp"
expect "info reads the target back" 0 '^target: +irc$' -- "$cli" info "$tmp/rgb-irc.bwp"
expect "pack keeps a project's target" 0 '^$' -- "$cli" pack "$tmp/rgb-irc.bwp" "$tmp/rgb-irc2.bwp"
expect "info: the target is kept" 0 '^target: +irc$' -- "$cli" info "$tmp/rgb-irc2.bwp"
expect "unpack --target in any case" 0 '^$' -- "$cli" unpack --target ANSI "$tmp/rgb-irc.bwp" "$tmp/rgb-ansi"
expect "info: unpack set ANSI" 0 '^target: +ansi$' -- "$cli" info "$tmp/rgb-ansi"
if grep -q '"target"' "$tmp/rgb-ansi/manifest.json"; then bad "an ANSI manifest names its target"; else ok "an ANSI manifest has no target key"; fi
expect "unpack --target irc of an .ans" 0 '^$' -- "$cli" unpack --target irc "$piece" "$tmp/tux-irc"
if grep -Eq '"target": *"irc"' "$tmp/tux-irc/manifest.json"; then ok "an IRC manifest says so"; else bad "an IRC manifest has no target"; fi
expect "a bad --target is a usage error" 2 "--target takes ansi or irc, not 'mirc'" -- "$cli" pack --target mirc "$piece" "$tmp/x.bwp"
expect "--target needs a value" 2 '.' -- "$cli" lint "$piece" --target
expect "convert takes no --target" 2 '.' -- "$cli" convert --target irc "$piece" "$tmp/x.ans"
expect "lint under ANSI: no ans-loss" 0 '^clean' -- "$cli" lint "$rgb"
# ans-loss is a note: printed, but only mIRC colours lint clean and exit 0
expect "lint --target irc: ans-loss" 0 '^0,0 ans-loss - note: 1 cell is in a colour an \.ans can.t hold, saved as the nearest of the 16 \(fine for IRC\)$' -- "$cli" lint --target irc "$rgb"
expect "lint: a note alone is clean" 0 '^clean, 1 note \(1 ans-loss\), noise ' -- "$cli" lint --target irc "$rgb"
expect "lint follows the project's target" 0 'ans-loss' -- "$cli" lint "$tmp/rgb-irc.bwp"
expect "lint --target ansi overrides it" 0 '^clean' -- "$cli" lint --target ansi "$tmp/rgb-irc.bwp"
expect "lint --json names ans-loss" 0 '"kind": *"ans-loss", *"severity": *"note"' -- "$cli" lint --json --target irc "$rgb"
# the same colour beside a real finding (a layer with an A in its own
# background colour) still exits 4, the note printed too
printf '\033[0;31;41mA' > "$tmp/merge.ans"
expect "unpack the merge" 0 '^$' -- "$cli" unpack "$tmp/merge.ans" "$tmp/rgb-merge"
cp "$rgb/layers/1.cells" "$tmp/rgb-merge/layers/2.cells"
sed -e 's/"id": 1/"id": 2/' -e 's/"x": 0/"x": 1/' "$rgb/layers/1.json" > "$tmp/rgb-merge/layers/2.json"
printf '{"format": "blockwright", "version": "1.3", "width": 80, "height": 1, "ice": true, "palette": "cga", "sauce": null, "target": "irc", "ircBudget": {"bytes": 407, "profile": "efnet-weechat"}, "layers": [1, 2], "nextLayerId": 3}\n' > "$tmp/rgb-merge/manifest.json"
expect "lint: a warning beside the note exits 4" 4 '^1 finding \(1 merge\), 1 note \(1 ans-loss\), noise ' -- "$cli" lint "$tmp/rgb-merge"
expect "lint: the note is printed beside it" 4 '^1,0 ans-loss - note: 1 cell is in a colour' -- "$cli" lint "$tmp/rgb-merge"
expect "lint --json: a warning's severity" 4 '"kind": *"merge", *"severity": *"warning"' -- "$cli" lint --json "$tmp/rgb-merge"
if [[ $("$cli" lint --target irc "$piece" 2>&1) == $("$cli" lint "$piece" 2>&1) ]]; then ok "lint --target irc of an .ans that loses nothing says what ANSI does"; else bad "lint --target irc of tux differs"; fi
expect "convert refuses a colour it can't hold" 1 'warning: .*rgb: 1 cell is in a colour the file can.t hold, written as the nearest of the 16' -- "$cli" convert "$rgb" "$tmp/rgb.ans"
if [[ -e $tmp/rgb.ans ]]; then bad "refused RGB convert wrote a file"; else ok "refused RGB convert writes nothing"; fi
expect "convert --allow-lossy writes the nearest" 0 'nearest of the 16' -- "$cli" convert --allow-lossy "$rgb" "$tmp/rgb.ans"
expect "convert to XBIN counts it too" 1 'nearest of the 16' -- "$cli" convert "$rgb" "$tmp/rgb.xb"
expect "snapshot refuses it" 1 'r1 not saved: the snapshot loses data' -- "$cli" snapshot "$tmp/rgb-ansi"
# IRC carries the colour: mIRC 77, its own code
expect "export --irc sends mIRC 77" 0 '1 line' -- "$cli" export --irc "$tmp/rgb-irc.bwp" "$tmp/rgb.txt"
printf '\00377,1X\00315BCD\n' >"$tmp/rgb-want.txt"
if cmp -s "$tmp/rgb.txt" "$tmp/rgb-want.txt"; then ok "export --irc writes ^C77"; else bad "export --irc wrote $(od -An -c "$tmp/rgb.txt")"; fi

# replay (#208): files per step, nothing written when a step fails
rp=$tmp/replay
mkdir -p "$rp"
expect "replay --help documents the steps file" 0 '"capture": "before-after"' -- "$cli" replay --help
expect "replay --help names the pressure mode (#382)" 0 '"pressure" none\|size\|strength' -- "$cli" replay --help
# a point's pressure scales the size with "pressure": "size", and not with none
printf '{"version": 1, "start": {"blank": [8, 4]}, "steps": [{"name": "s", "action": "glyph", "size": 4, "pressure": "%s", "points": [[3, 2, 0.25]]}]}' size > "$rp/psize.json"
printf '{"version": 1, "start": {"blank": [8, 4]}, "steps": [{"name": "s", "action": "glyph", "size": 4, "pressure": "%s", "points": [[3, 2, 0.25]]}]}' none > "$rp/pnone.json"
expect "replay: pressure to size" 0 '^$' -- "$cli" replay "$rp/psize.json" -o "$rp/psize"
expect "replay: pressure none" 0 '^$' -- "$cli" replay "$rp/pnone.json" -o "$rp/pnone"
if ! cmp -s "$rp/psize/01-s.ans" "$rp/pnone/01-s.ans"; then ok "replay: pressure to size changes the stroke"; else bad "replay: pressure to size painted what none does"; fi
expect "replay: the tour" 0 '^$' -- "$cli" replay "$data/replay/tour.json" -o "$rp/tour"
if diff -r "$data/replay/tour" "$rp/tour" > /dev/null; then ok "replay: the tour's saved bytes"; else bad "replay: the tour differs from data/replay/tour"; fi
printf '{"version": 1, "start": {"blank": [8, 4]}, "steps": [{"name": "dot", "action": "glyph", "points": [[1, 1]]}, {"name": "far", "action": "shade", "points": [[9, 0]]}]}' > "$rp/bad.json"
expect "replay: a step off the canvas, named" 1 'step 2 \(far\): ' -- "$cli" replay "$rp/bad.json" -o "$rp/bad"
if [[ ! -e $rp/bad ]]; then ok "replay: a failed replay writes nothing"; else bad "replay wrote files for a failed replay"; fi
expect "replay: -o is required" 2 "replay needs -o" -- "$cli" replay "$rp/bad.json"
# a view step (#398) is the window's: replay has no window, so it refuses the
# file, exit 1 as for a bad step, before the start is read, writing nothing
printf '{"version": 1, "start": {"blank": [8, 4]}, "steps": [{"name": "dot", "action": "glyph", "points": [[1, 1]]}, {"name": "zoomed", "action": "view", "zoom": 4, "ircFit": true}]}' > "$rp/view.json"
expect "replay: a view step is refused, named" 1 "^blockwright-cli: $rp/view\.json: step 2 \(zoomed\): view sets the window's zoom, scroll and IRC options, and blockwright-cli replay has no window; replay this file with blockwright --screenshot$" -- "$cli" replay "$rp/view.json" -o "$rp/view"
if [[ ! -e $rp/view ]]; then ok "replay: a refused view step writes nothing"; else bad "replay wrote files for a view step"; fi
printf '{"version": 1, "start": {"file": "missing.ans"}, "steps": [{"name": "fit", "action": "view", "ircFit": true}]}' > "$rp/view-nostart.json"
expect "replay: a view step is refused before the start is read" 1 'step 1 \(fit\): view sets the window' -- "$cli" replay "$rp/view-nostart.json" -o "$rp/view-nostart"
expect "replay --help documents the view step (#398)" 0 '^  view +the window.s view, for blockwright --screenshot only:$' -- "$cli" replay --help
expect "replay --help documents the window's size (#295)" 0 '^here: a top-level "window": \[w, h\], the window.s size in pixels' -- "$cli" replay --help
expect "replay --help documents a step's screenshot and its scale (#295)" 0 '^ +"when": "before" \| "after" \| "both", "scale": 1 \| 2\}$' -- "$cli" replay --help
# replay writes .ans files whatever the target, so --target isn't its option
expect "replay takes no --target" 2 '^blockwright-cli: --target is a lint, pack or unpack option, not a replay option$' -- "$cli" replay --target irc "$data/replay/tour.json" -o "$rp/target"
if [[ ! -e $rp/target ]]; then ok "replay --target writes nothing"; else bad "replay --target made $rp/target"; fi
# every file is made in memory and renamed into place only once all are
# written, so a write that fails leaves no new file; a directory in the way
# is refused before anything is written
printf '{"version": 1, "start": {"blank": [8, 4]}, "steps": [{"name": "a", "action": "glyph", "points": [[1, 1]]}, {"name": "b", "action": "glyph", "points": [[2, 1]]}]}' > "$rp/two.json"
mkdir -p "$rp/way/02-b.ans"
expect "replay: a directory in the way is refused" 1 '02-b\.ans: not written: a directory is in the way$' -- "$cli" replay "$rp/two.json" -o "$rp/way"
if [[ $(ls -A "$rp/way") == 02-b.ans ]]; then ok "replay: a directory in the way, nothing written"; else bad "replay wrote beside a directory in the way: $(ls -A "$rp/way")"; fi
mkdir -p "$rp/wayp/02-b.bwp"
expect "replay --project: a directory in the way is refused" 1 '02-b\.bwp: not written: a directory is in the way$' -- "$cli" replay --project "$rp/two.json" -o "$rp/wayp"
if [[ $(ls -A "$rp/wayp") == 02-b.bwp ]]; then ok "replay --project: a directory in the way, nothing written"; else bad "replay --project wrote beside a directory in the way: $(ls -A "$rp/wayp")"; fi
# files already there are left, but for those the run writes again
mkdir -p "$rp/old"
echo stale > "$rp/old/01-a.ans"
echo kept > "$rp/old/09-z.ans"
expect "replay into a directory used before" 0 '^$' -- "$cli" replay --project "$rp/two.json" -o "$rp/old"
if [[ $(ls -A "$rp/old" | tr '\n' ' ') == "01-a.ans 01-a.bwp 02-b.ans 02-b.bwp 09-z.ans " ]]; then ok "replay: the captures beside the old file, no temporaries left"; else bad "replay into a used directory left: $(ls -A "$rp/old" | tr '\n' ' ')"; fi
if [[ $(cat "$rp/old/09-z.ans") == kept && $(cat "$rp/old/01-a.ans") != stale ]]; then ok "replay: an old capture replaced, another file left"; else bad "replay: old files in the output directory"; fi

# a capture already there is written as save writes it (convert shows
# it): through a symlink, keeping its permissions, and refused when it
# isn't writable, with nothing written
mkdir -p "$rp/kept"
echo real > "$rp/kept/real.ans"
ln -s real.ans "$rp/kept/01-a.ans"
echo old > "$rp/kept/02-b.ans"
chmod 600 "$rp/kept/02-b.ans"
expect "replay onto a symlink and a 0600 file" 0 '^$' -- "$cli" replay "$rp/two.json" -o "$rp/kept"
if [[ -L $rp/kept/01-a.ans ]] && cmp -s "$rp/kept/real.ans" "$rp/old/01-a.ans"; then ok "replay writes through a symlink"; else bad "replay replaced a symlink instead of writing through it"; fi
if [[ $(ls -l "$rp/kept/02-b.ans" | cut -c1-10) == -rw------- ]] && cmp -s "$rp/kept/02-b.ans" "$rp/old/02-b.ans"; then ok "replay keeps a file's permissions"; else bad "replay changed a 0600 file to $(ls -l "$rp/kept/02-b.ans" | cut -c1-10)"; fi
if [[ $(ls -A "$rp/kept" | tr '\n' ' ') == "01-a.ans 02-b.ans real.ans " ]]; then ok "replay through a symlink: no temporaries left"; else bad "replay through a symlink left: $(ls -A "$rp/kept" | tr '\n' ' ')"; fi
if [[ $(id -u) -ne 0 ]]; then
  mkdir -p "$rp/ro"
  echo ro > "$rp/ro/02-b.ans"
  chmod 444 "$rp/ro/02-b.ans"
  expect "convert refuses a read-only file" 1 'Existing file .*02-b\.ans is not writable' -- "$cli" convert "$rp/old/01-a.ans" "$rp/ro/02-b.ans"
  expect "replay refuses a read-only capture as convert does" 1 '02-b\.ans: Existing file .*02-b\.ans is not writable' -- "$cli" replay "$rp/two.json" -o "$rp/ro"
  if [[ $(ls -A "$rp/ro") == 02-b.ans && $(cat "$rp/ro/02-b.ans") == ro ]]; then ok "replay: a read-only capture, nothing written"; else bad "replay wrote beside a read-only capture: $(ls -A "$rp/ro" | tr '\n' ' ')"; fi
else
  echo "skip replay: a read-only capture (running as root)"
fi
# every output stays open until all are written: with too few files to
# hold them all, the replay fails and writes nothing
{
  printf '{"version": 1, "start": {"blank": [8, 4]}, "steps": ['
  for i in $(seq 0 29); do printf '%s{"name": "s%d", "action": "glyph", "points": [[%d, %d]]}' "$([[ $i -gt 0 ]] && echo ,)" "$i" $((i % 8)) $((i / 8)); done
  printf ']}'
} > "$rp/many.json"
mkdir -p "$rp/many"
expect "replay: more outputs than open files, refused" 1 '^blockwright-cli: not written: 30 outputs need 94 open files at once \(one each and 64 to spare\), more than the open-file limit of 32$' -- bash -c 'ulimit -n 32 && exec "$0" "$@"' "$cli" replay "$rp/many.json" -o "$rp/many"
if [[ -z $(ls -A "$rp/many") ]]; then ok "replay: out of open files, nothing written"; else bad "replay out of open files left: $(ls -A "$rp/many" | tr '\n' ' ')"; fi
# the refusal comes before <dir> is made, as a failed step's does
expect "replay: out of open files, into a new directory" 1 'more than the open-file limit of 32$' -- bash -c 'ulimit -n 32 && exec "$0" "$@"' "$cli" replay "$rp/many.json" -o "$rp/many-new/deeper"
if [[ ! -e $rp/many-new ]]; then ok "replay: out of open files, no directory made"; else bad "replay out of open files made $rp/many-new"; fi
# the rule is the outputs and 64 spare: 94 holds 30, 93 doesn't
expect "replay: one open file short, refused" 1 '30 outputs need 94 open files at once \(one each and 64 to spare\), more than the open-file limit of 93$' -- bash -c 'ulimit -n 93 && exec "$0" "$@"' "$cli" replay "$rp/many.json" -o "$rp/many-93"
if [[ ! -e $rp/many-93 ]]; then ok "replay: one open file short, nothing made"; else bad "replay one open file short made $rp/many-93"; fi
expect "replay: exactly enough open files" 0 '^$' -- bash -c 'ulimit -n 94 && exec "$0" "$@"' "$cli" replay "$rp/many.json" -o "$rp/many-94"
if [[ $(ls -A "$rp/many-94" | wc -l) -eq 30 ]]; then ok "replay: exactly enough open files, 30 written"; else bad "replay with 94 open files wrote: $(ls -A "$rp/many-94" | tr '\n' ' ')"; fi
mkdir -p "$rp/many-soft"
expect "replay: as many outputs as the soft limit allows, raised" 0 '^$' -- bash -c 'ulimit -S -n 32 && exec "$0" "$@"' "$cli" replay "$rp/many.json" -o "$rp/many-soft"
if [[ $(ls -A "$rp/many-soft" | wc -l) -eq 30 ]]; then ok "replay: the soft limit raised for its outputs"; else bad "replay under a low soft limit wrote: $(ls -A "$rp/many-soft" | tr '\n' ' ')"; fi

# A project before 5.0 with attribute bits no build wrote (#370): every
# command that reads one clears them, as the app does, and says so in one
# note on stderr naming the file and the cell count. Its stdout, exit status
# and outputs are those of the same project without the bits.
# data/projects/stray-attrs is a 4.0 directory project whose first two cells
# hold bit 5 and the strikethrough bit (with blink), the third blink alone.
sa=$tmp/stray-attrs
mkdir -p "$sa/stray" "$sa/clean"
cp -R "$data/projects/stray-attrs" "$sa/stray/p"
# the clean twin: the same project as the CLI writes it back, cleared, and
# marked 4.0 too, so the two differ only in those bits
"$cli" unpack "$sa/stray/p" "$sa/clean/p" >/dev/null 2>&1
sed -e 's/"version": "[0-9.]*"/"version": "4.0"/' "$sa/clean/p/manifest.json" >"$sa/m" && mv "$sa/m" "$sa/clean/p/manifest.json"
if cmp -s "$sa/stray/p/manifest.json" "$sa/clean/p/manifest.json" && ! cmp -s "$sa/stray/p/layers/1.cells" "$sa/clean/p/layers/1.cells"; then
  ok "stray attribute bits: the clean twin differs only in its cells"
else
  bad "stray attribute bits: the clean twin's manifest differs, or its cells don't"
fi
sanote='cleared attribute bits no earlier build wrote from 2 cells'
# readnote <name> <note> <outputs to compare> -- <args, "p" the project>: the
# command run in the stray and the clean directory; the stray one's stderr is
# <note> and then the clean one's, the rest the same, both exit 0
readnote() {
  local name=$1 want=$2 outs=$3 d f
  shift 4
  for d in stray clean; do
    rm -f "$sa/$d/out" "$sa/$d/err" "$sa/$d/st"
    (cd "$sa/$d" && "$cli" "$@" >out 2>err; echo $? >st)
    tr -d '\r' <"$sa/$d/err" >"$sa/$d/err.n"
  done
  if [[ $(cat "$sa/stray/st") != 0 || $(cat "$sa/clean/st") != 0 ]]; then
    bad "$name: exit $(cat "$sa/stray/st") (clean $(cat "$sa/clean/st")), wanted 0: $(cat "$sa/stray/err")"; return
  fi
  if grep -q 'cleared attribute bits' "$sa/clean/err.n"; then bad "$name: a clean project notes cleared bits: $(cat "$sa/clean/err")"; return; fi
  if ! { printf '%s\n' "$want"; cat "$sa/clean/err.n"; } | cmp -s - "$sa/stray/err.n"; then
    bad "$name: stderr isn't the note and then the clean project's: $(cat "$sa/stray/err")"; return
  fi
  if ! cmp -s "$sa/stray/out" "$sa/clean/out"; then bad "$name: stdout differs from the clean project's: $(cat "$sa/stray/out")"; return; fi
  for f in $outs; do
    if ! diff -r "$sa/stray/$f" "$sa/clean/$f" >/dev/null 2>&1; then bad "$name: $f differs from the clean project's"; return; fi
  done
  ok "$name"
}
pnote="blockwright-cli: note: p: $sanote"
readnote "stray attribute bits: info notes them" "$pnote" "" -- info p
readnote "stray attribute bits: convert" "$pnote" "o.ans" -- convert p o.ans
readnote "stray attribute bits: export --irc" "$pnote" "o.txt" -- export --irc p o.txt
readnote "stray attribute bits: render --irc" "$pnote" "o.png" -- render --irc p o.png
readnote "stray attribute bits: shade" "$pnote" "s.ans" -- shade --ramp steel p s.ans
readnote "stray attribute bits: lint" "$pnote" "" -- lint p
readnote "stray attribute bits: lint --json, the JSON unchanged" "$pnote" "" -- lint --json p
readnote "stray attribute bits: pack" "$pnote" "o.bwp" -- pack p o.bwp
readnote "stray attribute bits: unpack" "$pnote" "u" -- unpack p u
readnote "stray attribute bits: run-generators" "$pnote" "" -- run-generators p
for d in stray clean; do
  printf '{"version": 1, "start": {"file": "p"}, "steps": [{"name": "dot", "action": "glyph", "points": [[5, 0]]}]}' >"$sa/$d/steps.json"
done
readnote "stray attribute bits: replay notes its start" "blockwright-cli: note: steps.json: start: p: $sanote" "r" -- replay --project steps.json -o r
# a step that fails after the start was read: the note, once, then the step's
# error, as the clean project gives it; exit 1, nothing on stdout or written
for d in stray clean; do
  printf '{"version": 1, "start": {"file": "p"}, "steps": [{"name": "gone", "action": "deleteLayer"}]}' >"$sa/$d/fail.json"
  (cd "$sa/$d" && "$cli" replay fail.json -o rf >out 2>err; echo $? >st)
  tr -d '\r' <"$sa/$d/err" >"$sa/$d/err.n"
done
if [[ $(cat "$sa/stray/st") != 1 || $(cat "$sa/clean/st") != 1 ]]; then
  bad "stray attribute bits: replay with a failing step: exit $(cat "$sa/stray/st") (clean $(cat "$sa/clean/st")), wanted 1: $(cat "$sa/stray/err")"
elif [[ -s $sa/stray/out || -e $sa/stray/rf ]]; then
  bad "stray attribute bits: replay with a failing step wrote stdout or outputs: $(cat "$sa/stray/out")"
elif [[ $(grep -c 'cleared attribute bits' "$sa/clean/err.n") != 0 || $(wc -l <"$sa/clean/err.n") -ne 1 ]]; then
  bad "stray attribute bits: replay with a failing step on the clean project: $(cat "$sa/clean/err")"
elif ! { printf '%s\n' "blockwright-cli: note: fail.json: start: p: $sanote"; cat "$sa/clean/err.n"; } | cmp -s - "$sa/stray/err.n"; then
  bad "stray attribute bits: replay with a failing step: stderr isn't the note once and then the step's error: $(cat "$sa/stray/err")"
else
  ok "stray attribute bits: replay notes its start when a later step fails"
fi
# a step that throws a core error (#373): layer ids used up, so newCellsLayer's
# add() refuses. The note still comes once, before the step's error, which is
# a normal step error (exit 1), not an abort; nothing on stdout or written
for d in stray clean; do
  rm -rf "$sa/$d/px" && cp -R "$sa/$d/p" "$sa/$d/px"
  sed 's/"nextLayerId": 2/"nextLayerId": 0/' "$sa/$d/px/manifest.json" >"$sa/$d/m" && mv "$sa/$d/m" "$sa/$d/px/manifest.json"
  printf '{"version": 1, "start": {"file": "px"}, "steps": [{"name": "add", "action": "newCellsLayer"}]}' >"$sa/$d/exh.json"
  (cd "$sa/$d" && "$cli" replay exh.json -o rx >out 2>err; echo $? >st)
  tr -d '\r' <"$sa/$d/err" >"$sa/$d/err.n"
done
xstep="blockwright-cli: exh.json: step 1 (add): layer ids exhausted"
if ! grep -q '"nextLayerId": 0' "$sa/stray/px/manifest.json"; then
  bad "stray attribute bits: replay with a core step error: the fixture's nextLayerId didn't become 0"
elif [[ $(cat "$sa/stray/st") != 1 || $(cat "$sa/clean/st") != 1 ]]; then
  bad "stray attribute bits: replay with a core step error: exit $(cat "$sa/stray/st") (clean $(cat "$sa/clean/st")), wanted 1: $(cat "$sa/stray/err")"
elif [[ -s $sa/stray/out || -e $sa/stray/rx || -s $sa/clean/out || -e $sa/clean/rx ]]; then
  bad "stray attribute bits: replay with a core step error wrote stdout or outputs: $(cat "$sa/stray/out")"
elif ! printf '%s\n' "$xstep" | cmp -s - "$sa/clean/err.n"; then
  bad "stray attribute bits: replay with a core step error on the clean project: stderr isn't the step's error alone: $(cat "$sa/clean/err")"
elif ! printf '%s\n' "blockwright-cli: note: exh.json: start: px: $sanote" "$xstep" | cmp -s - "$sa/stray/err.n"; then
  bad "stray attribute bits: replay with a core step error: stderr isn't the note once and then the step's error: $(cat "$sa/stray/err")"
else
  ok "stray attribute bits: replay notes its start when a step throws a core error"
fi
# add-generator saves the project, so it works on a copy
if [[ -z ${BW_CLI_WINDOWS:-} ]]; then  # (the generator is a sh script)
  mkdir -p "$sa/gen"
  printf '{"command": ["sh", "gen.sh"]}\n' >"$sa/gen/generator.json"
  printf 'printf "\\033[0;44m  \\033[31mAB"\n' >"$sa/gen/gen.sh"
  for d in stray clean; do rm -rf "$sa/$d/g" && cp -R "$sa/$d/p" "$sa/$d/g"; done
  readnote "stray attribute bits: add-generator" "blockwright-cli: note: g: $sanote" "g" -- add-generator g "$sa/gen"
fi
# snapshot saves the project cleared, so it reads clean after
readnote "stray attribute bits: snapshot" "$pnote" "" -- snapshot p
err=$(cd "$sa/stray" && "$cli" info p 2>&1 >/dev/null)
if [[ -z $err ]]; then ok "stray attribute bits: saved by snapshot, the project reads with no note"; else bad "stray attribute bits: after snapshot, info says: $err"; fi
# the same bits in 5.0 or later are refused, not cleared
cp -R "$data/projects/stray-attrs" "$sa/p5"
sed -e 's/"version": "4.0"/"version": "5.0"/' "$data/projects/stray-attrs/manifest.json" >"$sa/p5/manifest.json"
expect "stray attribute bits: refused in a 5.0 project" 1 'layers/1\.cells: layer cells: cell 0 has an unknown attribute$' -- "$cli" info "$sa/p5"
# a 5.0 project's bold, italic and underline are its own, not stray: no
# command notes them and they are kept. data/projects/formatted: cell 0 bold,
# cell 1 italic, underline and blink, cell 2 blink
fm=$tmp/formatted
mkdir -p "$fm"
cp -R "$data/projects/formatted" "$fm/p"
cp -R "$data/projects/formatted" "$fm/g"
printf '{"version": 1, "start": {"file": "p"}, "steps": [{"name": "dot", "action": "glyph", "points": [[5, 0]]}]}' >"$fm/steps.json"
# fmquiet <name> <args>: run in $fm, exit 0 and no note on stderr
fmquiet() {
  local name=$1 err st
  shift
  err=$(cd "$fm" && "$cli" "$@" 2>&1 >/dev/null); st=$?
  if [[ $st -ne 0 ]]; then bad "$name: exit $st: $err"; elif grep -q 'note:' <<<"$err"; then bad "$name: stderr: $err"; else ok "$name"; fi
}
fmquiet "5.0 formatting: info notes nothing" info p
fmquiet "5.0 formatting: export --irc notes nothing" export --irc p o.txt
fmquiet "5.0 formatting: unpack notes nothing" unpack p u
# (an .ans can't hold the formatting: replay's capture warns of that)
fmquiet "5.0 formatting: replay notes nothing" replay --allow-lossy --project steps.json -o r
if [[ -z ${BW_CLI_WINDOWS:-} ]]; then
  fmquiet "5.0 formatting: add-generator notes nothing" add-generator g "$sa/gen"
fi
# bold (^B), italic (^]) and underline (^_) reach the IRC text
for c in '\002' '\035' '\037'; do
  if [[ $(tr -dc "$c" <"$fm/o.txt" | wc -c) -ge 1 ]]; then ok "5.0 formatting: export --irc keeps $c"; else bad "5.0 formatting: export --irc lost $c"; fi
done
(cd "$fm" && "$cli" export --irc u o2.txt >/dev/null 2>&1)
if grep -q '"version": "5\.0"' "$fm/u/manifest.json" && cmp -s "$fm/o.txt" "$fm/o2.txt"; then
  ok "5.0 formatting: unpack writes 5.0 and the same IRC text"
else
  bad "5.0 formatting: unpack wrote $(grep '"version"' "$fm/u/manifest.json")"
fi

# main's std::exception backstop (#380): an error a command didn't wrap as a
# Failure ends the run with "blockwright-cli: <command>: <what>" and exit 1,
# not an abort. BLOCKWRIGHT_CLI_FAULT=save (a test-only hook) throws a
# std::runtime_error in save() once its temporary is written, before the
# commit: no output and no temporary is left, and a file there is kept
fault=$tmp/fault; mkdir -p "$fault"
out=$(BLOCKWRIGHT_CLI_FAULT=save "$cli" convert "$piece" "$fault/new.ans" 2>&1); st=$?
if [[ $st -eq 1 && $out == 'blockwright-cli: convert: fault injected at save' ]]; then
  ok "backstop: convert exits 1 with one line naming the command"
else
  bad "backstop: convert: exit $st: $out"
fi
if [[ -z $(ls -A "$fault") ]]; then ok "backstop: convert writes nothing"; else bad "backstop: convert left $(ls -A "$fault" | tr '\n' ' ')"; fi
printf 'kept\n' >"$fault/old.txt"
err=$(BLOCKWRIGHT_CLI_FAULT=save "$cli" export --irc "$piece" "$fault/old.txt" 2>&1 >/dev/null); st=$?
if [[ $st -eq 1 && $err == 'blockwright-cli: export: fault injected at save' ]]; then
  ok "backstop: export --irc exits 1 with one line naming the command"
else
  bad "backstop: export --irc: exit $st: $err"
fi
if [[ $(cat "$fault/old.txt") == kept && $(ls -A "$fault") == old.txt ]]; then ok "backstop: export --irc keeps the file there"; else bad "backstop: export --irc changed $fault: $(ls -A "$fault" | tr '\n' ' ')"; fi
expect "the fault hook fires only for its step" 0 '^$' -- env BLOCKWRIGHT_CLI_FAULT=commit "$cli" convert "$piece" "$fault/new.ans"

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
