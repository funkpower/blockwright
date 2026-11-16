#!/usr/bin/env bash
# The Flatpak manifest, checked without network or flatpak-builder:
#   flatpak_manifest_test.sh <repo root> [manifest]
#   flatpak_manifest_test.sh --self-test <repo root> <scratch dir>
#
# The manifest (default packaging/flatpak/online.pumpin.funkpower.blockwright.yml)
# names the app id the AppStream metadata carries; the KDE runtime and SDK
# at a 6.x branch whose Qt is at least the 6.7 minimum; blockwright as its
# command; the desktop file and icon packaging/linux installs, renamed to the
# app id; finish-args only from the allowed set, with Wayland, fallback X11,
# IPC, DRI and the home directory; every downloaded source over https and
# pinned by sha256, and no git source; libzip and zstd built as modules
# (CMakeLists.txt finds both by their CMake configs), libzip at least the
# version CMakeLists.txt asks for; Blockwright last, built from the tree two
# levels up (the repo root) without the tests. scripts/package-flatpak parses
# (bash -n) and builds this manifest.
#
# --self-test breaks a copy of the manifest in each of those ways and wants
# each copy to fail with its message, so the checks can't pass vacuously.
#
# POSIX tools only (sed, grep, awk): the FreeBSD job runs it too.
set -uo pipefail

app_id=online.pumpin.funkpower.blockwright

lint() {  # <root> <manifest>
  local root=$1 m=$2 fails=0 v
  ok() { echo "ok   $1"; }
  bad() { echo "FAIL $1"; fails=$((fails + 1)); }
  top() { sed -n "s/^$1: *//p" "$m" | sed "s/^['\"]//; s/['\"]\$//" | head -n 1; }
  [[ -f $m ]] || { echo "FAIL no manifest $m"; return 1; }

  v=$(top app-id)
  if [[ $v == "$app_id" ]] && grep -q "<id>$app_id</id>" "$root/packaging/linux/$app_id.metainfo.xml"; then
    ok "app-id $v is the AppStream id"
  else
    bad "app-id is '$v', not $app_id, the id in packaging/linux/$app_id.metainfo.xml"
  fi
  [[ $(top runtime) == org.kde.Platform ]] && ok "runtime org.kde.Platform" || bad "runtime is '$(top runtime)', not org.kde.Platform"
  [[ $(top sdk) == org.kde.Sdk ]] && ok "sdk org.kde.Sdk" || bad "sdk is '$(top sdk)', not org.kde.Sdk"
  v=$(top runtime-version)
  if [[ $v =~ ^6\.([0-9]+)$ ]] && ((BASH_REMATCH[1] >= 7)); then
    ok "runtime-version $v carries Qt 6.7 or later"
  else
    bad "runtime-version '$v' is no KDE 6.x branch at or above 6.7 (Qt 6.7 is the minimum)"
  fi
  [[ $(top command) == blockwright ]] && ok "command blockwright" || bad "command is '$(top command)', not blockwright"

  # The exported desktop file and icon must start with the app id: the
  # manifest renames the ones the CMake install puts in.
  local desktop=$root/packaging/linux/blockwright.desktop
  v=$(top rename-desktop-file)
  [[ $v == blockwright.desktop && -f $desktop ]] && ok "rename-desktop-file blockwright.desktop" ||
    bad "rename-desktop-file is '$v', not blockwright.desktop (packaging/linux)"
  v=$(top rename-icon)
  if [[ -n $v ]] && grep -qx "Icon=$v" "$desktop"; then ok "rename-icon $v is the desktop file's icon"; else
    bad "rename-icon is '$v', not the Icon= of packaging/linux/blockwright.desktop"; fi

  # finish-args: the list under the key, comments skipped
  local args
  args=$(awk '/^finish-args:/{on=1; next} on && /^[^ #]/{on=0} on && /^ *- /{sub(/^ *- */, ""); print}' "$m")
  local allowed=" --socket=wayland --socket=fallback-x11 --share=ipc --device=dri --filesystem=home "
  local a
  for a in $args; do
    [[ $allowed == *" $a "* ]] || bad "finish-arg $a is not one of:$allowed"
  done
  for a in $allowed; do
    grep -qx -- "$a" <<<"$args" && ok "finish-arg $a" || bad "finish-arg $a is missing"
  done

  # Sources: every url https with a 64-hex sha256 in the same source, no git
  local srcs
  srcs=$(awk '
    function flush() { if (n) print type "|" url "|" sha "|" path; n = 0; type = url = sha = path = "" }
    /^ *- type:/ { flush(); n = 1; type = $NF; next }
    n && /^ *url:/ { url = $2 }
    n && /^ *sha256:/ { sha = $2 }
    n && /^ *path:/ { path = $2 }
    n && /^ *- name:/ { flush() }
    END { flush() }' "$m")
  local type url sha path nsrc=0
  while IFS='|' read -r type url sha path; do
    [[ -n $type ]] || continue
    nsrc=$((nsrc + 1))
    case $type in
      archive|file)
        if [[ $url == https://* && $sha =~ ^[0-9a-f]{64}$ ]]; then ok "$type $url pinned by sha256"; else
          bad "$type source '$url' is not https with a sha256 ('$sha')"; fi ;;
      dir) [[ $path == ../.. ]] && ok "dir source ../.. (the repo root)" || bad "dir source path '$path', not ../.." ;;
      *) bad "source type $type: only archive, file and dir (pinned or the tree itself)" ;;
    esac
  done <<<"$srcs"
  ((nsrc)) || bad "no sources found"

  # Modules, in order: zstd and libzip, then blockwright last
  local mods
  mods=$(sed -n 's/^  - name: *//p' "$m" | tr '\n' ' ')
  [[ $mods == *zstd* && $mods == *libzip* ]] && ok "zstd and libzip are built" || bad "modules '$mods' don't build zstd and libzip"
  [[ $mods == *"blockwright " && $mods != *"blockwright "?* ]] && ok "blockwright is the last module" ||
    bad "blockwright is not the last module ('$mods')"
  local want have
  want=$(sed -n 's/^find_package(libzip \([0-9.]*\) .*/\1/p' "$root/CMakeLists.txt")
  have=$(grep -o 'libzip-[0-9.]*[0-9]\.tar' "$m" | head -n 1 | sed 's/^libzip-//; s/\.tar$//')
  if [[ -n $want && -n $have ]] && [[ $(printf '%s\n%s\n' "$want" "$have" | sort -t. -k1,1n -k2,2n -k3,3n | head -n 1) == "$want" ]]; then
    ok "libzip $have is at least $want"
  else
    bad "libzip '$have' is older than CMakeLists.txt's minimum '$want'"
  fi
  # The blockwright module: everything after its name
  local bw
  bw=$(awk '/^  - name: blockwright$/{on=1} on' "$m")
  grep -q -- '-DBUILD_TESTING=OFF' <<<"$bw" && ok "blockwright builds without the tests" ||
    bad "the blockwright module doesn't configure -DBUILD_TESTING=OFF (the build has no network for Catch2)"
  grep -q 'buildsystem: cmake-ninja' <<<"$bw" && ok "blockwright builds with cmake-ninja" || bad "the blockwright module isn't cmake-ninja"
  grep -q '^project(blockwright ' "$root/CMakeLists.txt" && ok "../.. from packaging/flatpak is the repo root" ||
    bad "$root/CMakeLists.txt is not Blockwright's"

  bash -n "$root/scripts/package-flatpak" && grep -q "packaging/flatpak/\$app_id.yml" "$root/scripts/package-flatpak" &&
    ok "scripts/package-flatpak parses and builds the manifest" || bad "scripts/package-flatpak doesn't parse or doesn't build the manifest"
  ((fails == 0))
}

if [[ ${1-} == --self-test ]]; then
  root=$2 tmp=$3
  m=$root/packaging/flatpak/$app_id.yml
  rm -rf "$tmp" && mkdir -p "$tmp"
  fails=0
  case_() {  # <name> <expected FAIL text> <sed program>
    local name=$1 want=$2 out
    sed "$3" "$m" > "$tmp/$name.yml"
    if cmp -s "$m" "$tmp/$name.yml"; then echo "FAIL $name: the edit changed nothing"; fails=$((fails + 1)); return; fi
    if out=$(lint "$root" "$tmp/$name.yml"); then
      echo "FAIL $name: the broken manifest passed"; fails=$((fails + 1))
    elif grep -qF -- "FAIL $want" <<<"$out"; then
      echo "ok   $name"
    else
      echo "FAIL $name: no 'FAIL $want' in:"; echo "$out" | grep '^FAIL'; fails=$((fails + 1))
    fi
  }
  case_ app_id "app-id is 'org.example.Blockwright'" 's/^app-id: .*/app-id: org.example.Blockwright/'
  case_ old_runtime "runtime-version '6.6'" "s/^runtime-version: .*/runtime-version: '6.6'/"
  case_ gnome_runtime "runtime is 'org.gnome.Platform'" 's/^runtime: .*/runtime: org.gnome.Platform/'
  case_ x11 "finish-arg --socket=x11 is not one of" 's/--socket=fallback-x11/--socket=x11/'
  case_ host "finish-arg --filesystem=host is not one of" 's/--filesystem=home/--filesystem=host/'
  case_ network "finish-arg --share=network is not one of" 's/^  - --share=ipc$/  - --share=network/'
  case_ no_dri "finish-arg --device=dri is missing" '/^  - --device=dri$/d'
  case_ http "archive source 'http://" 's|url: https://github.com/nih-at|url: http://github.com/nih-at|'
  case_ no_sha "archive source 'https://github.com/nih-at" '/^ *sha256: c4006/d'
  case_ git "source type git" 's/^      - type: dir$/      - type: git/'
  case_ old_libzip "libzip '1.6.0' is older" 's/libzip-1\.[0-9.]*\.tar/libzip-1.6.0.tar/'
  case_ tests "the blockwright module doesn't configure -DBUILD_TESTING=OFF" 's/-DBUILD_TESTING=OFF/-DBUILD_TESTING=ON/'
  case_ icon "rename-icon is 'bw'" 's/^rename-icon: .*/rename-icon: bw/'
  case_ path "dir source path '..'" 's|^        path: \.\./\.\.$|        path: ..|'
  ((fails == 0)) && exit 0 || exit 1
fi

root=$1
lint "$root" "${2:-$root/packaging/flatpak/$app_id.yml}"
