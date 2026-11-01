#!/usr/bin/env bash
# packaging/macos/third-party puts Qt's notice (with the version, frameworks
# and plugins of the Qt it is given), the LGPL, Qt's SBOMs and the libzip and
# zstd licences into an app's Contents/Resources/doc, and fails, adding
# nothing, when one of them is missing or a Qt file belongs to no known
# module, or when filling in its notice fails (and then leaves nothing in
# TMPDIR either). Runs on fake Qt prefixes and app bundles, so it needs no Mac:
#   macos_third_party_test.sh <source dir> <work dir>          all cases, checked
#   macos_third_party_test.sh <source dir> <work dir> <case>   one case's exit code
set -euo pipefail
src=${1:?usage: macos_third_party_test.sh <source dir> <work dir> [case]}
work=${2:?usage: macos_third_party_test.sh <source dir> <work dir> [case]}
only=${3:-}
tool=$src/packaging/macos/third-party
cases="ok pdf no_version no_sbom unknown_framework unknown_plugin no_deps_licence no_deps_stamp no_license leftover_placeholder"

# <dir>/{qt,deps,Blockwright.app} as Qt 6.9.1, scripts/macos-deps and
# macdeployqt leave them, then changed for the case
fixture() {
  local d=$1 c=$2 m f
  rm -rf "$d"
  mkdir -p "$d/qt/lib/cmake/Qt6" "$d/qt/sbom" "$d/deps/licenses" "$d/tmp"
  printf 'include("${CMAKE_CURRENT_LIST_DIR}/Qt6ConfigVersionImpl.cmake")\n' > "$d/qt/lib/cmake/Qt6/Qt6ConfigVersion.cmake"
  printf '# version file\nset(PACKAGE_VERSION "6.9.1")\n  set(PACKAGE_VERSION "${PACKAGE_VERSION} (${installedBits}bit)")\n' \
    > "$d/qt/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
  for m in qtbase qtsvg qtimageformats qtpdf qtdeclarative; do
    printf 'SPDXVersion: SPDX-2.3\nDocumentName: %s\n' "$m" > "$d/qt/sbom/$m-6.9.1.spdx"
    printf 'SPDXVersion: SPDX-2.3\nDocumentName: %s source\n' "$m" > "$d/qt/sbom/$m-6.9.1.source.spdx"
  done
  echo 'libzip licence' > "$d/deps/licenses/libzip.txt"
  echo 'zstd licence' > "$d/deps/licenses/zstd.txt"
  echo 'zstd 1.5.7 libzip 1.11.4 12.0 arm64' > "$d/deps/.macos-deps"
  local app=$d/Blockwright.app
  mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/doc" "$app/Contents/PlugIns"
  echo 'GPL v3' > "$app/Contents/Resources/doc/LICENSE"
  echo 'notice' > "$app/Contents/Resources/doc/NOTICE"
  for f in QtCore QtGui QtWidgets QtConcurrent QtSvg; do
    mkdir -p "$app/Contents/Frameworks/$f.framework/Versions/A"
    echo "$f" > "$app/Contents/Frameworks/$f.framework/Versions/A/$f"
  done
  for f in platforms/libqcocoa styles/libqmacstyle imageformats/libqgif imageformats/libqjpeg \
           imageformats/libqwebp imageformats/libqtiff imageformats/libqsvg iconengines/libqsvgicon; do
    mkdir -p "$app/Contents/PlugIns/$(dirname "$f")"
    echo "$f" > "$app/Contents/PlugIns/$f.dylib"
  done
  case $c in
    ok) ;;
    pdf)
      mkdir -p "$app/Contents/Frameworks/QtPdf.framework/Versions/A"
      echo 'libqpdf' > "$app/Contents/PlugIns/imageformats/libqpdf.dylib" ;;
    no_version) rm "$d/qt/lib/cmake/Qt6/"* ;;
    no_sbom) rm "$d/qt/sbom/qtimageformats-6.9.1.spdx" ;;
    unknown_framework) mkdir -p "$app/Contents/Frameworks/QtQuick.framework" ;;
    unknown_plugin) echo x > "$app/Contents/PlugIns/imageformats/libqfoo.dylib" ;;
    no_deps_licence) rm "$d/deps/licenses/zstd.txt" ;;
    no_deps_stamp) rm "$d/deps/.macos-deps" ;;
    no_license) rm "$app/Contents/Resources/doc/LICENSE" ;;
    leftover_placeholder)
      # A copy of the scripts whose template has a placeholder nothing fills
      # in, so the run fails in packaging/qt-third-party, after its temporary
      # directory exists
      mkdir -p "$d/src/packaging/macos" "$d/src/packaging/windows/licenses"
      cp "$src/packaging/qt-third-party" "$d/src/packaging/"
      cp "$src/packaging/macos/third-party" "$src/packaging/macos/THIRD-PARTY.txt" "$d/src/packaging/macos/"
      cp "$src/packaging/windows/licenses/LGPL-3.0-only.txt" "$d/src/packaging/windows/licenses/"
      echo '  @NOT_FILLED_IN@' >> "$d/src/packaging/macos/THIRD-PARTY.txt" ;;
    *) echo "unknown case $c" >&2; exit 2 ;;
  esac
}

run() {
  local t=$tool
  [[ -d $1/src ]] && t=$1/src/packaging/macos/third-party
  TMPDIR=$1/tmp "$t" "$1/qt" "$1/deps" "$1/Blockwright.app"
}
# tmp_empty <case dir> <what>: the run left nothing in its TMPDIR
tmp_empty() {
  if [[ -n $(ls -A "$1/tmp") ]]; then bad "$2 but leaves $(ls -A "$1/tmp" | tr '\n' ' ')in TMPDIR"; else ok "$2 and leaves TMPDIR empty"; fi
}

if [[ -n $only ]]; then
  fixture "$work/$only" "$only"
  run "$work/$only"
  exit $?
fi

finished=0
trap 'rc=$?; if [[ $finished != 1 ]]; then echo "FAIL stopped early (rc $rc)"; exit 1; fi' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
# has <file> <grep -E pattern> <what>
has() { if grep -qE -- "$2" "$1"; then ok "$3"; else bad "$3: no /$2/ in $1"; fi; }

for c in $cases; do
  # a path with a space in it, as an app under ~/Library/Application Support would have
  d="$work/case $c"
  fixture "$d" "$c"
  doc=$d/Blockwright.app/Contents/Resources/doc
  if [[ $c == ok || $c == pdf ]]; then
    if ! run "$d" >/dev/null; then
      bad "$c: third-party failed"
      continue
    fi
    t=$doc/THIRD-PARTY.txt
    has "$t" '^- Qt 6\.9\.1, The Qt Company' "$c: Qt 6.9.1 named"
    has "$t" '^  https://download\.qt\.io/archive/qt/6\.9/6\.9\.1/submodules/$' "$c: source linked as archive/qt/6.9/6.9.1/submodules/"
    if [[ $c == pdf ]]; then
      has "$t" '^    frameworks in Contents/Frameworks: QtConcurrent, QtCore, QtGui, QtPdf, QtSvg, QtWidgets$' "$c: frameworks listed"
      want="qtbase-6.9.1.spdx qtimageformats-6.9.1.spdx qtpdf-6.9.1.spdx qtsvg-6.9.1.spdx"
    else
      has "$t" '^    frameworks in Contents/Frameworks: QtConcurrent, QtCore, QtGui, QtSvg, QtWidgets$' "$c: frameworks listed"
      want="qtbase-6.9.1.spdx qtimageformats-6.9.1.spdx qtsvg-6.9.1.spdx"
    fi
    has "$t" '^    plugins in Contents/PlugIns: iconengines, imageformats, platforms, styles$' "$c: plugin directories listed"
    has "$t" '^- libzip 1\.11\.4, ' "$c: libzip version from scripts/macos-deps"
    has "$t" '^  1\.5\.7, Meta Platforms' "$c: zstd version from scripts/macos-deps"
    if grep -q '@[A-Z][A-Z0-9_]*@' "$t"; then bad "$c: placeholder left: $(grep -n '@[A-Z][A-Z0-9_]*@' "$t")"; else ok "$c: no placeholder left"; fi
    tmp_empty "$d" "$c: succeeds"
    if cmp -s "$doc/LGPL-3.0-only.txt" "$src/packaging/windows/licenses/LGPL-3.0-only.txt" &&
       cmp -s "$doc/libzip.txt" "$d/deps/licenses/libzip.txt" && cmp -s "$doc/zstd.txt" "$d/deps/licenses/zstd.txt"; then
      ok "$c: LGPL, libzip and zstd licences copied"
    else
      bad "$c: LGPL, libzip or zstd licence missing or different"
    fi
    got=$(cd "$doc/qt-sbom" 2>/dev/null && ls | LC_ALL=C sort | tr '\n' ' ')
    if [[ $got == "$want " ]] && cmp -s "$doc/qt-sbom/qtbase-6.9.1.spdx" "$d/qt/sbom/qtbase-6.9.1.spdx"; then
      ok "$c: qt-sbom holds $want"
    else
      bad "$c: qt-sbom holds '$got', wants '$want'"
    fi
    got=$(cd "$doc" && ls | LC_ALL=C sort | tr '\n' ' ')
    if [[ $got == "LGPL-3.0-only.txt LICENSE NOTICE THIRD-PARTY.txt libzip.txt qt-sbom zstd.txt " ]] &&
       [[ $(cat "$doc/LICENSE") == 'GPL v3' && $(cat "$doc/NOTICE") == notice ]]; then
      ok "$c: doc holds the install's LICENSE and NOTICE and the notices, nothing else"
    else
      bad "$c: doc holds '$got'"
    fi
    # Run again, as after a second macdeployqt: the same files, no stale SBOM
    echo stale > "$doc/qt-sbom/qtstale-6.9.1.spdx"
    if run "$d" >/dev/null && [[ ! -e $doc/qt-sbom/qtstale-6.9.1.spdx ]]; then
      ok "$c: a second run replaces qt-sbom"
    else
      bad "$c: a second run failed or left a stale SBOM"
    fi
    tmp_empty "$d" "$c: a second run succeeds"
  else
    if run "$d" >/dev/null 2>"$d/err"; then
      bad "$c: exit 0"
      continue
    fi
    want="LICENSE NOTICE "
    [[ $c == no_license ]] && want="NOTICE "
    got=$(cd "$doc" && ls -A | LC_ALL=C sort | tr '\n' ' ')
    if [[ $got != "$want" ]]; then
      bad "$c: fails but leaves '$got' in doc"
    else
      ok "$c: fails and adds nothing to doc ($(head -n 1 "$d/err" | sed "s|$d|<case>|g"))"
    fi
    tmp_empty "$d" "$c: fails"
  fi
done

finished=1
if (( fails > 0 )); then
  exit 1
fi
exit 0
