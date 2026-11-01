#!/usr/bin/env bash
# The AppImage runs, and carries Qt and every library but the system's:
#   appimage_test.sh <blockwright-<version>-x86_64.AppImage> <scratch dir> <tests/data>
#
# Runs the editor (offscreen, and under Wayland in a headless weston when
# weston is on PATH) and blockwright-cli, as --cli and under a
# blockwright-cli symlink: --version, info, render and library list. Then
# extracts the AppImage and checks every ELF file in it with ldd: none needs a
# library that isn't found, and none loads Qt, ICU, libzip, zstd or another
# library Qt brings from outside the AppImage. The libraries the system
# provides (glibc, OpenGL, X11, fontconfig and the like) are listed. Last, the
# notices: Qt's LGPL and SBOMs, ICU's licence, and one for every library in
# usr/lib, as bundled-libraries.txt names them.
#
# Without weston the Wayland run is skipped, or fails with
# BW_APPIMAGE_WAYLAND=require (scripts/package-appimage sets it).
set -uo pipefail
appimage=$1 tmp=$2 data=$3
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
appimage=$(cd "$(dirname "$appimage")" && pwd)/$(basename "$appimage")
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

[[ $(basename "$appimage") =~ ^blockwright-([0-9.]+)-x86_64\.AppImage$ ]] || { bad "$appimage isn't blockwright-<version>-x86_64.AppImage"; exit 1; }
version=${BASH_REMATCH[1]}
# A binfmt_misc handler for AppImages (AppImageLauncher's) runs every AppImage
# under its own path, so ARGV0 loses the name it was run under, and it talks
# on stderr. A handler registered on the host applies in its containers too,
# where /proc/sys/fs/binfmt_misc doesn't show it. It matches the magic bytes at
# offset 8, which the runtime never reads: the checks run a copy without them,
# and the AppImage itself has to run blockwright-cli --version.
orig=$appimage
appimage=$tmp/$(basename "$orig")
cp "$orig" "$appimage" && printf '\0\0\0' | dd of="$appimage" bs=1 seek=8 conv=notrunc status=none
# No FUSE needed (containers have none): the runtime extracts itself and runs.
export APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen LC_ALL=C.UTF-8
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share XDG_CACHE_HOME=$tmp/home/.cache
mkdir -p "$HOME"
cd "$tmp" || exit 1

check_output() {  # <label> <regex> <command...>
  local label=$1 re=$2 out
  shift 2
  if out=$("$@" 2>&1) && [[ $out =~ $re ]]; then ok "$label"; else bad "$label: $out"; fi
}
check_output "the AppImage itself runs --cli --version" $'(^|\n)blockwright-cli '"$version " "$orig" --cli --version
check_output "blockwright --version" "^blockwright $version " "$appimage" --version
check_output "--cli --version" "^blockwright-cli $version " "$appimage" --cli --version
ln -s "$appimage" "$tmp/blockwright-cli"
check_output "a blockwright-cli symlink runs blockwright-cli" "^blockwright-cli $version " "$tmp/blockwright-cli" --version
check_output "blockwright-cli info" "size: +30x17" "$tmp/blockwright-cli" info "$data/golden/tux.ans"
if "$tmp/blockwright-cli" render "$data/golden/tux.ans" "$tmp/tux.png" >"$tmp/render.log" 2>&1 &&
   [[ $(od -An -tx1 -N8 "$tmp/tux.png" | tr -d ' \n') == 89504e470d0a1a0a ]]; then
  ok "blockwright-cli render writes a PNG"
else
  bad "blockwright-cli render: $(cat "$tmp/render.log")"
fi
check_output "blockwright-cli library list finds the bundled library" $'stamp\t' "$tmp/blockwright-cli" library list
# The editor opens a piece and stays up until it is closed.
timeout 10 "$appimage" "$data/golden/tux.ans" >"$tmp/gui.log" 2>&1
rc=$?
[[ $rc == 124 ]] && ok "the editor opens a piece (offscreen)" || bad "the editor exited $rc: $(cat "$tmp/gui.log")"
# Under Wayland the platform plugin loads xdg-shell and a decoration plugin
# from the AppImage, or makes no window and exits.
if command -v weston >/dev/null; then
  mkdir -m 700 "$tmp/xdg"
  XDG_RUNTIME_DIR=$tmp/xdg weston --backend=headless-backend.so --socket=bw-wayland --idle-time=0 \
    >"$tmp/weston.log" 2>&1 &
  weston=$!
  for _ in $(seq 50); do [[ -S $tmp/xdg/bw-wayland ]] && break; sleep 0.2; done
  if [[ -S $tmp/xdg/bw-wayland ]]; then
    XDG_RUNTIME_DIR=$tmp/xdg WAYLAND_DISPLAY=bw-wayland QT_QPA_PLATFORM=wayland \
      timeout 10 "$appimage" "$data/golden/tux.ans" >"$tmp/wayland.log" 2>&1
    rc=$?
    if [[ $rc == 124 ]] && ! grep -q -E 'shell integration|No decoration plugins|Could not load' "$tmp/wayland.log"; then
      ok "the editor opens a piece under Wayland"
    else
      bad "the editor under Wayland exited $rc: $(cat "$tmp/wayland.log")"
    fi
  else
    bad "weston made no Wayland socket: $(cat "$tmp/weston.log")"
  fi
  kill "$weston" 2>/dev/null; wait "$weston" 2>/dev/null
elif [[ ${BW_APPIMAGE_WAYLAND:-} == require ]]; then
  bad "no weston on PATH to run the editor under Wayland"
else
  echo "skip the editor under Wayland: no weston on PATH"
fi

"$appimage" --appimage-extract >/dev/null 2>&1
root=$tmp/squashfs-root
if [[ ! -x $root/AppRun ]]; then
  bad "--appimage-extract left no squashfs-root/AppRun"
  exit 1
fi
missing= leaks=
declare -A system
while IFS= read -r -d '' f; do
  [[ $(head -c 4 "$f" | od -An -c | tr -d ' ') == 177ELF ]] || continue
  while read -r lib arrow path _; do
    [[ $arrow == "=>" ]] || continue
    if [[ $path == not ]]; then
      missing+=$'\n'"  ${f#"$root"/}: $lib"
    elif [[ $path != "$root"/* ]]; then
      if [[ $lib =~ ^lib(Qt6|icu|zip\.|zstd|double-conversion|pcre2-16|b2\.|md4c) ]]; then
        leaks+=$'\n'"  ${f#"$root"/}: $lib => $path"
      else
        system[$lib]=1
      fi
    fi
  done < <(ldd "$f" 2>/dev/null)
done < <(find "$root" -type f \( -name '*.so*' -o -perm -u+x \) -print0)
[[ -z $missing ]] && ok "every library is found" || bad "libraries not found:$missing"
[[ -z $leaks ]] && ok "Qt and its libraries load from the AppImage" || bad "loaded from the system:$leaks"
[[ -f $root/usr/lib/libQt6Core.so.6 ]] && ok "usr/lib/libQt6Core.so.6" || bad "no usr/lib/libQt6Core.so.6"
for p in platforms/libqxcb.so platforms/libqoffscreen.so platforms/libqwayland-generic.so \
         wayland-shell-integration/libxdg-shell.so wayland-decoration-client/libbradient.so; do
  [[ -f $root/usr/plugins/$p ]] && ok "usr/plugins/$p" || bad "no usr/plugins/$p"
done
echo "from the system: $(printf '%s\n' "${!system[@]}" | sort | tr '\n' ' ')"

doc=$root/usr/share/doc
for n in LICENSE NOTICE THIRD-PARTY.txt licenses/LGPL-3.0-only.txt licenses/ICU-73.txt; do
  [[ -s $doc/blockwright/$n ]] && ok "usr/share/doc/blockwright/$n" || bad "no usr/share/doc/blockwright/$n"
done
for m in qtbase qtsvg qtimageformats qtwayland; do
  sbom=$(ls "$doc/blockwright/qt-sbom/$m-"[0-9]*.spdx 2>/dev/null)
  [[ -n $sbom ]] && ok "usr/share/doc/blockwright/qt-sbom/${sbom##*/}" || bad "no usr/share/doc/blockwright/qt-sbom/$m-<version>.spdx"
done
if grep -q '@[A-Z_]*@' "$doc/blockwright/THIRD-PARTY.txt" 2>/dev/null; then
  bad "THIRD-PARTY.txt keeps a placeholder: $(grep '@[A-Z_]*@' "$doc/blockwright/THIRD-PARTY.txt")"
fi
uncovered=
for f in "$root"/usr/lib/*; do
  n=$(basename "$f")
  notice=$(awk -F'\t' -v n="$n" '$1 == n { print $3 }' "$doc/blockwright/bundled-libraries.txt" 2>/dev/null)
  [[ -n $notice && -s $doc/$notice ]] || uncovered+=" $n${notice:+ ($notice)}"
done
[[ -z $uncovered ]] && ok "every library in usr/lib has a notice" || bad "no notice for:$uncovered"
exit $((fails > 0))
