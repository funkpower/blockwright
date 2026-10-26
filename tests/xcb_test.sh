#!/usr/bin/env bash
# xcb_test.sh <scratch dir> <test binary> [args...]: what the tests of the xcb
# test binaries run them through (BW_APP_XCB_TEST_ENV, written into the test's
# command, not left to CROSSCOMPILING_EMULATOR). Runs the binary on a virtual X
# server of its own, with HOME and the XDG directories in an empty profile made
# for the run under <scratch dir> (XDG_RUNTIME_DIR too, made mode 700 as the
# spec asks, the one thing in it), and fails if the run leaves anything in it,
# as app_profile_test.sh does for the offscreen binaries; the profile is kept
# for a look only then. The server runs without GLX: the app draws without
# OpenGL, and Xvfb's GL driver (Mesa's) keeps its shader cache in the profile.
# Exits 77 (skipped) when Xvfb isn't installed.
set -uo pipefail
if ! command -v xvfb-run >/dev/null; then
    echo "xvfb-run not found: skipped"
    exit 77
fi
scratch=$1; shift
name=$(basename "$1")
mkdir -p "$scratch" && profile=$(mktemp -d "$scratch/$name.XXXXXX") && mkdir -m 700 "$profile/runtime" || exit 1
env HOME="$profile/home" XDG_CONFIG_HOME="$profile/config" XDG_DATA_HOME="$profile/data" \
    XDG_CACHE_HOME="$profile/cache" XDG_STATE_HOME="$profile/state" XDG_RUNTIME_DIR="$profile/runtime" \
    xvfb-run -a -s "-screen 0 1600x1200x24 -nolisten tcp -extension GLX" \
    env QT_QPA_PLATFORM=xcb "$@"
st=$?
written=$(cd "$profile" && find . -mindepth 1 ! -path ./runtime | sort)
if [[ -n $written ]]; then
    echo "FAIL $name wrote into the profile ($profile):"; echo "$written" | sed 's/^/  /'
    exit 1
fi
rmdir "$profile/runtime" "$profile"
exit $st
