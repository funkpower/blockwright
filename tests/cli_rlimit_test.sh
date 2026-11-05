#!/usr/bin/env bash
# replay's open-file refusal when the soft limit can't be raised:
# cli_rlimit_test.sh <blockwright-cli> <setrlimit shim> <scratch dir>. The
# shim (setrlimit_fails.cpp) makes a setrlimit of the open-file limit fail
# and passes every other resource through; the message names the limit still
# in force, not the one asked for. Skips (77) where LD_PRELOAD isn't
# honoured; fails where the shim doesn't load.
set -uo pipefail
cli=$1 shim=$2 tmp=$3
rm -rf "$tmp" && mkdir -p "$tmp"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
# a sanitizer runtime refuses to start after a preloaded library (and sets
# RLIMIT_CORE at startup, which the shim passes through)
export ASAN_OPTIONS=${ASAN_OPTIONS:+$ASAN_OPTIONS:}verify_asan_link_order=0

if ! (ulimit -S -n 32) 2>/dev/null; then echo "skip: the hard open-file limit is under 32"; exit 77; fi
# The probe's lowering succeeds silently where LD_PRELOAD isn't honoured
# (skip), fails with EPERM where the shim took (run), and anything else is
# the shim failing to load, an unresolved symbol, say (fail). It runs in the
# C locale, as bash's message is translated otherwise (LANGUAGE too goes,
# though gettext ignores it under C)
probe=$(LC_ALL=C LANGUAGE= LD_PRELOAD=$shim bash -c 'ulimit -S -n 31' 2>&1)
if [[ -z $probe ]]; then
  echo "skip: LD_PRELOAD doesn't take the shim here"; exit 77
elif [[ $probe != *'not permitted'* ]]; then
  echo "FAIL the shim didn't load: $probe"; exit 1
fi

{
  printf '{"version": 1, "start": {"blank": [8, 4]}, "steps": ['
  for i in $(seq 0 29); do printf '%s{"name": "s%d", "action": "glyph", "points": [[%d, %d]]}' "$([[ $i -gt 0 ]] && echo ,)" "$i" $((i % 8)) $((i / 8)); done
  printf ']}'
} > "$tmp/many.json"
out=$(bash -c 'ulimit -S -n 32 && LD_PRELOAD=$1 exec "$2" replay "$3" -o "$4"' _ "$shim" "$cli" "$tmp/many.json" "$tmp/out" 2>&1); st=$?
want='blockwright-cli: not written: 30 outputs need 94 open files at once (one each and 64 to spare), more than the open-file limit of 32'
if [[ $st -eq 1 && $out == "$want" ]]; then ok "replay: setrlimit fails, the limit in force named"; else bad "replay with setrlimit failing: exit $st: $out"; fi
if [[ ! -e $tmp/out ]]; then ok "replay: setrlimit fails, nothing made"; else bad "replay with setrlimit failing made $tmp/out"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
