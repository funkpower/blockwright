#!/usr/bin/env bash
# micron_cli_test.sh <blockwright-cli> <tests/data> <scratch dir>: `cli export
# --micron` writes each golden-micron page byte for byte from its piece (an
# .ans, an XBIN, or the same .ans packed as a project), centres and writes
# the cache header on request, and refuses the IRC export's options; and a
# page (.mu) as input opens as the Micron target, reports what it drops, and
# writes again as the same page.
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
# a page as input: it opens as the Micron target, and a page the writer wrote
# opens to the cells it came from, so writing it again gives the same page
for name in swatch swatch-ice-off durin-gate darkmage tux-palette-font; do
  if "$cli" export --micron "$data/golden-micron/$name.mu" "$tmp/again-$name.mu" >/dev/null 2>"$tmp/again.err" &&
    cmp -s "$tmp/again-$name.mu" "$data/golden-micron/$name.mu"; then echo "ok   $name.mu reopens"
  else fail "$name.mu: reopened and written again it differs ($(cat "$tmp/again.err"))"; fi
done
if "$cli" export --micron --centre --cache 0 "$data/golden-micron/swatch-centre.mu" "$tmp/again-centre.mu" \
    >/dev/null 2>"$tmp/again.err" && cmp -s "$tmp/again-centre.mu" "$data/golden-micron/swatch-centre.mu"; then
  echo "ok   swatch-centre.mu reopens"
else fail "swatch-centre.mu: reopened and written again it differs"; fi
grep -q "12 lines centred or right-aligned at the page width, the first on line 2" "$tmp/again.err" ||
  fail "swatch-centre.mu: no alignment warning: $(cat "$tmp/again.err")"
if [ -e "$tmp/rgb-bg.mu" ] && "$cli" export --micron "$tmp/rgb-bg.mu" "$tmp/rgb-bg-again.mu" >/dev/null 2>&1 &&
  cmp -s "$tmp/rgb-bg.mu" "$tmp/rgb-bg-again.mu"; then echo "ok   rgb-bg.mu reopens"
else fail "rgb-bg.mu: reopened and written again it differs"; fi

pages=$data/micron
if "$cli" info "$pages/links-fields.mu" >"$tmp/info.out" 2>"$tmp/info.err"; then
  grep -q "^format: *Micron page" "$tmp/info.out" || fail "info: no Micron format: $(cat "$tmp/info.out")"
  grep -q "^target: *micron" "$tmp/info.out" || fail "info: target is not micron"
  grep -q "^size: *31x7" "$tmp/info.out" || fail "info: size is not 31x7"
  grep -q "2 links drawn as their labels, the first on line 1" "$tmp/info.err" || fail "info: no link warning"
  grep -q "6 fields drawn as their text, the first on line 2" "$tmp/info.err" || fail "info: no field warning"
  [ "$fails" -eq 0 ] && echo "ok   info on a page"
else fail "info on a page failed: $(cat "$tmp/info.err")"; fi
"$cli" lint "$pages/links-fields.mu" >"$tmp/lint.out" 2>&1 || fail "lint on a page failed: $(cat "$tmp/lint.out")"
grep -q "fine for Micron" "$tmp/lint.out" && echo "ok   lint takes a page as Micron" ||
  fail "lint doesn't take a page as Micron: $(cat "$tmp/lint.out")"
if "$cli" pack "$pages/sections.mu" "$tmp/sections.bwp" >/dev/null 2>"$tmp/pack.err" &&
  "$cli" info "$tmp/sections.bwp" >"$tmp/pack.out" 2>&1 && grep -q "^target: *micron" "$tmp/pack.out"; then
  grep -q "5 headings drawn as text in their heading colours, the first on line 1" "$tmp/pack.err" &&
    echo "ok   pack keeps the Micron target" || fail "pack: no heading warning: $(cat "$tmp/pack.err")"
else fail "pack: a page doesn't pack as a Micron project: $(cat "$tmp/pack.out" "$tmp/pack.err")"; fi
if "$cli" render "$data/golden-micron/swatch.mu" "$tmp/swatch.png" >/dev/null 2>&1 && [ -s "$tmp/swatch.png" ]; then
  echo "ok   render a page"
else fail "render: a page doesn't render"; fi
# RGB colours don't fit an .ans: refused, then written with --allow-lossy
refuse 1 "a page's RGB colours as .ans" convert "$pages/colours.mu" "$tmp/r.mu.ans"
[ -e "$tmp/r.mu.ans" ] && fail "a refused conversion wrote its output"
if "$cli" convert --allow-lossy "$pages/colours.mu" "$tmp/colours.ans" >/dev/null 2>&1 && [ -s "$tmp/colours.ans" ]; then
  echo "ok   convert a page"
else fail "convert --allow-lossy: a page doesn't convert"; fi

# a steps file can start from a page
mkdir -p "$tmp/replay"
cp "$pages/sections.mu" "$tmp/replay/"
printf '{"version": 1, "start": {"file": "sections.mu"}, "steps": [{"name": "one", "action": "selectAll"}]}\n' \
  >"$tmp/replay/steps.json"
if "$cli" replay --allow-lossy "$tmp/replay/steps.json" -o "$tmp/replay/out" >/dev/null 2>&1 &&
  grep -aq "text two" "$tmp/replay/out/01-one.ans" && ! grep -aq ">>Sub" "$tmp/replay/out/01-one.ans"; then
  echo "ok   replay starts from a page"
else fail "replay: a page as the start isn't read as a page"; fi

"$cli" --help >"$tmp/help" 2>&1
grep -q -- "export --micron" "$tmp/help" || fail "--help doesn't list export --micron"
echo "$fails failure(s)"
[ "$fails" -eq 0 ]
