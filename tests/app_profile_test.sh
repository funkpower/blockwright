#!/usr/bin/env bash
# app_profile_test.sh <scratch dir> <test binary>...: each binary, run whole
# with HOME and the XDG directories pointing into an empty profile, leaves that
# profile empty. That is where the user's real settings would be on Linux (the
# QtProject store a QFileDialog writes, Blockwright's own); macOS keeps native
# settings in CFPreferences and Windows in the registry, which this can't see.
# fontconfig's cache is left out.
set -uo pipefail
tmp=$1; shift
fails=0
for bin in "$@"; do
  name=$(basename "$bin")
  profile=$tmp/$name
  rm -rf "$profile" && mkdir -p "$profile"
  if ! env HOME="$profile/home" XDG_CONFIG_HOME="$profile/config" XDG_DATA_HOME="$profile/data" \
      XDG_CACHE_HOME="$profile/cache" XDG_STATE_HOME="$profile/state" QT_QPA_PLATFORM=offscreen \
      "$bin" >"$tmp/$name.log" 2>&1; then
    echo "FAIL $name: the tests failed (see $tmp/$name.log)"; fails=$((fails + 1)); continue
  fi
  # fontconfig's own cache, which it writes to the user's cache directory
  # whenever the system's is out of date for a font directory, isn't the
  # program's (a fresh build container can have stale system caches)
  written=$(cd "$profile" && find . -mindepth 1 ! -path ./cache ! -path ./cache/fontconfig ! -path './cache/fontconfig/*' | sort)
  if [[ -n $written ]]; then
    echo "FAIL $name wrote into the profile:"; echo "$written" | sed 's/^/  /'; fails=$((fails + 1))
  else
    echo "ok   $name"
  fi
done
exit $((fails > 0))
