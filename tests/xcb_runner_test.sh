#!/usr/bin/env bash
# xcb_runner_test.sh <xcb_test.sh> <scratch dir>: holds xcb_test.sh, the xcb
# test binaries' emulator, to its job, with stand-in binaries run the way ctest
# runs the real one, from a caller whose HOME and XDG directories are a
# stand-in for the user's profile:
# - a run sees HOME and every XDG directory in an empty profile of its own, not
#   the caller's, XDG_RUNTIME_DIR a directory there that only the user can
#   enter, on an X server (QT_QPA_PLATFORM=xcb and DISPLAY);
# - a run that writes into that profile fails, and names what it wrote, the
#   runtime directory included;
# - the binary's own status, 77 (skipped) among them, is the runner's;
# - nothing reaches the caller's profile, and a clean run leaves no profile.
# Exits 77 (skipped) when Xvfb isn't installed, as the runner does.
set -uo pipefail
runner=$1 tmp=$2
if ! command -v xvfb-run >/dev/null; then
  echo "xvfb-run not found: skipped"
  exit 77
fi
command rm -rf "$tmp"
mkdir -p "$tmp/user" "$tmp/bin" && mkdir -m 700 "$tmp/user/runtime"
user=$tmp/user runs=$tmp/runs
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }

cat >"$tmp/bin/env-probe" <<'EOF'
#!/usr/bin/env bash
for v in HOME XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME XDG_STATE_HOME XDG_RUNTIME_DIR QT_QPA_PLATFORM DISPLAY; do
  echo "$v=${!v:-}"
done
# POSIX find, not stat, whose options differ between systems
if [[ -n $(find "$XDG_RUNTIME_DIR" -prune -perm 700) ]]; then echo "runtime mode=700"; else echo "runtime mode=$(ls -ld "$XDG_RUNTIME_DIR")"; fi
EOF
cat >"$tmp/bin/writer" <<'EOF'
#!/usr/bin/env bash
mkdir -p "$XDG_CONFIG_HOME/Blockwright" && echo "[window]" >"$XDG_CONFIG_HOME/Blockwright/Blockwright.conf"
EOF
cat >"$tmp/bin/runtime-writer" <<'EOF'
#!/usr/bin/env bash
: >"$XDG_RUNTIME_DIR/probe"
EOF
printf '#!/usr/bin/env bash\nexit 77\n' >"$tmp/bin/skipper"
printf '#!/usr/bin/env bash\nexit 3\n' >"$tmp/bin/failer"
chmod +x "$tmp/bin/"*

run() { # binary: runs it through the runner from the stand-in user profile
  env HOME="$user/home" XDG_CONFIG_HOME="$user/config" XDG_DATA_HOME="$user/data" \
    XDG_CACHE_HOME="$user/cache" XDG_STATE_HOME="$user/state" XDG_RUNTIME_DIR="$user/runtime" \
    bash "$runner" "$runs" "$tmp/bin/$1" 2>&1
}

out=$(run env-probe); st=$?
bad=()
for v in HOME XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME XDG_STATE_HOME XDG_RUNTIME_DIR; do
  val=$(sed -n "s/^$v=//p" <<<"$out")
  [[ $val == "$runs"/env-probe.*/* ]] || bad+=("$v=$val")
done
[[ $out == *"runtime mode=700"* ]] || bad+=("XDG_RUNTIME_DIR not mode 700")
[[ $out == *"QT_QPA_PLATFORM=xcb"* ]] || bad+=("QT_QPA_PLATFORM not xcb")
[[ $out =~ DISPLAY=:[0-9] ]] || bad+=("no DISPLAY")
if [[ $st -eq 0 && ${#bad[@]} -eq 0 ]]; then
  echo "ok   a run's HOME and XDG directories, the runtime one too, are its own profile, on an X server"
else
  fail "a run is not in a profile of its own on an X server (exit $st):"; printf '  %s\n' "${bad[@]}"
  echo "$out" | sed 's/^/  | /'
fi

out=$(run writer); st=$?
if [[ $st -ne 0 && $out == *"FAIL writer wrote into the profile"* && $out == *"./config/Blockwright/Blockwright.conf"* ]]; then
  echo "ok   a run that writes into its profile fails and names the file"
else
  fail "a run that wrote into its profile was not failed (exit $st):"; echo "$out" | sed 's/^/  | /'
fi

out=$(run runtime-writer); st=$?
if [[ $st -ne 0 && $out == *"FAIL runtime-writer wrote into the profile"* && $out == *"./runtime/probe"* ]]; then
  echo "ok   a run that writes into its runtime directory fails and names the file"
else
  fail "a run that wrote into its runtime directory was not failed (exit $st):"; echo "$out" | sed 's/^/  | /'
fi

for bin in skipper:77 failer:3; do
  run "${bin%%:*}" >/dev/null; st=$?
  if [[ $st -eq ${bin#*:} ]]; then
    echo "ok   the runner exits ${bin#*:} when the binary does"
  else
    fail "the binary exited ${bin#*:}, the runner $st"
  fi
done

written=$(cd "$user" && find . -mindepth 1 ! -path ./runtime | sort)
if [[ -z $written ]]; then
  echo "ok   nothing reached the caller's profile"
else
  fail "the runs wrote into the caller's profile:"; echo "$written" | sed 's/^/  /'
fi
left=$(cd "$runs" 2>/dev/null && find . -mindepth 1 -maxdepth 1 ! -name 'writer.*' ! -name 'runtime-writer.*' | sort)
if [[ -z $left ]]; then
  echo "ok   only the failed run's profile is kept"
else
  fail "profiles of runs that left theirs empty were kept:"; echo "$left" | sed 's/^/  /'
fi
exit $((fails > 0))
