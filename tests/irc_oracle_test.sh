#!/usr/bin/env bash
# irc_oracle_test.sh <ans2irc> <tests/data>: the committed golden-irc outputs
# are what the ans2irc CLI from the pinned submodule writes.
set -uo pipefail
ans2irc=$1 data=$2
fails=0 n=0
for f in "$data"/golden/*.ans; do
  name=$(basename "$f" .ans)
  n=$((n + 1))
  if "$ans2irc" "$f" | cmp -s - "$data/golden-irc/$name.txt"; then
    echo "ok   $name"
  else
    echo "FAIL $name: ans2irc output differs from golden-irc/$name.txt"; fails=$((fails + 1))
  fi
done
extra=$(comm -13 <(cd "$data/golden" && ls *.ans | sed 's/\.ans$//') <(cd "$data/golden-irc" && ls *.txt | sed 's/\.txt$//'))
if [[ -n $extra ]]; then echo "FAIL golden-irc outputs with no piece: $extra"; fails=$((fails + 1)); fi
[[ $n -gt 0 ]] || { echo "FAIL no pieces"; fails=$((fails + 1)); }
echo "$fails failure(s)"
[[ $fails -eq 0 ]]
