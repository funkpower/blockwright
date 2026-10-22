#!/usr/bin/env bash
# forge pr-merge: forge_merge_test.sh <scripts/forge> <scratch dir>
#
# Runs the real script with fake pass and curl first on PATH: curl prints a
# fixture PR on GET and records the --data-binary payload on POST, so nothing
# reaches the forge.
set -uo pipefail
forge=$1 tmp=$2
command -v jq >/dev/null || { echo "jq not found, skipping"; exit 77; }
rm -rf "$tmp" && mkdir -p "$tmp/bin"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

cat > "$tmp/bin/pass" <<'STUB'
#!/bin/sh
echo dummy-token
STUB
cat > "$tmp/bin/curl" <<'STUB'
#!/bin/sh
cat > "$FAKE_DIR/config"
method= data= url=
while [ $# -gt 0 ]; do
  case $1 in
    -X) method=$2; shift ;;
    --data-binary) data=$2; shift ;;
    -H|-K) shift ;;
    -*) ;;
    *) url=$1 ;;
  esac
  shift
done
echo "$method $url" >> "$FAKE_DIR/calls"
case $method in
  GET) cat "$FAKE_DIR/pr.json" ;;
  POST)
    printf '%s' "$data" > "$FAKE_DIR/payload"
    if [ -n "${FAKE_POST_FAIL:-}" ]; then echo '{"message":"conflict"}'; exit 22; fi ;;
esac
STUB
chmod +x "$tmp/bin/pass" "$tmp/bin/curl"
export PATH="$tmp/bin:$PATH" FAKE_DIR=$tmp

# merge <name> <jq filter for the body> <expected message>: pr-merge 7 exits 0,
# prints the success line, and POSTs the squash payload with that message
merge() {
  local name=$1 body=$2 want=$3
  rm -f "$tmp/payload" "$tmp/calls"
  jq -n --rawfile x "$tmp/x" "{number:7,title:\"Add thing\",body:($body)}" > "$tmp/pr.json"
  local out st
  out=$("$forge" pr-merge 7 2>&1); st=$?
  if [[ $st -ne 0 ]]; then bad "$name: exit $st: $out"; return; fi
  [[ $out == "squash-merged #7" ]] || { bad "$name: output: $out"; return; }
  grep -qx "POST https://.*/pulls/7/merge" "$tmp/calls" || { bad "$name: no POST to /pulls/7/merge"; return; }
  grep -q dummy-token "$tmp/config" || { bad "$name: token not passed to curl"; return; }
  if jq -e --arg m "$want" \
      '. == {Do:"squash",MergeTitleField:"Add thing (#7)",MergeMessageField:$m,delete_branch_after_merge:true}' \
      "$tmp/payload" >/dev/null; then
    ok "$name"
  else
    bad "$name: payload $(head -c 300 "$tmp/payload")"
  fi
}

head -c 60000 /dev/zero | tr '\0' x > "$tmp/x"   # $x in the filters
merge "60,010-byte body gives its first paragraph" '"Summary.\n\n" + $x' "Summary."
[[ $(jq -j .body "$tmp/pr.json" | wc -c) -eq 60010 ]] || bad "60,010-byte body is $(jq -j .body "$tmp/pr.json" | wc -c) bytes"
merge "1 MB body gives its first paragraph" '"Summary.\r\n\r\n" + ([range(17)] | map($x) | join("\n"))' "Summary."
merge "null body gives an empty message" null ""
merge "empty body gives an empty message" '""' ""
merge "body with no blank line gives the whole body" '"One.\nTwo.\n"' $'One.\nTwo.'
merge "heading-only body gives the heading" '"## Heading\n"' "## Heading"
merge "leading blank lines are skipped, CRLF is stripped" '"\n  \r\nFirst\r\nsecond\r\n \t\r\nrest"' $'First\nsecond'

rm -f "$tmp/payload"
jq -n '{number:7,title:"Add thing",body:"Summary."}' > "$tmp/pr.json"
out=$(FAKE_POST_FAIL=1 "$forge" pr-merge 7 2>&1); st=$?
if [[ $st -eq 0 ]]; then bad "failed POST: exit 0"
elif [[ $out == *squash-merged* ]]; then bad "failed POST: printed success: $out"
elif [[ ! -s $tmp/payload ]]; then bad "failed POST: no POST made"
else ok "failed POST exits $st without a success line"
fi

echo "$fails failed"
[[ $fails -eq 0 ]]
