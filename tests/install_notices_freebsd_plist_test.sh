#!/bin/sh
# The FreeBSD port lists every notice the install carries, and nothing more:
#   install_notices_freebsd_plist_test.sh <build dir> <source dir> <scratch dir> <full docdir> [<config>]
#
# Installs the build with DESTDIR=<scratch>/stage and compares the files below
# <full docdir> (the build's CMAKE_INSTALL_FULL_DOCDIR) with the %%DOCSDIR%%/
# lines of packaging/freebsd/pkg-plist, so a notice added to the install and
# left out of the plist (or the other way round) fails here and not first in
# the port's `make check-plist`. <config> is passed to `cmake --install
# --config` (needed for a multi-config generator; empty or absent uses cmake's
# default).
set -u
build=$1 src=$2 tmp=$3 docdir=$4 config=${5:-}
plist=$src/packaging/freebsd/pkg-plist
rm -rf "$tmp" && mkdir -p "$tmp" || exit 1
stage=$tmp/stage

if ! DESTDIR=$stage cmake --install "$build" ${config:+--config "$config"} >/dev/null; then
  echo "FAIL cmake --install"; exit 1
fi
if [ ! -d "$stage$docdir" ]; then
  echo "FAIL nothing installed in $docdir"; exit 1
fi
(cd "$stage$docdir" && find . -type f) | sed 's|^\./|%%DOCSDIR%%/|' | LC_ALL=C sort > "$tmp/installed"
grep '^%%DOCSDIR%%/' "$plist" | LC_ALL=C sort > "$tmp/listed"

fails=0
missing=$(LC_ALL=C comm -23 "$tmp/installed" "$tmp/listed")
extra=$(LC_ALL=C comm -13 "$tmp/installed" "$tmp/listed")
if [ -z "$missing" ]; then
  echo "ok   pkg-plist lists every installed notice"
else
  echo "FAIL installed but not in pkg-plist:"; echo "$missing"; fails=$((fails + 1))
fi
if [ -z "$extra" ]; then
  echo "ok   every notice in pkg-plist is installed"
else
  echo "FAIL in pkg-plist but not installed:"; echo "$extra"; fails=$((fails + 1))
fi
if [ -s "$tmp/installed" ]; then
  echo "ok   $(wc -l < "$tmp/installed" | tr -d ' ') notices compared"
else
  echo "FAIL no notices installed"; fails=$((fails + 1))
fi
echo "$fails failure(s)"
[ "$fails" -eq 0 ]
