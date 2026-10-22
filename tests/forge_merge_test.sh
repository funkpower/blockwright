#!/usr/bin/env bash
# scripts/forge: forge_merge_test.sh <scripts/forge> <scratch dir>
#
# Runs the real script with fake pass and curl first on PATH: curl prints a
# fixture PR on GET and records the --data-binary payload on POST, so nothing
# reaches the forge. Every request also checks its own arguments: one longer
# than 4 KiB is logged to $tmp/longargs and the request fails, so a body passed
# on the command line fails here even where the kernel would let it through.
# pass prints a two-line entry (FAKE_PASS replaces it), neither line of which
# may ever reach the script's output, even under bash -x.
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
if [ -n "${FAKE_PASS+set}" ]; then printf '%s' "$FAKE_PASS"
else printf 'dummy-token\nsecond-secret\n'
fi
STUB
cat > "$tmp/bin/curl" <<'STUB'
#!/bin/sh
method= data= url= max=0
for a in "$@"; do [ ${#a} -gt $max ] && max=${#a}; done
while [ $# -gt 0 ]; do
  case $1 in
    -X) method=$2; shift ;;
    --data-binary) data=$2; shift ;;
    -K) cat "$2" > "$FAKE_DIR/config"; shift ;;
    -H) shift ;;
    -*) ;;
    *) url=$1 ;;
  esac
  shift
done
echo "$method $url" >> "$FAKE_DIR/calls"
if [ $max -gt 4096 ]; then
  echo "$method $url: $max-byte argument" | tee -a "$FAKE_DIR/longargs" >&2; exit 98
fi
case $data in
  @-) cat > "$FAKE_DIR/payload" ;;
  @*) cat "${data#@}" > "$FAKE_DIR/payload" ;;
  ?*) printf '%s' "$data" > "$FAKE_DIR/payload" ;;
esac
case $method in
  GET) cat "$FAKE_DIR/pr.json" ;;
  *)
    if [ -n "${FAKE_POST_FAIL:-}" ]; then echo '{"message":"conflict"}'; exit 22; fi
    echo '{"number":8,"html_url":"https://forge.test/issues/8"}' ;;
esac
STUB
chmod +x "$tmp/bin/pass" "$tmp/bin/curl"
export PATH="$tmp/bin:$PATH" FAKE_DIR=$tmp
unset FAKE_PASS

# merge <name> <jq filter for the body> <expected message>: pr-merge 7 exits 0,
# prints the success line, and POSTs the squash payload with that message
merge() {
  local name=$1 body=$2 want=$3
  rm -f "$tmp/payload" "$tmp/calls"
  jq -n --rawfile x "$tmp/x" --rawfile big "$tmp/big" "{number:7,title:\"Add thing\",body:($body)}" > "$tmp/pr.json"
  local out st
  out=$("$forge" pr-merge 7 2>&1); st=$?
  if [[ $st -ne 0 ]]; then bad "$name: exit $st: $out"; return; fi
  [[ $out == "squash-merged #7" ]] || { bad "$name: output: $out"; return; }
  grep -qx "POST https://.*/pulls/7/merge" "$tmp/calls" || { bad "$name: no POST to /pulls/7/merge"; return; }
  [[ $(cat "$tmp/config") == 'header = "Authorization: token dummy-token"' ]] \
    || { bad "$name: curl config is not the first line of the pass entry"; return; }
  if jq -e --rawfile m <(printf '%s' "$want") \
      '. == {Do:"squash",MergeTitleField:"Add thing (#7)",MergeMessageField:$m,delete_branch_after_merge:true}' \
      "$tmp/payload" >/dev/null; then
    ok "$name"
  else
    bad "$name: payload $(jq -c '.MergeMessageField |= .[:200]' "$tmp/payload" 2>/dev/null)"
  fi
}

head -c 60000 /dev/zero | tr '\0' x > "$tmp/x"      # $x in the filters
head -c 1048576 /dev/zero | tr '\0' x > "$tmp/big"  # $big
merge "60,010-byte body gives its first paragraph" '"Summary.\n\n" + $x' "Summary."
[[ $(jq -j .body "$tmp/pr.json" | wc -c) -eq 60010 ]] || bad "60,010-byte body is $(jq -j .body "$tmp/pr.json" | wc -c) bytes"
merge "1 MB body gives its first paragraph" '"Summary.\r\n\r\n" + ([range(17)] | map($x) | join("\n"))' "Summary."
merge "null body gives an empty message" null ""
merge "empty body gives an empty message" '""' ""
merge "body with no blank line gives the whole body" '"One.\nTwo.\n"' $'One.\nTwo.'
merge "heading-only body gives the heading" '"## Heading\n"' "## Heading"
merge "leading blank lines are skipped, CRLF is stripped" '"\n  \r\nFirst\r\nsecond\r\n \t\r\nrest"' $'First\nsecond'

# A 1 MiB first paragraph is more than one argv element may hold (#23).
merge "1 MiB single-paragraph body is the whole message" '$big' "$(cat "$tmp/big")"
[[ $(jq -j .body "$tmp/pr.json" | wc -c) -eq 1048576 ]] || bad "1 MiB body is $(jq -j .body "$tmp/pr.json" | wc -c) bytes"

# send <name> <expected .body> <expected stdout> <forge args...>: the command
# exits 0, prints the expected line and POSTs a payload whose body is the file
send() {
  local name=$1 want=$2 wantout=$3; shift 3
  rm -f "$tmp/payload" "$tmp/calls"
  local out st
  out=$("$forge" "$@" 2>&1); st=$?
  if [[ $st -ne 0 ]]; then bad "$name: exit $st: $(head -c 300 <<<"$out")"; return; fi
  [[ $out == "$wantout" ]] || { bad "$name: output: $(head -c 300 <<<"$out")"; return; }
  if cmp -s <(jq -j .body "$tmp/payload") "$want"; then ok "$name"
  else bad "$name: body is $(jq -j .body "$tmp/payload" 2>/dev/null | wc -c) bytes"
  fi
}

send "pr-create sends a 1 MiB body" "$tmp/big" 8 pr-create p0-x "Add thing" "$tmp/big"
jq -e '.head == "p0-x" and .base == "main" and .title == "Add thing"' "$tmp/payload" >/dev/null \
  || bad "pr-create: payload fields $(jq -c 'del(.body)' "$tmp/payload")"
grep -qx "POST https://.*/pulls" "$tmp/calls" || bad "pr-create: no POST to /pulls"
send "comment sends a 1 MiB body" "$tmp/big" https://forge.test/issues/8 comment 23 "$tmp/big"
grep -qx "POST https://.*/issues/23/comments" "$tmp/calls" || bad "comment: no POST to /issues/23/comments"
send "issue-create sends a 1 MiB body" "$tmp/big" 8 issue-create "A bug" "$tmp/big"
jq -e '.title == "A bug"' "$tmp/payload" >/dev/null || bad "issue-create: title $(jq -c .title "$tmp/payload")"

# pr-merge with an explicit 1 MiB message file
rm -f "$tmp/payload"
jq -n '{number:7,title:"Add thing",body:"Summary."}' > "$tmp/pr.json"
out=$("$forge" pr-merge 7 "Custom" "$tmp/big" 2>&1); st=$?
if [[ $st -ne 0 ]]; then bad "1 MiB message file: exit $st: $(head -c 300 <<<"$out")"
elif jq -e '.MergeTitleField == "Custom" and .Do == "squash"' "$tmp/payload" >/dev/null \
    && cmp -s <(jq -j .MergeMessageField "$tmp/payload") "$tmp/big"; then
  ok "1 MiB message file is the whole message"
else bad "1 MiB message file: payload $(jq -c '.MergeMessageField |= length' "$tmp/payload" 2>/dev/null)"
fi

for c in "pr-create p0-x T" "comment 23" "issue-create T"; do
  rm -f "$tmp/calls"
  # shellcheck disable=SC2086
  if "$forge" $c "$tmp/missing" >/dev/null 2>&1; then bad "${c%% *} with a missing body file: exit 0"
  elif [[ -e $tmp/calls ]]; then bad "${c%% *} with a missing body file: called $(cat "$tmp/calls")"
  else ok "${c%% *} with a missing body file fails before any request"
  fi
done

rm -f "$tmp/payload" "$tmp/calls"
"$forge" issue-close 23 >/dev/null 2>&1 || bad "issue-close: exit $?"
if grep -qx "PATCH https://.*/issues/23" "$tmp/calls" && jq -e '. == {state:"closed"}' "$tmp/payload" >/dev/null
then ok "issue-close PATCHes state closed"
else bad "issue-close: $(cat "$tmp/calls" 2>/dev/null) $(cat "$tmp/payload" 2>/dev/null)"
fi

rm -f "$tmp/payload"
jq -n '{number:7,title:"Add thing",body:"Summary."}' > "$tmp/pr.json"
out=$(FAKE_POST_FAIL=1 "$forge" pr-merge 7 2>&1); st=$?
if [[ $st -eq 0 ]]; then bad "failed POST: exit 0"
elif [[ $out == *squash-merged* ]]; then bad "failed POST: printed success: $out"
elif [[ ! -s $tmp/payload ]]; then bad "failed POST: no POST made"
else ok "failed POST exits $st without a success line"
fi

# bash -x must not show the token or the rest of the pass entry (Codex P1)
jq -n '{number:7,title:"Add thing",body:"Summary."}' > "$tmp/pr.json"
for c in "pr-get 7" "pr-merge 7" "comment 23 $tmp/x"; do
  rm -f "$tmp/config"
  # shellcheck disable=SC2086
  out=$(bash -x "$forge" $c 2>&1); st=$?
  if [[ $st -ne 0 ]]; then bad "bash -x ${c%% *}: exit $st"
  elif [[ $out != *"+ call "* ]]; then bad "bash -x ${c%% *}: no trace in the output"
  elif [[ $out == *dummy-token* || $out == *second-secret* ]]; then bad "bash -x ${c%% *}: the pass entry is in the output"
  elif [[ $(cat "$tmp/config") != 'header = "Authorization: token dummy-token"' ]]; then
    bad "bash -x ${c%% *}: token not passed to curl"
  else ok "bash -x ${c%% *} shows neither line of the pass entry"
  fi
done
# the merge request runs in the script's own shell; tracing resumes after it
[[ $(bash -x "$forge" pr-merge 7 2>&1) == *"+ echo 'squash-merged #7'"* ]] \
  && ok "bash -x traces again after a request" || bad "bash -x: xtrace not restored after a request"

# A token that is empty or would break curl's quoted config value is refused
# before any request, without being printed (Codex P2)
for t in "" 'ab"cd' 'ab\cd' 'ab cd' $'\nab'; do
  rm -f "$tmp/calls"
  out=$(FAKE_PASS=$t "$forge" pr-get 7 2>&1); st=$?
  name="token $(printf '%q' "$t")"
  if [[ $st -eq 0 ]]; then bad "$name: exit 0"
  elif [[ -e $tmp/calls ]]; then bad "$name: called $(cat "$tmp/calls")"
  elif [[ -n $t && $out == *"$t"* ]]; then bad "$name: printed the token"
  elif [[ $out != "forge: the forgejo/pumpin-funkpower token is empty or not a token" ]]; then
    bad "$name: output: $out"
  else ok "$name is refused before any request"
  fi
done
rm -f "$tmp/config"
FAKE_PASS=$'0123abcdef._~+/=-Z\nnext\n' "$forge" pr-get 7 >/dev/null 2>&1 || bad "token alphabet: exit $?"
[[ $(cat "$tmp/config" 2>/dev/null) == 'header = "Authorization: token 0123abcdef._~+/=-Z"' ]] \
  && ok "a token from the whole alphabet reaches curl unchanged" \
  || bad "token alphabet: config $(cat "$tmp/config" 2>/dev/null)"

# every stubbed request, whatever the case, kept its arguments under 4 KiB
if [[ -e $tmp/longargs ]]; then bad "long curl arguments: $(cat "$tmp/longargs")"
else ok "no request had a curl argument over 4 KiB"
fi

echo "$fails failed"
[[ $fails -eq 0 ]]
