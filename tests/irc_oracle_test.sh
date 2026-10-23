#!/usr/bin/env bash
# irc_oracle_test.sh <blockwright-cli> <tests/data> <scratch dir>: `cli export
# --irc` writes each golden piece byte for byte as the committed golden-irc
# output, which is a fixed oracle: it changes only when the encoder's output
# is meant to change.
set -uo pipefail
cli=$1 data=$2 tmp=$3
rm -rf "$tmp" && mkdir -p "$tmp"
fails=0 n=0
for f in "$data"/golden/*.ans; do
  name=$(basename "$f" .ans)
  n=$((n + 1))
  if ! "$cli" export --irc --allow-over-budget "$f" "$tmp/$name.txt" >/dev/null 2>"$tmp/$name.err"; then
    echo "FAIL $name: export --irc failed: $(<"$tmp/$name.err")"; fails=$((fails + 1))
  elif cmp -s "$tmp/$name.txt" "$data/golden-irc/$name.txt"; then
    echo "ok   $name"
  else
    echo "FAIL $name: export --irc output differs from golden-irc/$name.txt"; fails=$((fails + 1))
  fi
done
extra=$(comm -13 <(cd "$data/golden" && ls *.ans | sed 's/\.ans$//') <(cd "$data/golden-irc" && ls *.txt | sed 's/\.txt$//'))
if [[ -n $extra ]]; then echo "FAIL golden-irc outputs with no piece: $extra"; fails=$((fails + 1)); fi
[[ $n -gt 0 ]] || { echo "FAIL no pieces"; fails=$((fails + 1)); }
echo "$fails failure(s)"
[[ $fails -eq 0 ]]
