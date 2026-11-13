#!/usr/bin/env bash
# scripts/package-macos refuses, before building anything, a Qt whose QtCore
# needs a newer macOS than the deployment target:
#   macos_package_qt_test.sh <source dir> <scratch dir>
set -uo pipefail
src=$1 dir=$2
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
rm -rf "$dir" && mkdir -p "$dir"
arch=$(uname -m)

# fakeqt <dir> <minos>: a Qt prefix package-macos accepts the shape of, whose
# QtCore is built for macOS <minos>
fakeqt() {
  mkdir -p "$1/bin" "$1/lib/cmake/Qt6" "$1/lib/QtCore.framework"
  printf '#!/bin/sh\nexit 1\n' > "$1/bin/macdeployqt" && chmod +x "$1/bin/macdeployqt"
  echo 'int qt_core_stub(void) { return 0; }' > "$dir/stub.c"
  cc -arch "$arch" -dynamiclib -mmacosx-version-min="$2" -o "$1/lib/QtCore.framework/QtCore" "$dir/stub.c" ||
    { echo "skip: cc can't build a $arch dylib for macOS $2"; exit 77; }
}
for tool in cc otool lipo; do
  command -v "$tool" >/dev/null || { echo "skip: no $tool"; exit 77; }
done
fakeqt "$dir/qt13" 13.0
fakeqt "$dir/qt12" 12.0

# run <name> <env...> -- <args...>: package-macos with PATH limited to the
# system's, so a Qt that passes stops at the missing cmake, not a build
run() {
  local name=$1; shift
  local envs=()
  while [[ $1 != -- ]]; do envs+=("$1"); shift; done; shift
  env -i HOME="$dir/home" PATH=/usr/bin:/bin ${envs[@]+"${envs[@]}"} bash "$src/scripts/package-macos" \
    --work "$dir/$name-work" --out "$dir/$name-out" "$@" > "$dir/$name.log" 2>&1
}

if run new -- --qt "$dir/qt13"; then bad "a Qt for macOS 13 was accepted"; else
  log=$(<"$dir/new.log")
  if [[ $log == *"needs macOS 13.0"*"deployment target is macOS 12.0"* ]]; then ok "a Qt for macOS 13 is refused, naming both versions"
  else bad "the refusal doesn't name both versions: $log"; fi
  if [[ $log == *RELEASING.md* && $log == *MACOSX_DEPLOYMENT_TARGET=13.0* ]]; then ok "the refusal names both fixes"
  else bad "the refusal doesn't name the fixes: $log"; fi
fi
if [[ ! -e $dir/new-work && ! -e $dir/new-out ]]; then ok "nothing is built or written before the refusal"
else bad "the refused run made its work or out directory"; fi

if run raised MACOSX_DEPLOYMENT_TARGET=13.0 -- --qt "$dir/qt13"; then bad "the raised run went through with no cmake"
elif grep -q 'cmake is not on PATH' "$dir/raised.log"; then ok "with the target raised to 13.0 the same Qt passes the check"
else bad "with the target raised: $(<"$dir/raised.log")"; fi

if run ok12 -- --qt "$dir/qt12"; then bad "the 12.0 run went through with no cmake"
elif grep -q 'cmake is not on PATH' "$dir/ok12.log"; then ok "a Qt for macOS 12.0 passes the check at the project's target"
else bad "a Qt for macOS 12.0: $(<"$dir/ok12.log")"; fi

if run lower MACOSX_DEPLOYMENT_TARGET=11.5 -- --qt "$dir/qt12" || ! grep -q 'needs macOS 12.0.*target is macOS 11.5' "$dir/lower.log"
then bad "a Qt for 12.0 with the target at 11.5: $(<"$dir/lower.log")"
else ok "a Qt for 12.0 is refused for a target of 11.5"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
