#!/usr/bin/env bash
# The Debian and Fedora packages and the manual pages are the version the tree is:
#   linux_packages_test.sh <source dir>
#
# The newest packaging/debian/changelog entry's upstream version, the spec's
# Version: and the manual pages' .TH lines are project(VERSION) in
# CMakeLists.txt, and the files the packages need are there: debian/rules
# runnable, debian/source/format 3.0 (quilt), and a %changelog entry in the
# spec for its Version-Release, and %license holds every notice. blockwright-cli.1 describes every command
# blockwright-cli --help lists.
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
entry=$(sed -n '/^%changelog/,${/^\* /{p;q}}' "$spec")
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
[[ -z $missing ]] && ok "blockwright.spec: %license holds LICENSE, NOTICE and the bundled licences" ||
  bad "blockwright.spec's %license lacks:$missing"

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
if [[ $(wc -w <<<"$commands") -lt 10 ]]; then
  bad "no command list in cli/main.cpp's --help: '$commands'"
else
  missing=
  for cmd in $commands; do
    grep -Eq "^\.BI \"?$cmd[ \"]" "$src/packaging/linux/blockwright-cli.1" || missing+=" $cmd"
  done
  [[ -z $missing ]] && ok "blockwright-cli.1 describes all $(wc -w <<<"$commands") commands" ||
    bad "blockwright-cli.1 doesn't describe:$missing"
fi

exit $((fails > 0))
