#!/usr/bin/env bash
# A build without network never downloads Catch2:
#   catch2_offline_test.sh <source dir> <scratch dir> <generator> <Qt6_DIR> [<prefix path> [<Catch2_DIR>]]
#
# <prefix path> is the outer build's CMAKE_PREFIX_PATH with ':' between
# entries. <Catch2_DIR> is the installed Catch2 3 the outer build found, if it
# found one.
#
# With BW_FETCH_CATCH2=OFF and no Catch2 to find, the configure stops with a
# message naming the ways out and downloads nothing; with BUILD_TESTING=OFF as
# well it succeeds without Catch2. Given an installed Catch2, BW_FETCH_CATCH2=OFF
# configures the tests against it.
set -uo pipefail
src=$1 tmp=$2 gen=$3 qt=$4 prefixes=${5:-} catch2=${6:-}
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# configure <name> <args...>: configure <src> into <tmp>/<name>, log in <tmp>/<name>.log
configure() {
  local name=$1; shift
  cmake -S "$src" -B "$tmp/$name" -G "$gen" -DQt6_DIR="$qt" -DCMAKE_PREFIX_PATH="${prefixes//:/;}" \
    -DBW_FETCH_CATCH2=OFF "$@" >"$tmp/$name.log" 2>&1
}

configure no-catch2 -DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON; st=$?
log=$(tr -s ' \n' ' ' < "$tmp/no-catch2.log")
if [[ $st -ne 0 && $log == *"BW_FETCH_CATCH2 is OFF"* && $log == *"-DBUILD_TESTING=OFF"* ]]; then
  ok "no Catch2 and BW_FETCH_CATCH2=OFF: the configure stops and says why"
else
  bad "no Catch2 and BW_FETCH_CATCH2=OFF (exit $st): $(cat "$tmp/no-catch2.log")"
fi
if [[ -e $tmp/no-catch2/_deps ]]; then
  bad "no Catch2 and BW_FETCH_CATCH2=OFF: something was fetched into _deps/"
else
  ok "nothing fetched"
fi

if configure no-tests -DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON -DBUILD_TESTING=OFF; then
  if [[ -e $tmp/no-tests/_deps || -e $tmp/no-tests/tests ]]; then
    bad "BUILD_TESTING=OFF: the tests or Catch2 were configured anyway"
  else
    ok "BUILD_TESTING=OFF configures without Catch2 and without the tests"
  fi
else
  bad "BUILD_TESTING=OFF without Catch2: $(cat "$tmp/no-tests.log")"
fi

if [[ -n $catch2 ]]; then
  if configure installed -DCatch2_DIR="$catch2" && [[ -f $tmp/installed/tests/CTestTestfile.cmake && ! -e $tmp/installed/_deps ]]; then
    ok "BW_FETCH_CATCH2=OFF uses the installed Catch2"
  else
    bad "BW_FETCH_CATCH2=OFF with Catch2 at $catch2: $(cat "$tmp/installed.log")"
  fi
else
  echo "skip the installed-Catch2 case: this build downloaded Catch2"
fi
exit $((fails > 0))
