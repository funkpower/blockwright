#!/usr/bin/env bash
# The notices test stays inside its staging dir when an install dir is absolute:
#   install_notices_outside_test.sh <source dir> <scratch dir> <generator> <Qt6_DIR>
#
# Configures a CLI-only build of <source dir> whose prefix and
# CMAKE_INSTALL_DOCDIR are absolute paths outside the staging dir (which
# `cmake --install --prefix` would not relocate), runs install_notices_test.sh
# on it, and checks that neither path was created. Configure, build and install
# all name one configuration, so a multi-config generator (which ignores
# CMAKE_BUILD_TYPE) builds the configuration that gets installed.
set -uo pipefail
src=$1 tmp=$2 gen=$3 qt=$4
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
build=$tmp/build prefix=$tmp/outside-prefix docs=$tmp/outside-docs config=Release

cmake -S "$src" -B "$build" -G "$gen" -DQt6_DIR="$qt" -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE="$config" \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_DOCDIR="$docs" >/dev/null || exit 1
nice -n 10 cmake --build "$build" --config "$config" -j 8 --target blockwright-cli >/dev/null || exit 1

bash "$src/tests/install_notices_test.sh" "$build" "$src" "$tmp/notices" "$prefix/bin" "$docs" "$config"; st=$?
for p in "$prefix" "$docs"; do
  if [[ -e $p ]]; then echo "FAIL the install wrote outside staging: $p"; st=1; fi
done
[[ $st -eq 0 ]] && echo "ok   nothing written outside staging"
exit $st
