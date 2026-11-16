#!/usr/bin/env bash
# The installed Flatpak runs and carries what a Linux install carries:
#   flatpak_test.sh <version> <scratch dir> <tests/data>
#
# Wants online.pumpin.funkpower.blockwright installed in the user
# installation (FLATPAK_USER_DIR, if set, names it); scripts/package-flatpak
# --check installs its bundle and runs this. In the sandbox: blockwright and
# blockwright-cli --version, and blockwright-cli info, render and library
# list on a golden piece; the editor opens a piece offscreen and stays up.
# Then what the app carries: the bundled Library, the samples, the licence,
# NOTICE and the fonts' and icons' notices, libzip's and zstd's licences, the
# AppStream metadata (valid, with appstreamcli where it is installed) and the
# desktop file and icon exported under the app id; and the permissions are
# the manifest's.
set -uo pipefail
version=$1 tmp=$2 data=$3
app=online.pumpin.funkpower.blockwright
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0 nl=$'\n'
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# The pieces are copied into the scratch dir, which the runs are given
# access to, wherever it is (--filesystem=home covers only $HOME).
cp "$data/golden/tux.ans" "$tmp/"
run() { flatpak run --user --filesystem="$tmp" --env=QT_QPA_PLATFORM=offscreen "$@"; }
cli() { run --command=blockwright-cli "$app" "$@"; }
check_output() {  # <label> <regex> <command...>
  local label=$1 re=$2 out
  shift 2
  if out=$("$@" 2>&1) && [[ $out =~ $re ]]; then ok "$label"; else bad "$label: $out"; fi
}

location=$(flatpak --user info --show-location "$app" 2>/dev/null) || { bad "$app is not installed"; exit 1; }
check_output "blockwright --version" "(^|$nl)blockwright $version " run "$app" --version
check_output "blockwright-cli --version" "(^|$nl)blockwright-cli $version " cli --version
check_output "blockwright-cli info" "size: +30x17" cli info "$tmp/tux.ans"
if cli render "$tmp/tux.ans" "$tmp/tux.png" >"$tmp/render.log" 2>&1 &&
   [[ $(od -An -tx1 -N8 "$tmp/tux.png" | tr -d ' \n') == 89504e470d0a1a0a ]]; then
  ok "blockwright-cli render writes a PNG"
else
  bad "blockwright-cli render: $(cat "$tmp/render.log")"
fi
check_output "blockwright-cli library list finds the bundled library" $'stamp\t' cli library list
timeout -k 5 10 flatpak run --user --filesystem="$tmp" --env=QT_QPA_PLATFORM=offscreen "$app" "$tmp/tux.ans" >"$tmp/gui.log" 2>&1
rc=$?
[[ $rc == 124 ]] && ok "the editor opens a piece (offscreen)" || bad "the editor exited $rc: $(cat "$tmp/gui.log")"

files=$location/files
[[ -n $(ls "$files/share/blockwright/library" 2>/dev/null) ]] && ok "the bundled Library" || bad "no $files/share/blockwright/library"
[[ -n $(ls "$files/share/blockwright/samples" 2>/dev/null) ]] && ok "the samples" || bad "no $files/share/blockwright/samples"
for f in LICENSE NOTICE oldschool-pc-fonts/LICENSE tabler-icons/LICENSE unifont/OFL-1.1.txt noto-sans/OFL.txt; do
  [[ -s $files/share/doc/blockwright/$f ]] && ok "share/doc/blockwright/$f" || bad "no share/doc/blockwright/$f"
done
for m in libzip zstd; do
  [[ -n $(ls "$files/share/licenses/$app/$m" 2>/dev/null) ]] && ok "$m's licence" || bad "no share/licenses/$app/$m"
done

metainfo=$files/share/metainfo/$app.metainfo.xml
if [[ -f $metainfo ]] && grep -q "<launchable type=\"desktop-id\">$app.desktop</launchable>" "$metainfo"; then
  ok "the AppStream metadata launches $app.desktop"
else
  bad "no $metainfo launching $app.desktop"
fi
if command -v appstreamcli >/dev/null; then
  if out=$(appstreamcli validate --no-net --explain "$metainfo" 2>&1); then ok "appstreamcli validate"; else bad "appstreamcli validate: $out"; fi
else
  echo "skip appstreamcli validate: no appstreamcli on PATH"
fi
desktop=$location/export/share/applications/$app.desktop
if [[ -f $desktop ]] && grep -qx "Icon=$app" "$desktop" && grep -q "^Exec=.*flatpak run .*$app" "$desktop"; then
  ok "the desktop file is exported as $app.desktop"
else
  bad "no exported $desktop with Icon=$app and a flatpak run Exec"
fi
[[ -f $location/export/share/icons/hicolor/256x256/apps/$app.png ]] && ok "the icon is exported as $app.png" ||
  bad "no exported hicolor/256x256/apps/$app.png"

# The KDE runtime adds its own (kdeglobals read-only, the session bus names
# its integration talks to): each permission the manifest asks for is
# wanted among them, and no host file system, other devices or network.
perms=$(flatpak --user info --show-permissions "$app" 2>&1)
has() {  # <key> <item>: the item is in the key's ;-separated list
  sed -n "s/^$1=//p" <<<"$perms" | tr ';' '\n' | grep -qx -- "$2"
}
for want in sockets:wayland sockets:fallback-x11 shared:ipc devices:dri filesystems:home; do
  has "${want%%:*}" "${want#*:}" && ok "permission ${want/:/ }" || bad "no permission ${want/:/ } in: $perms"
done
for unwanted in shared:network sockets:x11 devices:all filesystems:host filesystems:host-os; do
  has "${unwanted%%:*}" "${unwanted#*:}" && bad "permission ${unwanted/:/ } in: $perms" || ok "no ${unwanted/:/ }"
done

((fails == 0))
