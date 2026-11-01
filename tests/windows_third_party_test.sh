#!/usr/bin/env bash
# packaging/windows/third-party fills the Windows Qt's version into
# THIRD-PARTY.txt, and fails, writing nothing, when it can't read one:
#   windows_third_party_test.sh <source dir> <work dir>
if [ -z "${BASH_VERSINFO:-}" ] || [ "${BASH_VERSINFO:-0}" -lt 4 ]; then
  echo "FAIL needs bash 4 or later"
  exit 1
fi
set -euo pipefail
finished=0
trap 'rc=$?; if [[ $finished != 1 ]]; then echo "FAIL stopped early (rc $rc)"; exit 1; fi' EXIT
src=${1:?usage: windows_third_party_test.sh <source dir> <work dir>}
work=${2:?usage: windows_third_party_test.sh <source dir> <work dir>}
tool=$src/packaging/windows/third-party
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# A Qt laid out as the official win64_llvm_mingw one: CR LF line ends, the
# version in Qt6ConfigVersionImpl.cmake, and a second PACKAGE_VERSION line
# that isn't a version
qt=$work/qt-6.9.1
mkdir -p "$qt/lib/cmake/Qt6"
printf 'include("${CMAKE_CURRENT_LIST_DIR}/Qt6ConfigVersionImpl.cmake")\r\n' > "$qt/lib/cmake/Qt6/Qt6ConfigVersion.cmake"
printf '# version file\r\nset(PACKAGE_VERSION "6.9.1")\r\n  set(PACKAGE_VERSION "${PACKAGE_VERSION} (${installedBits}bit)")\r\n' \
  > "$qt/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
out=$work/THIRD-PARTY.txt
rm -f "$out"
if "$tool" "$qt" "$out" >/dev/null; then
  if grep -q -- '- Qt 6\.9\.1 (Core' "$out" &&
     grep -q '^  https://download\.qt\.io/archive/qt/6\.9/6\.9\.1/submodules/$' "$out"; then
    ok "Qt 6.9.1 named and linked as archive/qt/6.9/6.9.1/submodules/"
  else
    bad "Qt 6.9.1 not filled in: $(grep -n 'Qt 6\|download\.qt\.io' "$out" | tr '\n' ' ')"
  fi
  if grep -q '@[A-Z_]*@' "$out"; then
    bad "placeholder left: $(grep -n '@[A-Z_]*@' "$out")"
  else
    ok "no placeholder left"
  fi
  if bash "$src/tests/packaging_qt_links_test.sh" "$src" >/dev/null; then
    ok "the template itself passes packaging_qt_links"
  else
    bad "the template fails packaging_qt_links"
  fi
else
  bad "third-party failed on a Qt 6.9.1"
fi

# No readable version: fails and writes nothing
for c in missing notversion; do
  q=$work/qt-$c
  mkdir -p "$q/lib/cmake/Qt6"
  rm -f "$q/lib/cmake/Qt6/"*
  [[ $c == notversion ]] && printf 'set(PACKAGE_VERSION "${X}")\r\n' > "$q/lib/cmake/Qt6/Qt6ConfigVersion.cmake"
  o=$work/THIRD-PARTY-$c.txt
  rm -f "$o"
  if "$tool" "$q" "$o" 2>/dev/null; then
    bad "$c: exit 0 without a Qt version"
  elif [[ -e $o || -e $o.tmp ]]; then
    bad "$c: wrote $o"
  else
    ok "$c: fails and writes nothing"
  fi
done

finished=1
if (( fails > 0 )); then
  exit 1
fi
exit 0
