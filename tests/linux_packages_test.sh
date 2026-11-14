#!/usr/bin/env bash
# The Debian and Fedora packages and the manual pages are the version the tree is:
#   linux_packages_test.sh <source dir>
#
# The newest packaging/debian/changelog entry's upstream version, the spec's
# Version:, the manual pages' .TH lines and the metainfo's newest <release>
# are project(VERSION) in CMakeLists.txt, and the files the packages need are
# there: debian/rules runnable, debian/source/format 3.0 (quilt), and a %changelog entry in the
# spec for its Version-Release, and %license holds every notice. blockwright-cli.1 describes every command
# blockwright-cli --help lists. The AppImage's THIRD-PARTY.txt links Qt's source
# under download.qt.io/archive/qt/, which keeps every release. Each AUR
# package's .SRCINFO says what its PKGBUILD says: every field, by
# `makepkg --printsrcinfo` where makepkg runs (not as root), and otherwise the
# fields the PKGBUILD sets, read by bash.
set -uo pipefail
src=$1
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

version=$(sed -n 's/^project(blockwright VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt" | head -n 1)
[[ -n $version ]] || { echo "FAIL no project(VERSION) in CMakeLists.txt"; exit 1; }

deb=$src/packaging/debian
# blockwright (0.1.0-1) unstable; urgency=medium
top=$(head -n 1 "$deb/changelog")
if [[ $top =~ ^blockwright\ \(([0-9.]+)-([0-9]+)\)\  ]]; then
  if [[ ${BASH_REMATCH[1]} == "$version" ]]; then
    ok "debian/changelog: $version-${BASH_REMATCH[2]}"
  else
    bad "debian/changelog's newest entry is ${BASH_REMATCH[1]}, the tree is $version"
  fi
else
  bad "debian/changelog's first line isn't 'blockwright (<version>-<revision>) ...': $top"
fi
grep -q '^Source: blockwright$' "$deb/control" && ok "debian/control: Source: blockwright" ||
  bad "debian/control has no 'Source: blockwright'"
[[ -x $deb/rules ]] && ok "debian/rules is executable" || bad "debian/rules isn't executable"
[[ $(cat "$deb/source/format" 2>/dev/null) == "3.0 (quilt)" ]] && ok "debian/source/format: 3.0 (quilt)" ||
  bad "debian/source/format isn't '3.0 (quilt)'"

spec=$src/packaging/rpm/blockwright.spec
spec_version=$(sed -n 's/^Version:[[:space:]]*//p' "$spec")
release=$(sed -n 's/^Release:[[:space:]]*\([0-9]*\).*/\1/p' "$spec")
if [[ $spec_version == "$version" ]]; then
  ok "blockwright.spec: Version $version"
else
  bad "blockwright.spec's Version is '$spec_version', the tree is $version"
fi
# * Sat Oct 31 2026 funkpower - 0.1.0-1
entry=$(awk '/^%changelog/ { on = 1; next } on && /^\* / { print; exit }' "$spec")
if [[ $entry == *" - $spec_version-$release" ]]; then
  ok "blockwright.spec: %changelog starts at $spec_version-$release"
else
  bad "blockwright.spec's newest %changelog entry isn't $spec_version-$release: $entry"
fi

# %license survives tsflags=nodocs, %doc doesn't: NOTICE and every bundled
# licence CMakeLists.txt installs beside it are %license files too
lic=$(sed -n 's/^%license[[:space:]]*//p' "$spec" | tr -s ' \t' '\n')
missing=
for want in LICENSE NOTICE; do
  grep -qx "$want" <<<"$lic" || missing+=" $want"
done
for dir in $(sed -n 's|.*FILES third_party/\([^/]*\)/LICENSE.*|\1|p' "$src/CMakeLists.txt"); do
  if ! grep -qx "LICENSE.$dir" <<<"$lic"; then
    missing+=" LICENSE.$dir"
  elif ! grep -Eq "^cp( -p)? third_party/$dir/LICENSE LICENSE\.$dir$" "$spec"; then
    missing+=" LICENSE.$dir(not copied in %prep)"
  fi
done
# Unifont's two licence texts (OFL-1.1.txt and COPYING, its OFL and GPL)
for pair in OFL-1.1.txt:OFL COPYING:GPL; do
  f=${pair%%:*} name=LICENSE.unifont-${pair#*:}
  if ! grep -q "FILES third_party/unifont/.*$f" "$src/CMakeLists.txt"; then
    missing+=" (CMakeLists.txt installs no third_party/unifont/$f)"
  elif ! grep -qx "$name" <<<"$lic"; then
    missing+=" $name"
  elif ! grep -Eq "^cp( -p)? third_party/unifont/$f $name$" "$spec"; then
    missing+=" $name(not copied in %prep)"
  fi
done
[[ -z $missing ]] && ok "blockwright.spec: %license holds LICENSE, NOTICE and the bundled licences" ||
  bad "blockwright.spec's %license lacks:$missing"

# <release version="0.1.1" date="2026-11-14">, newest first
metainfo=$src/packaging/linux/online.pumpin.funkpower.blockwright.metainfo.xml
newest=$(sed -n 's/.*<release version="\([^"]*\)".*/\1/p' "$metainfo" | head -n 1)
if [[ $newest == "$version" ]]; then
  ok "metainfo: newest release $version"
else
  bad "the metainfo's newest <release> is '$newest', the tree is $version"
fi

for page in blockwright blockwright-cli; do
  th=$(grep -m 1 '^\.TH ' "$src/packaging/linux/$page.1")
  if [[ $th == *"\"Blockwright $version\""* ]]; then
    ok "$page.1 is Blockwright $version"
  else
    bad "$page.1's .TH isn't Blockwright $version: $th"
  fi
done
# parser.addPositionalArgument(... "command"), QStringLiteral("info, convert, ... or import-scene"));
commands=$(sed -n 's/.*addPositionalArgument(QStringLiteral("command"), QStringLiteral("\([^"]*\)").*/\1/p' "$src/cli/main.cpp")
commands=${commands//,/}
commands=${commands// or / }
count=$(wc -w <<<"$commands" | tr -d ' ')
if [[ $count -lt 10 ]]; then
  bad "no command list in cli/main.cpp's --help: '$commands'"
else
  missing=
  for cmd in $commands; do
    grep -Eq "^\.BI \"?$cmd[ \"]" "$src/packaging/linux/blockwright-cli.1" || missing+=" $cmd"
  done
  [[ -z $missing ]] && ok "blockwright-cli.1 describes all $count commands" ||
    bad "blockwright-cli.1 doesn't describe:$missing"
fi

# official_releases/qt/ keeps only the current minors, so a link there dies
# once the bundled Qt's minor is superseded
third=$src/packaging/linux/THIRD-PARTY.txt
qt_link='https://download.qt.io/archive/qt/@QT_MINOR@/@QT_VERSION@/submodules/'
if grep -q 'download\.qt\.io/official_releases' "$third"; then
  bad "THIRD-PARTY.txt links Qt's source under official_releases, which drops old minors: $(grep -n 'official_releases' "$third")"
elif grep -qF "$qt_link" "$third"; then
  ok "THIRD-PARTY.txt links Qt's source under download.qt.io/archive/qt/"
else
  bad "THIRD-PARTY.txt has no link to Qt's source ($qt_link)"
fi

# .SRCINFO is what the AUR shows and what AUR helpers resolve dependencies
# from, so a field the PKGBUILD changed without it goes out wrong
srcinfo_tmp=$(mktemp -d)
trap 'rm -rf "$srcinfo_tmp"' EXIT
makepkg_runs=
tab=$'\t' # BSD sed has no \t
command -v makepkg >/dev/null && [[ $(id -u) != 0 ]] && makepkg_runs=1
for aur in "$src"/packaging/aur/*/; do
  aur=${aur%/}
  name=aur/${aur##*/}
  if [[ ! -f $aur/PKGBUILD || ! -f $aur/.SRCINFO ]]; then
    bad "$name has no PKGBUILD or no .SRCINFO"
    continue
  fi
  # the PKGBUILD's fields as .SRCINFO writes them: "<key> = <value>" per element
  fields=$(cd "$srcinfo_tmp" && bash -c '
    set +u
    source "$1" || exit 1
    for key in pkgdesc pkgver pkgrel url arch license checkdepends makedepends depends optdepends provides conflicts source sha256sums; do
      declare -n v=$key
      for e in "${v[@]}"; do printf "%s = %s\n" "$key" "$e"; done
      unset -n v
    done' bash "$aur/PKGBUILD") || { bad "$name/PKGBUILD doesn't source"; continue; }
  have=$(sed -n "s/^$tab//p" "$aur/.SRCINFO" | grep -E '^(pkgdesc|pkgver|pkgrel|url|arch|license|checkdepends|makedepends|depends|optdepends|provides|conflicts|source|sha256sums) = ' | sort)
  if [[ $(sort <<<"$fields") == "$have" ]]; then
    ok "$name/.SRCINFO has its PKGBUILD's fields"
  else
    bad "$name/.SRCINFO and PKGBUILD differ (- .SRCINFO, + PKGBUILD; makepkg --printsrcinfo > .SRCINFO):
$(diff <(echo "$have") <(sort <<<"$fields") | grep '^[<>]' | sed 's/^</-/; s/^>/+/')"
  fi
  if [[ -n $makepkg_runs ]]; then
    rm -rf "$srcinfo_tmp/p" && mkdir "$srcinfo_tmp/p" && cp "$aur/PKGBUILD" "$srcinfo_tmp/p/"
    if ! printed=$(cd "$srcinfo_tmp/p" && makepkg --printsrcinfo 2>&1); then
      bad "$name: makepkg --printsrcinfo failed: $printed"
    elif [[ $printed == "$(cat "$aur/.SRCINFO")" ]]; then
      ok "$name/.SRCINFO is makepkg --printsrcinfo's"
    else
      bad "$name/.SRCINFO isn't makepkg --printsrcinfo's:
$(diff <(cat "$aur/.SRCINFO") <(echo "$printed"))"
    fi
  fi
done

exit $((fails > 0))
