#!/usr/bin/env bash
# app_guard_fixture_test.sh <cmake> <ctest> <generator> <c++ compiler> <scratch dir> <source dir>:
# holds app_tests_all_guarded to its job.
# - app_guard_check.cmake runs under `cmake -P`, which takes none of the
#   project's policies, so its first command sets them to the project's own
#   minimum version (CMake 3.x rejects IN_LIST without CMP0057).
# - The fixture project in tests/data/app-guard passes the guard as written,
#   and fails it for an app test binary declared after bw_add_app_guard_test(),
#   for one in a subdirectory and for one put in BW_APP_TEST_BINARIES after it.
# The fixture is configured for one named configuration and the nested ctest
# runs that one, so under a multi-config generator (where a test without -C is
# NOT_AVAILABLE) the guard still runs. A multi-config generator ignores
# CMAKE_BUILD_TYPE and takes its configurations from CMAKE_CONFIGURATION_TYPES,
# which the environment may set to a list without that one, so both are set.
set -uo pipefail
cmake=$1 ctest=$2 generator=$3 cxx=$4 tmp=$5 src=$6 config=Release
mkdir -p "$tmp"
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }

min=$(grep -oP '^cmake_minimum_required\(VERSION \K[0-9.]+' "$src/CMakeLists.txt")
first=$(grep -vE '^[[:space:]]*(#|$)' "$src/tests/app_guard_check.cmake" | head -n1)
if [[ $first == "cmake_minimum_required(VERSION $min)" ]]; then
  echo "ok   app_guard_check.cmake sets the policies of CMake $min"
else
  fail "app_guard_check.cmake starts with '$first', not cmake_minimum_required(VERSION $min)"
fi

guard() { # name late=ON|OFF: configures the fixture and prints its guard's output
  local build=$tmp/$1
  command rm -rf "$build"
  if ! "$cmake" -S "$src/tests/data/app-guard" -B "$build" -G "$generator" \
      -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_BUILD_TYPE="$config" -DCMAKE_CONFIGURATION_TYPES="$config" \
      -DLATE="$2" >"$tmp/$1.configure.log" 2>&1; then
    echo "configure failed (see $tmp/$1.configure.log)"; return 2
  fi
  "$ctest" --test-dir "$build" -C "$config" -R '^app_tests_all_guarded$' -V 2>&1
}

out=$(guard in-order OFF); st=$?
if [[ $st -eq 0 && $out == *"ok   guarded"* ]]; then
  echo "ok   the fixture as written passes the guard"
else
  fail "the fixture as written does not pass the guard:"; echo "$out" | sed 's/^/  /'
fi

out=$(guard late ON); st=$?
missing=()
for bin in late sub-late; do
  for want in "FAIL $bin is not run by app_tests_leave_profile_alone" "FAIL $bin runs under the user's own HOME"; do
    [[ $out == *"$want"* ]] || missing+=("$want")
  done
done
want="FAIL late-listed is not run by app_tests_leave_profile_alone"
[[ $out == *"$want"* ]] || missing+=("$want")
if [[ $st -ne 0 && ${#missing[@]} -eq 0 ]]; then
  echo "ok   the guard rejects a late app test, one in a subdirectory and one listed late"
else
  fail "the guard let a late, subdirectory or late-listed app test through (exit $st); missing:"
  printf '  %s\n' "${missing[@]}"; echo "$out" | sed 's/^/  | /'
fi
exit $((fails > 0))
