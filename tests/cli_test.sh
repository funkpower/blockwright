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

expect "no command" 2 'no command' -- "$cli"
expect "unknown command" 2 "unknown command 'frobnicate'" -- "$cli" frobnicate
expect "convert arity" 2 'convert takes' -- "$cli" convert "$piece"
expect "info arity" 2 'info takes' -- "$cli" info
expect "unknown option" 2 'Unknown option' -- "$cli" --bogus

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
