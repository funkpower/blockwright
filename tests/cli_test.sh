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

expect "info: dimensions" 0 '^size: +30x17$' -- "$cli" info "$piece"
expect "info: SAUCE author" 0 '^author: +funkpower$' -- "$cli" info "$piece"
expect "info: FileSize checked" 0 '^filesize: +1948 \(matches content\)$' -- "$cli" info "$piece"
expect "info: flags" 0 '^flags: +0x1 \(iCE on' -- "$cli" info "$piece"

expect "convert" 0 '^$' -- "$cli" convert "$piece" "$tmp/tux.ans"
if cmp -s "$piece" "$tmp/tux.ans"; then ok "convert output is byte-identical"; else bad "convert output differs"; fi

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
# --fit: colours merged until every line is in the budget; text left alone
for i in $(seq 0 79); do printf '\033[%d;%dm\xdf' $((30 + i % 8)) $((40 + (i + 3) % 8)); done > "$tmp/halves.ans"
expect "half blocks over the budget" 3 '^1 line, longest 600 bytes, budget 407: 1 over$' -- "$cli" export --irc "$tmp/halves.ans" "$tmp/halves.txt"
expect "--fit fits them" 0 '^1 line, longest [0-9]+ bytes, budget 407: 0 over$' -- "$cli" export --irc --fit "$tmp/halves.ans" "$tmp/halves.txt"
expect "--fit marks the line it merged" 0 '^ +1 +[0-9]+  fit: [1-9][0-9]* cells merged$' -- "$cli" export --irc --fit "$tmp/halves.ans" "$tmp/halves.txt"
expect "--fit counts what it merged" 0 '^fit: [1-9][0-9]* cells merged in 1 line$' -- "$cli" export --irc --fit "$tmp/halves.ans" "$tmp/halves.txt"
if [[ $(wc -c < "$tmp/halves.txt") -le 408 ]]; then ok "--fit writes the fitted line"; else bad "--fit wrote $(wc -c < "$tmp/halves.txt") bytes"; fi
expect "--fit takes the budget" 0 'budget 300: 0 over$' -- "$cli" export --irc --fit --budget 300 "$tmp/halves.ans" "$tmp/halves.txt"
expect "--fit never changes text" 3 '^fit: 0 cells merged in 0 lines$' -- "$cli" export --irc --fit "$tmp/rainbow.ans" "$tmp/rainbow.txt"
expect "--fit leaves a piece in budget alone" 0 '^fit: 0 cells merged in 0 lines$' -- "$cli" export --irc --fit "$piece" "$tmp/tux-fit.txt"
if cmp -s "$data/golden-irc/tux.txt" "$tmp/tux-fit.txt"; then ok "--fit of a piece in budget is golden-irc's"; else bad "--fit changed a piece in budget"; fi
# --pad: blank rows and trailing black kept, each line ended on a black block
printf '\r\n\033[31mx\r\n' > "$tmp/edge.ans"
expect "export drops a blank top row" 0 '^1 line, longest 5 bytes' -- "$cli" export --irc "$tmp/edge.ans" "$tmp/edge.txt"
expect "--pad keeps it" 0 '^2 lines, longest 88 bytes' -- "$cli" export --irc --pad "$tmp/edge.ans" "$tmp/edge.txt"
if [[ $(grep -c $'\xe2\x96\x88$' "$tmp/edge.txt") -eq 2 ]]; then ok "--pad ends every line on a block"; else bad "--pad lines: $(cat -A "$tmp/edge.txt")"; fi
# --swap-shades: ▓ on red as ░ the other way round, one colour code for the row
for i in $(seq 0 39); do printf '\033[34;41m\xb2\033[31;44m\xb0'; done > "$tmp/shade.ans"
expect "shades over the budget" 3 '^1 line, longest 560 bytes' -- "$cli" export --irc "$tmp/shade.ans" "$tmp/shade.txt"
expect "--swap-shades" 0 '^1 line, longest 244 bytes, budget 407: 0 over$' -- "$cli" export --irc --swap-shades "$tmp/shade.ans" "$tmp/shade.txt"
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
expect "usage lists lint" 2 'lint \[--ramp R\]\.\.\. \[--box x0,y0,x1,y1\] \[--json\] <in>' -- "$cli" lint
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
expect "lint: a skip as JSON" 4 '^\{"findings":\[\{"kind":"rung-skip","ramp":"steel","x":3,"y":0,"x2":4,"y2":0,"message":"cell 3,0: rung 4 next to rung 8 at cell 4,0 \(more than one rung apart\)"\}\],"noise":0\.0000,"ice":true\}$' -- \
  "$cli" lint --json --ramp steel "$tmp/lint-skip.ans"
expect "lint: a wide rung as JSON" 4 '"kind":"wide-rung","ramp":"steel","x":2,"y":3,"x2":7,"y2":3,' -- "$cli" lint --json --ramp steel "$tmp/lint-wide.ans"
expect "lint: a speck as JSON" 4 '^\{"findings":\[\{"kind":"speckle","ramp":"steel","x":2,"y":1,"x2":2,"y2":1,' -- "$cli" lint --json --ramp steel "$tmp/lint-speck.ans"
expect "lint: a non-neighbour shade as JSON" 4 '^\{"findings":\[\{"kind":"non-neighbour-shade","ramp":"steel","x":1,"y":0,"x2":1,"y2":0,' -- "$cli" lint --json --ramp steel "$tmp/lint-shade.ans"
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
declare -A owner=([--allow-lossy]="convert shade pack unpack snapshot" [--scale]=render [--font-width]=render
                  [--irc]="export render" [--budget]="export render" [--allow-over-budget]=export
                  [--fit]=export [--pad]=export [--swap-shades]=export
                  [--ramp]="shade lint" [--darken]=shade [--steps]=shade [--box]="shade lint" [--ice]=shade
                  [--json]=lint)
declare -A value=([--scale]=2 [--font-width]=9 [--budget]=60 [--ramp]=steel [--steps]=2 [--box]=0,0,1,1)
foreign=$tmp/foreign && mkdir -p "$foreign"
for cmd in info convert render export shade lint pack unpack snapshot; do
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
expect "options after the files are checked too" 2 '^blockwright-cli: --allow-lossy is a convert, shade, pack, unpack or snapshot option, not a render option$' -- \
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
expect "library check: a good library" 0 '^2 stamp\(s\), 0 text style\(s\), 0 problem\(s\)$' -- "$cli" library check "$lib"
expect "library list: tab-separated" 0 $'^stamp\tsky\tSmall star\t5x3\t16\t-\tstar,night\t.*[/\\\\]sky[/\\\\]small-star\\.ans$' -- "$cli" library list "$lib"
expect "library list: sorted by path" 0 $'^stamp\tfigures\tTux\t30x17\t80\t-\tpenguin,linux\t' -- bash -c '"$1" library list "$2" | head -1' _ "$cli" "$lib"
expect "library list: no dir lists the bundled and user libraries" 0 $'^stamp\tsky\tSmall star\t' -- env BLOCKWRIGHT_BUNDLED_LIBRARY="$lib" BLOCKWRIGHT_USER_LIBRARY="$tmp/no-user-lib" "$cli" library list
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
problems=7
if [[ -z ${BW_CLI_WINDOWS:-} ]]; then  # (a Windows program sees no Linux link)
  mkdir -p "$tmp/shared"; ln -s "$tmp/shared" "$bl/linked"; ln -s "$bl/gone.ans" "$bl/dangling.ans"
  problems=9
fi
expect "library check: a comma in a tag" 1 'comma\.ans: a sidecar tag holds a comma' -- "$cli" library check "$bl"
expect "library check: an all-blank stamp" 1 'blank\.ans: \.ans has no drawn cell' -- "$cli" library check "$bl"
if [[ $problems == 9 ]]; then
  expect "library check: a linked folder" 1 'linked: symbolic link to a folder, not followed' -- "$cli" library check "$bl"
  expect "library check: a broken link" 1 'dangling\.ans: broken symbolic link, skipped' -- "$cli" library check "$bl"
  expect "library list: a linked folder is a warning" 0 'warning: .*linked: symbolic link to a folder' -- "$cli" library list "$bl"
fi
expect "library check: counts" 1 "^0 stamp\\(s\\), 0 text style\\(s\\), $problems problem\\(s\\)\$" -- "$cli" library check "$bl"
expect "library list: problems are warnings" 0 'warning: .*orphan\.json: sidecar with no orphan\.ans' -- "$cli" library list "$bl"
b2=$tmp/badlib2; mkdir -p "$b2"
cp "$lib/sky/small-star.ans" "$b2/dup.ans"
printf '{"category":7,"category":"sky","tags":[],"ircBytes":9999,"ircBytes":16}\n' > "$b2/dup.json"
mkfifo "$b2/pipe.ans"
expect "library check: a repeated key" 1 'dup\.ans: sidecar has the key "category" more than once' -- timeout 10 "$cli" library check "$b2"
expect "library check: a FIFO is never opened" 1 'pipe\.ans: not a regular file, skipped' -- timeout 10 "$cli" library check "$b2"
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
expect "library check: text styles" 0 '^1 stamp\(s\), 2 text style\(s\), 0 problem\(s\)$' -- "$cli" library check "$sl"
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
expect "library check: text style counts" 1 '^0 stamp\(s\), 0 text style\(s\), 9 problem\(s\)$' -- "$cli" library check "$s2"
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
expect "export takes the project's budget" 3 '^17 lines, longest 80 bytes, budget 60: 6 over$' -- "$cli" export --irc "$proj/tight" "$proj/tight.txt"
expect "--budget overrides the project's" 0 '^17 lines, longest 80 bytes, budget 407: 0 over$' -- "$cli" export --irc --budget 407 "$proj/tight" "$proj/tight.txt"
expect "render reads a project" 0 '^$' -- "$cli" render "$proj/tux.bwp" "$proj/tux.png"
expect "snapshot a directory" 0 '^r1$' -- "$cli" snapshot "$proj/tux"
expect "snapshot again" 0 '^r2$' -- "$cli" snapshot "$proj/tux"
if cmp -s "$piece" "$proj/tux/snapshots/r2.ans"; then ok "a snapshot is the flattened .ans"; else bad "snapshot r2 differs from the piece"; fi
expect "snapshot a zip" 0 '^r1$' -- "$cli" snapshot "$proj/tux.bwp"
expect "info: snapshots" 0 '^snapshots: +1 \(last r1\)$' -- "$cli" info "$proj/tux.bwp"
expect "snapshot takes a project" 1 'tux\.ans: not a project' -- "$cli" snapshot "$piece"
expect "pack takes --allow-lossy" 0 '^$' -- "$cli" pack --allow-lossy "$piece" "$proj/x.bwp"
cp "$proj/tux.bwp" "$proj/keep.bwp"
expect "a flat file never replaces a project" 1 'keep\.bwp: not written: it is a project' -- "$cli" convert "$piece" "$proj/keep.bwp"
if cmp -s "$proj/tux.bwp" "$proj/keep.bwp"; then ok "the project is untouched"; else bad "convert wrote over a project"; fi
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

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
