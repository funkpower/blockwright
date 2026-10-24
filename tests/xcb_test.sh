#!/usr/bin/env bash
# Runs an xcb test binary on a virtual X server of its own; exits 77
# (skipped) when Xvfb isn't installed.
set -euo pipefail
if ! command -v xvfb-run >/dev/null; then
    echo "xvfb-run not found: skipped"
    exit 77
fi
exec xvfb-run -a -s "-screen 0 1600x1200x24 -nolisten tcp" env QT_QPA_PLATFORM=xcb "$@"
