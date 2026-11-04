#!/usr/bin/env bash
# app_refusal_test.sh <scratch dir> <test binary>...: when the run's scratch
# directory can't be made (TMPDIR points nowhere), each binary refuses to run
# with exit 1 and leaves a profile that already holds Blockwright settings
# byte-identical. Without the scratch directory the app's settings would be the
# profile's own, so nothing may be written or cleared before the check (#252).
set -uo pipefail
tmp=$1; shift
fails=0
conf='[window]\ngeometry=keepme\n[fonts]\nfolder=/some/real/folder\n[recent]\nfiles=a.ans\n'
# each entry's type and path, then each file's hash; POSIX find (no -printf),
# which BSD find lacks
type_of() { if [[ -L $1 ]]; then echo l; elif [[ -d $1 ]]; then echo d; elif [[ -f $1 ]]; then echo f; else echo o; fi; }
snapshot() {
  (cd "$1" && find . -mindepth 1 | LC_ALL=C sort | while IFS= read -r p; do echo "$(type_of "$p") $p"; done &&
     find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum)
}
for bin in "$@"; do
  name=$(basename "$bin")
  profile=$tmp/$name
  rm -rf "$profile" && mkdir -p "$profile"/{home/.config/Blockwright,config/Blockwright,data,cache,state}
  printf "$conf" >"$profile/config/Blockwright/Blockwright.conf"
  printf "$conf" >"$profile/home/.config/Blockwright/Blockwright.conf"
  before=$(snapshot "$profile")
  env TMPDIR="$tmp/no-such-dir/tmp" HOME="$profile/home" XDG_CONFIG_HOME="$profile/config" \
      XDG_DATA_HOME="$profile/data" XDG_CACHE_HOME="$profile/cache" XDG_STATE_HOME="$profile/state" \
      QT_QPA_PLATFORM=offscreen "$bin" >"$tmp/$name.log" 2>&1
  rc=$?
  after=$(snapshot "$profile")
  if [[ $rc -ne 1 ]] || ! grep -q "not in the scratch directory; not running" "$tmp/$name.log"; then
    echo "FAIL $name: exit $rc, expected 1 and a refusal (see $tmp/$name.log)"; fails=$((fails + 1))
  elif [[ $before != "$after" ]]; then
    echo "FAIL $name changed the profile:"; diff <(echo "$before") <(echo "$after") | sed 's/^/  /'
    fails=$((fails + 1))
  else
    echo "ok   $name"
  fi
done
exit $((fails > 0))
