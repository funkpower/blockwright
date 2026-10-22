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
if cmp -s "$data/golden-irc/tux.txt" "$tmp/tux.txt"; then ok "export --irc output is ans2irc's"; else bad "export --irc output differs from golden-irc/tux.txt"; fi
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

# Every command refuses every option it doesn't use: exit 2, a usage message
# naming the option and the command, and nothing written.
declare -A owner=([--allow-lossy]=convert [--scale]=render [--font-width]=render
                  [--irc]=export [--budget]=export [--allow-over-budget]=export)
declare -A value=([--scale]=2 [--font-width]=9 [--budget]=60)
foreign=$tmp/foreign && mkdir -p "$foreign"
for cmd in info convert render export; do
  for opt in --allow-lossy --scale --font-width --irc --budget --allow-over-budget; do
    [[ ${owner[$opt]} == "$cmd" ]] && continue
    out=$foreign/$cmd${opt}.out
    case $cmd in
      info) argv=(info "$piece") ;;
      export) argv=(export --irc "$piece" "$out") ;;
      *) argv=("$cmd" "$piece" "$out") ;;
    esac
    a=${owner[$opt]}; [[ $a == export ]] && a="an $a" || a="a $a"
    expect "$cmd rejects $opt" 2 "^blockwright-cli: $opt is $a option, not a $cmd option$" -- \
      "$cli" "${argv[@]:0:1}" "$opt" ${value[$opt]:+"${value[$opt]}"} "${argv[@]:1}"
    if [[ -e $out ]]; then bad "$cmd with $opt wrote $out"; fi
  done
done
out=$("$cli" info --scale 2 "$piece" 2>/dev/null)
if [[ -z $out ]]; then ok "info with a foreign option prints no info"; else bad "info with a foreign option printed: $out"; fi
if [[ -z $(ls -A "$foreign") ]]; then ok "commands with a foreign option write nothing"; else bad "foreign options wrote: $(ls -A "$foreign")"; fi
expect "options after the files are checked too" 2 '^blockwright-cli: --allow-lossy is a convert option, not a render option$' -- \
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

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
