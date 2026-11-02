#!/usr/bin/env bash
# A Linux install carries the desktop entry, the icons, the AppStream
# metadata and the manual pages, where the desktop and man find them:
#   linux_desktop_test.sh <build dir> <scratch dir> <full bindir> <full datadir> [<config>]
#
# Installs the build with DESTDIR=<scratch>/stage and checks the files, that
# the entry's Exec and Icon name the installed program and icon, and, where
# desktop-file-validate and appstreamcli are installed, that both files pass
# them.
set -uo pipefail
build=$1 tmp=$2 bindir=$3 datadir=$4 config=${5:-}
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

stage=$tmp/stage
args=(--install "$build")
[[ -n $config ]] && args+=(--config "$config")
if ! DESTDIR=$stage cmake "${args[@]}" >"$tmp/install.log" 2>&1; then
  bad "cmake --install: $(cat "$tmp/install.log")"
  exit 1
fi
share=$stage$datadir
desktop=$share/applications/blockwright.desktop
metainfo=$share/metainfo/online.pumpin.funkpower.blockwright.metainfo.xml

if [[ -f $desktop ]]; then
  ok "share/applications/blockwright.desktop"
  if grep -qx 'Exec=blockwright %f' "$desktop" && [[ -x $stage$bindir/blockwright ]]; then
    ok "Exec runs the installed blockwright with a file"
  else
    bad "Exec isn't 'blockwright %f', or $bindir/blockwright isn't installed"
  fi
  if grep -qx 'Icon=blockwright' "$desktop"; then ok "Icon=blockwright"; else bad "Icon isn't blockwright"; fi
else
  bad "no $desktop"
fi
for size in 16 32 48 128 256 512; do
  icon=$share/icons/hicolor/${size}x$size/apps/blockwright.png
  if [[ ! -f $icon ]]; then
    bad "no $icon"
  elif [[ $(od -An -tx1 -j16 -N8 "$icon" | tr -d ' \n') != $(printf '%08x%08x' "$size" "$size") ]]; then
    bad "$icon isn't ${size}x$size"
  else
    ok "hicolor/${size}x$size/apps/blockwright.png"
  fi
done
for page in blockwright blockwright-cli; do
  [[ -f $share/man/man1/$page.1 ]] && ok "share/man/man1/$page.1" || bad "no $share/man/man1/$page.1"
done
if [[ -f $metainfo ]] && grep -q '<launchable type="desktop-id">blockwright.desktop</launchable>' "$metainfo"; then
  ok "the metainfo names blockwright.desktop"
else
  bad "no $metainfo, or it doesn't launch blockwright.desktop"
fi

if command -v desktop-file-validate >/dev/null; then
  if out=$(desktop-file-validate "$desktop" 2>&1) && [[ -z $out ]]; then
    ok "desktop-file-validate"
  else
    bad "desktop-file-validate: $out"
  fi
else
  echo "skip desktop-file-validate: not installed"
fi
if command -v appstreamcli >/dev/null; then
  if out=$(appstreamcli validate --no-net "$metainfo" 2>&1); then
    ok "appstreamcli validate"
  else
    bad "appstreamcli validate: $out"
  fi
else
  echo "skip appstreamcli validate: not installed"
fi
exit $((fails > 0))
