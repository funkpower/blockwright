# A test generator: a bar of `count` `glyph`s in palette colour `ink`,
# bright when `bright`; with `fail` it fails instead. It reads the compact
# JSON on stdin with sed: every value it reads is a number, a boolean or a
# one-character string.
in=$(cat)
get() { printf '%s' "$in" | sed -n "s/.*\"$1\":\"*\([^,}\"]*\).*/\1/p"; }
if [ "$(get fail)" = true ]; then
  echo "bars: asked to fail" >&2
  exit 1
fi
count=$(get count) ink=$(get ink) glyph=$(get glyph)
sgr=$(echo "0 4 2 6 1 5 3 7" | cut -d' ' -f$((ink % 8 + 1)))
bold=0
if [ "$ink" -ge 8 ] || [ "$(get bright)" = true ]; then bold=1; fi
printf '\033[%s;3%sm' "$bold" "$sgr"
i=0
while [ "$i" -lt "$count" ]; do printf '%s' "$glyph"; i=$((i + 1)); done
printf '\033[0m'
