#!/usr/bin/env bash
# micron_cli_test.sh <blockwright-cli> <tests/data> <scratch dir>: `cli export
# --micron` writes each golden-micron page byte for byte from its piece (an
# .ans, an XBIN, or the same .ans packed as a project), centres and writes
# the cache header on request, and refuses the IRC export's options.
set -uo pipefail
cli=$1 data=$2 tmp=$3
rm -rf "$tmp" && mkdir -p "$tmp"
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
same() {  # same <name> <in> [options]: export <in> and compare with golden-micron/<name>.mu
  name=$1 in=$2; shift 2
  if ! "$cli" export --micron "$@" "$in" "$tmp/$name.mu" >"$tmp/$name.out" 2>"$tmp/$name.err"; then
    fail "$name: export --micron failed: $(cat "$tmp/$name.err")"
  elif cmp -s "$tmp/$name.mu" "$data/golden-micron/$name.mu"; then
    echo "ok   $name"
  else
    fail "$name: output differs from golden-micron/$name.mu"
  fi
}
same durin-gate "$data/golden/durin-gate.ans"
same darkmage "$data/golden/darkmage.ans"
same tux-palette-font "$data/xbin/tux-palette-font.xb"
grep -q "own palette" "$tmp/tux-palette-font.out" || fail "the XBIN's report doesn't say it has its own palette"
grep -q "^41 lines, 80 columns" "$tmp/durin-gate.out" || fail "durin-gate report: $(cat "$tmp/durin-gate.out")"

# a project reads as the piece it holds
if "$cli" pack "$data/golden/durin-gate.ans" "$tmp/durin.bwp" >/dev/null 2>&1; then
  if "$cli" export --micron "$tmp/durin.bwp" "$tmp/durin-bwp.mu" >/dev/null 2>"$tmp/bwp.err"; then
    if cmp -s "$tmp/durin-bwp.mu" "$data/golden-micron/durin-gate.mu"; then echo "ok   project"
    else fail "project: output differs from golden-micron/durin-gate.mu"; fi
  else
    fail "project: export --micron failed: $(cat "$tmp/bwp.err")"
  fi
else
  fail "pack durin-gate failed"
fi

# an RGB background in an ANSI-target project with iCE off is written as it
# is, as the app writes it, not folded to the nearest of the 16
bt='`'
if "$cli" export --micron "$data/micron/rgb-bg" "$tmp/rgb-bg.mu" >/dev/null 2>"$tmp/rgb-bg.err"; then
  [ "$(cat "$tmp/rgb-bg.mu")" = "${bt}Faaa${bt}BTff4040X${bt}${bt}" ] ||
    fail "rgb-bg: wrote $(cat "$tmp/rgb-bg.mu"), expected ${bt}Faaa${bt}BTff4040X${bt}${bt}"
  if "$cli" pack "$data/micron/rgb-bg" "$tmp/rgb-bg.bwp" >/dev/null 2>&1 &&
    "$cli" export --micron "$tmp/rgb-bg.bwp" "$tmp/rgb-bg-zip.mu" >/dev/null 2>&1 &&
    cmp -s "$tmp/rgb-bg.mu" "$tmp/rgb-bg-zip.mu"; then echo "ok   rgb-bg"
  else fail "rgb-bg: the packed project writes otherwise"; fi
else
  fail "rgb-bg: export --micron failed: $(cat "$tmp/rgb-bg.err")"
fi

# --centre and --cache: the header first, every line centred, the art unchanged
if "$cli" export --micron --centre --cache 600 "$data/golden/durin-gate.ans" "$tmp/c.mu" >"$tmp/c.out" 2>"$tmp/c.err"; then
  [ "$(head -n 1 "$tmp/c.mu")" = "#!c=600" ] || fail "centre: first line is not #!c=600"
  open=$(sed 1d "$tmp/c.mu" | grep -vc "^${bt}c${bt}.*${bt}f${bt}b\$")
  [ "$open" = 0 ] || fail "centre: $open line(s) not opened by ${bt}c and closed by ${bt}f${bt}b"
  sed -e 1d -e "s/^${bt}c//" -e "s/${bt}f${bt}b\$/${bt}${bt}/" "$tmp/c.mu" >"$tmp/c-left.mu"
  cmp -s "$tmp/c-left.mu" "$data/golden-micron/durin-gate.mu" || fail "centre: the art differs from the left-aligned page"
  grep -q "centred" "$tmp/c.out" || fail "centre: the report doesn't say centred"
  [ "$fails" -eq 0 ] && echo "ok   centre and cache"
else
  fail "centre: export failed: $(cat "$tmp/c.err")"
fi

# refusals: a usage error exits 2, an output refused 1; nothing is written
refuse() {  # refuse <exit> <label> <args...>
  want=$1 label=$2; shift 2
  rm -f "$tmp/r.mu" "$tmp/r.bwp"
  "$cli" "$@" >/dev/null 2>"$tmp/r.err"; rc=$?
  if [ "$rc" -ne "$want" ]; then fail "$label: exit $rc, expected $want ($(cat "$tmp/r.err"))"
  elif [ -e "$tmp/r.mu" ] || [ -e "$tmp/r.bwp" ]; then fail "$label: wrote an output"
  else echo "ok   refuses $label"; fi
}
in=$data/golden/durin-gate.ans
refuse 2 "--irc with --micron" export --irc --micron "$in" "$tmp/r.mu"
refuse 2 "export with neither" export "$in" "$tmp/r.mu"
refuse 2 "--budget with --micron" export --micron --budget 400 "$in" "$tmp/r.mu"
refuse 2 "--fit with --micron" export --micron --fit "$in" "$tmp/r.mu"
refuse 2 "--centre with --irc" export --irc --centre "$in" "$tmp/r.mu"
refuse 2 "--cache with --irc" export --irc --cache 0 "$in" "$tmp/r.mu"
refuse 2 "--cache -1" export --micron --cache -1 "$in" "$tmp/r.mu"
refuse 2 "--cache x" export --micron --cache x "$in" "$tmp/r.mu"
refuse 2 "--centre with convert" convert --centre "$in" "$tmp/r.mu"
refuse 1 "a .bwp output" export --micron "$in" "$tmp/r.bwp"
cp "$in" "$tmp/same.ans"
refuse 1 "the input as output" export --micron "$tmp/same.ans" "$tmp/same.ans"
cmp -s "$in" "$tmp/same.ans" || fail "the input was overwritten"
"$cli" --help >"$tmp/help" 2>&1
grep -q -- "export --micron" "$tmp/help" || fail "--help doesn't list export --micron"
echo "$fails failure(s)"
[ "$fails" -eq 0 ]
