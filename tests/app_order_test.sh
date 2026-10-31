#!/usr/bin/env bash
# app_order_test.sh <app test binary>: a test case that closes a window, and so
# saves its layout, runs first in one process with a layout test, and the
# layout test still starts from the default layout (#249). Catch orders the
# pair by seed, so the first seed that lists the closing test first is used.
# Each layout test is named, not tagged, so a tag that later covers more test
# cases can't change the count the run must report.
# Catch's list ends its lines in CRLF on Windows, so a trailing \r is dropped.
set -uo pipefail
bin=$1
first="New and closing ask before dropping what a flat save didn't keep"
fails=0
for second in \
    "Tool Options shows each tool's settings and help without scrolling where the column is tall enough" \
    "reopening Toolbox keeps following the window"; do
  seed=
  for s in $(seq 1 50); do
    top=$("$bin" --list-tests --order rand --rng-seed "$s" "$first,$second" | sed -n 2p | sed 's/^ *//; s/\r$//')
    if [[ $top == "$first" ]]; then seed=$s; break; fi
  done
  if [[ -z $seed ]]; then
    echo "FAIL no seed in 1..50 runs \"$first\" before $second"; fails=$((fails + 1)); continue
  fi
  out=$("$bin" --order rand --rng-seed "$seed" "$first,$second" 2>&1)
  if [[ $? -ne 0 ]] || ! grep -q "(.* in 2 test cases)" <<<"$out"; then
    echo "FAIL $second after the closing test (seed $seed):"; echo "$out" | tail -20; fails=$((fails + 1))
  else
    echo "ok   $second after the closing test (seed $seed)"
  fi
done
exit $((fails > 0))
