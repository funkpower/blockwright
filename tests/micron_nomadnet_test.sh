#!/bin/sh
# micron_nomadnet_test.sh <python> <tests dir>: NomadNet's own Micron parser
# reads every page in tests/data/golden-micron as the cells its .json gives
# (micron_parser_check.py). Skips (77) when <python> can't import nomadnet.
py=$1 dir=$2
command -v "$py" >/dev/null 2>&1 || { echo "skip: no $py"; exit 77; }
"$py" -c "import nomadnet.ui.textui.MicronParser" >/dev/null 2>&1 ||
  { echo "skip: $py can't import nomadnet (pip install nomadnet)"; exit 77; }
fails=0 n=0
for page in "$dir"/data/golden-micron/*.mu; do
  n=$((n + 1))
  name=$(basename "$page" .mu)
  if "$py" "$dir/micron_parser_check.py" "$page" "${page%.mu}.json"; then
    echo "ok   $name"
  else
    echo "FAIL $name"; fails=$((fails + 1))
  fi
done
[ "$n" -gt 0 ] || { echo "FAIL no pages"; fails=1; }
echo "$fails failure(s)"
[ "$fails" -eq 0 ]
