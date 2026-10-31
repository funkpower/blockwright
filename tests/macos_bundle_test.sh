#!/usr/bin/env bash
# The app bundle's Info.plist (BLOCKWRIGHT_MACOS_BUNDLE):
#   macos_bundle_test.sh <bundle dir> <version> <bundle id> <deployment target>
set -uo pipefail
app=$1 version=$2 id=$3 target=$4
plist=$app/Contents/Info.plist
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
get() { /usr/libexec/PlistBuddy -c "Print :$1" "$plist" 2>&1; }
# want <key path> <value>
want() {
  local v; v=$(get "$1")
  if [[ $v == "$2" ]]; then ok "$1 = $2"; else bad "$1: $v (want $2)"; fi
}

if out=$(plutil -lint "$plist" 2>&1); then ok "Info.plist is well formed"; else bad "$out"; fi
if [[ $(basename "$app") == Blockwright.app ]]; then ok "the bundle is Blockwright.app"; else bad "the bundle is $(basename "$app")"; fi
want CFBundleExecutable Blockwright
if [[ -x $app/Contents/MacOS/Blockwright ]]; then ok "the executable is in Contents/MacOS"; else bad "no $app/Contents/MacOS/Blockwright"; fi
want CFBundleIconFile blockwright.icns
if [[ $(head -c 4 "$app/Contents/Resources/blockwright.icns" 2>/dev/null) == icns ]]; then ok "the icon is in Contents/Resources"
else bad "no icon at $app/Contents/Resources/blockwright.icns"; fi
want CFBundleIdentifier "$id"
want CFBundleName Blockwright
want CFBundlePackageType APPL
want CFBundleShortVersionString "$version"
want CFBundleVersion "$version"
want LSMinimumSystemVersion "$target"
want NSHighResolutionCapable true
want CFBundleDocumentTypes:0:LSItemContentTypes:0 "$id.project"
want CFBundleDocumentTypes:0:LSHandlerRank Owner
want CFBundleDocumentTypes:1:LSItemContentTypes:0 "$id.ansi"
want CFBundleDocumentTypes:1:LSItemContentTypes:1 "$id.xbin"
want CFBundleDocumentTypes:2:LSItemContentTypes:0 com.apple.macbinary-archive
want CFBundleDocumentTypes:2:LSHandlerRank Alternate
want UTExportedTypeDeclarations:0:UTTypeIdentifier "$id.project"
want UTExportedTypeDeclarations:0:UTTypeTagSpecification:public.filename-extension:0 bwp
want UTImportedTypeDeclarations:0:UTTypeIdentifier "$id.ansi"
want UTImportedTypeDeclarations:0:UTTypeTagSpecification:public.filename-extension:0 ans
want UTImportedTypeDeclarations:1:UTTypeIdentifier "$id.xbin"
want UTImportedTypeDeclarations:1:UTTypeTagSpecification:public.filename-extension:0 xb
if [[ -z $(grep -E '@[A-Z_]+@|\$\{' "$plist") ]]; then ok "no placeholder left"; else bad "placeholders left: $(grep -E '@[A-Z_]+@|\$\{' "$plist")"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
