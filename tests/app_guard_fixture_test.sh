#!/usr/bin/env bash
# app_guard_fixture_test.sh <cmake> <ctest> <generator> <c++ compiler> <scratch dir> <source dir>:
# holds app_tests_all_guarded to its job.
# - app_guard_check.cmake runs under `cmake -P`, which takes none of the
#   project's policies, so its first command sets them to the project's own
#   minimum version (CMake 3.x rejects IN_LIST without CMP0057).
# - The fixture project in tests/data/app-guard passes the guard as written,
#   and fails it for an app test binary declared after bw_add_app_guard_test(),
#   for one in a subdirectory and for one put in BW_APP_TEST_BINARIES after it.
#   An X server binary passes when its test runs it through BW_APP_XCB_TEST_ENV
#   and fails with no test, even under the real xcb binary's name. The guard
#   reads the commands ctest runs: under CMP0158 OLD and NEW it fails one whose
#   test runs it by $<TARGET_FILE> with the wrapper only as its emulator, one
#   that also has a bare test, and, under NEW (CMake 3.29 on), one run by
#   target name with the wrapper only as its emulator, which passes under OLD.
#   It fails one a shell command string runs bare, by full or relative path
#   (not counting not-xcb's run against xcb) or by its bare name, one whose
#   path is an option's value and two whose name is (--exe=xcb, run as ./xcb
#   and found on PATH=.). It fails closed: tests that only mention xcb as a
#   whole name (backend=xcb, backend='xcb', /build/xcb/data, and a shell
#   assignment backend='xcb' before printf) fail it by design, while longer
#   names that contain xcb (/build/xcb-data/, libxcb.so, xcb_test.sh, and xcb
#   with each file-name character class on each side: Axcb, axcb, 2xcb, .xcb,
#   _xcb, -xcb, xcbA, xcba, xcb2, xcb., xcb_, xcb-) pass, and
#   so do tests that name xcb behind the wrapper. Before the tree is built it
#   fails one whose executable isn't there, since ctest lists no command for
#   that test.
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

guard() { # name [--unbuilt] [-Dvar=value...]: configures the fixture and prints its guard's output
  local name=$1 build=$tmp/$1 built=yes; shift
  [[ ${1:-} == --unbuilt ]] && { built=; shift; }
  command rm -rf "$build"
  if ! "$cmake" -S "$src/tests/data/app-guard" -B "$build" -G "$generator" \
      -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_BUILD_TYPE="$config" -DCMAKE_CONFIGURATION_TYPES="$config" \
      "$@" >"$tmp/$name.configure.log" 2>&1; then
    echo "configure failed (see $tmp/$name.configure.log)"; return 2
  fi
  # built, as the guard sees a tree ctest runs: ctest lists no command for a
  # test whose executable isn't there
  if [[ $built ]] && ! "$cmake" --build "$build" --config "$config" >"$tmp/$name.build.log" 2>&1; then
    echo "build failed (see $tmp/$name.build.log)"; return 2
  fi
  "$ctest" --test-dir "$build" -C "$config" -R '^app_tests_all_guarded$' -V 2>&1
}

for policy in OLD NEW; do
  out=$(guard in-order-$policy -DCMAKE_POLICY_DEFAULT_CMP0158=$policy); st=$?
  if [[ $st -eq 0 && $out == *"ok   guarded"* && $out == *"ok   xcb"* ]]; then
    echo "ok   the fixture as written passes the guard under CMP0158 $policy"
  else
    fail "the fixture as written does not pass the guard under CMP0158 $policy:"; echo "$out" | sed 's/^/  /'
  fi
done

out=$(guard late -DLATE=ON); st=$?
missing=()
for bin in late sub-late blockwright-app-xcb-tests; do
  for want in "FAIL $bin is not run by app_tests_leave_profile_alone" "FAIL $bin runs under the user's own HOME"; do
    [[ $out == *"$want"* ]] || missing+=("$want")
  done
done
want="FAIL late-listed is not run by app_tests_leave_profile_alone"
[[ $out == *"$want"* ]] || missing+=("$want")
if [[ $st -ne 0 && ${#missing[@]} -eq 0 ]]; then
  echo "ok   the guard rejects a late app test, one in a subdirectory, one listed late and an xcb one with no emulator"
else
  fail "the guard let a late, subdirectory, late-listed or unguarded xcb app test through (exit $st); missing:"
  printf '  %s\n' "${missing[@]}"; echo "$out" | sed 's/^/  | /'
fi
# CMP0158 is CMake 3.29's; an older one runs a target-name test under its
# emulator whatever the setting.
version=$("$cmake" --version | sed -n '1s/^cmake version //p')
policy_known=$([[ $(printf '%s\n' 3.29 "$version" | sort -V | head -n1) == 3.29 ]] && echo yes)
for policy in OLD NEW; do
  out=$(guard xcb-bad-$policy -DXCB_BAD=ON -DCMAKE_POLICY_DEFAULT_CMP0158=$policy); st=$?
  missing=()
  for want in "FAIL xcb-target-file is named outside xcb_test.sh in the test xcb-target-file" \
              "FAIL xcb-target-file runs under the user's own HOME" \
              "FAIL xcb-also-bare is named outside xcb_test.sh in the test xcb-also-bare-unwrapped" \
              "FAIL xcb-also-bare runs under the user's own HOME" "ok   xcb"$'\n'; do
    [[ $out == *"$want"* ]] || missing+=("$want")
  done
  if [[ $policy == NEW && $policy_known == yes ]]; then
    for want in "FAIL xcb-emulator is named outside xcb_test.sh in the test xcb-emulator" \
                "FAIL xcb-emulator runs under the user's own HOME"; do
      [[ $out == *"$want"* ]] || missing+=("$want")
    done
  else
    [[ $out == *"ok   xcb-emulator"* ]] || missing+=("ok   xcb-emulator")
  fi
  if [[ $st -ne 0 && ${#missing[@]} -eq 0 ]]; then
    echo "ok   under CMP0158 $policy the guard rejects each xcb binary a test runs without the wrapper (CMake $version)"
  else
    fail "under CMP0158 $policy the guard let an unwrapped xcb test through, or failed a wrapped one (exit $st); missing:"
    printf '  %s\n' "${missing[@]}"; echo "$out" | sed 's/^/  | /'
  fi
done

expect_fail() { # what out st want...: passes when the guard failed and printed each want
  local what=$1 out=$2 st=$3 missing=(); shift 3
  for want in "$@"; do [[ $out == *"$want"* ]] || missing+=("$want"); done
  if [[ $st -ne 0 && ${#missing[@]} -eq 0 ]]; then
    echo "ok   the guard rejects $what"
  else
    fail "the guard let $what through (exit $st); missing:"
    printf '  %s\n' "${missing[@]}"; echo "$out" | sed 's/^/  | /'
  fi
}
out=$(guard xcb-shell -DXCB_SHELL=ON); st=$?
expect_fail "an xcb binary a shell command string runs bare" "$out" $st \
  "FAIL xcb is named outside xcb_test.sh in the test xcb-shell-unwrapped" "FAIL xcb runs under the user's own HOME"
out=$(guard xcb-relative -DXCB_RELATIVE=ON); st=$?
expect_fail "an xcb binary a shell command string runs bare by relative path" "$out" $st \
  "FAIL xcb is named outside xcb_test.sh in the test xcb-relative" "FAIL xcb runs under the user's own HOME"
if [[ $out == *"in the test not-xcb"* ]]; then
  fail "the guard counts not-xcb's run as one of xcb:"; echo "$out" | sed 's/^/  | /'
else
  echo "ok   the guard doesn't count not-xcb's run as one of xcb"
fi
out=$(guard xcb-option -DXCB_OPTION=ON); st=$?
expect_fail "an xcb binary run by a path or a name in an option's value or by its bare name in a shell string" \
  "$out" $st "FAIL xcb is named outside xcb_test.sh in the test xcb-option" \
  "FAIL xcb is named outside xcb_test.sh in the test xcb-exe-prefix" \
  "FAIL xcb is named outside xcb_test.sh in the test xcb-bare-option" \
  "FAIL xcb is named outside xcb_test.sh in the test xcb-word"
out=$(guard xcb-mention -DXCB_MENTION=ON); st=$?
expect_fail "a test that only mentions xcb as a whole name, by design" "$out" $st \
  "FAIL xcb is named outside xcb_test.sh in the test mention-argument" \
  "FAIL xcb is named outside xcb_test.sh in the test mention-quoted" \
  "FAIL xcb is named outside xcb_test.sh in the test mention-path" \
  "FAIL xcb is named outside xcb_test.sh in the test harmless-quoted"
expect_pass() { # what out st: passes when the guard passed and xcb with it
  if [[ $3 -eq 0 && $2 == *"ok   xcb"$'\n'* ]]; then
    echo "ok   the guard passes $1"
  else
    fail "the guard fails $1 (exit $3):"; echo "$2" | sed 's/^/  | /'
  fi
}
out=$(guard xcb-substring -DXCB_SUBSTRING=ON); st=$?
expect_pass "tests that only name longer names containing xcb" "$out" $st
out=$(guard xcb-wrapped -DXCB_WRAPPED=ON); st=$?
expect_pass "tests that name xcb behind the wrapper" "$out" $st
out=$(guard xcb-unbuilt --unbuilt -DXCB_BARE=ON); st=$?
expect_fail "an unbuilt tree, where a bare test has no command to read" "$out" $st \
  "FAIL the test xcb-bare has no command: its executable isn't built"
exit $((fails > 0))
