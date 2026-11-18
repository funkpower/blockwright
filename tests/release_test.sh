#!/usr/bin/env bash
# scripts/release makes the same tarball of a commit every time, of its
# tracked files only, with the commit written into packaging/tarball-commit:
#   release_test.sh <scripts/release> <source dir> <scratch dir>
#
# Runs it on a throwaway repository holding this tree's .gitattributes and
# packaging/tarball-commit as committed, under a git configuration of its own.
set -uo pipefail
release=$1 src=$2 tmp=$3
rm -rf "$tmp" && mkdir -p "$tmp"
tmp=$(cd "$tmp" && pwd)
fails=0
# whether the permission bits of $1 are $2 (octal), with POSIX find: stat's
# options differ between systems
mode_is() { [[ -n $(find "$1" -prune -perm "$2") ]]; }
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
command -v git >/dev/null || { echo "skip: no git"; exit 77; }
command -v python3 >/dev/null || { echo "skip: no python3"; exit 77; }

export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=$tmp/gitconfig
printf '[user]\n\tname = test\n\temail = test@example.invalid\n[init]\n\tdefaultBranch = main\n' > "$GIT_CONFIG_GLOBAL"
repo=$tmp/repo
mkdir -p "$repo/packaging" "$repo/sub"
cp "$src/.gitattributes" "$repo/"
# as committed: a tree made by git archive (this one, maybe) has the commit
# written into it
printf '%s\n' '$Format:%h$' > "$repo/packaging/tarball-commit"
printf 'project(blockwright VERSION 1.2.3 LANGUAGES CXX)\n' > "$repo/CMakeLists.txt"
mkdir -p "$repo/packaging/aur/blockwright"
printf 'pkgname=blockwright\npkgver=0.0.1\npkgrel=4\nsource=("x.tar.gz")\nsha256sums=(\x27%s\x27)\n' \
  0000000000000000000000000000000000000000000000000000000000000000 > "$repo/packaging/aur/blockwright/PKGBUILD"
mkdir -p "$repo/packaging/freebsd"
printf 'PORTNAME=\tblockwright\nDISTVERSION=\t0.0.1\n' > "$repo/packaging/freebsd/Makefile"
printf 'descr\n' > "$repo/packaging/freebsd/pkg-descr"
printf 'bin/blockwright\n' > "$repo/packaging/freebsd/pkg-plist"
printf 'TIMESTAMP = 0\n' > "$repo/packaging/freebsd/distinfo"
pin=0123456789abcdef0123456789abcdef01234567
printf '%s\n' "$pin" > "$repo/packaging/docs-commit"
printf 'tracked\n' > "$repo/sub/file.txt"
printf '#!/bin/sh\n' > "$repo/sub/run.sh"; chmod 755 "$repo/sub/run.sh"
git -C "$repo" init -q && git -C "$repo" add -A &&
  GIT_COMMITTER_DATE='2026-01-02T03:04:05Z' GIT_AUTHOR_DATE='2026-01-02T03:04:05Z' git -C "$repo" commit -qm one ||
  { echo "FAIL could not make the repository"; exit 1; }
git -C "$repo" tag v1.2.3
short=$(git -C "$repo" rev-parse --short=7 HEAD)
# a change and a file that were never committed
printf 'changed\n' > "$repo/sub/file.txt"
printf 'untracked\n' > "$repo/sub/untracked.txt"

if bash "$release" --no-help-bundle --repo "$repo" --out "$tmp/a" 1.2.3 >"$tmp/a.log" 2>&1; then
  ok "the release of v1.2.3"
else
  bad "the release of v1.2.3: $(cat "$tmp/a.log")"; exit 1
fi
tarball=$tmp/a/blockwright-1.2.3.tar.gz
list=$(tar -tzf "$tarball" | LC_ALL=C sort | tr '\n' ' ')
want='blockwright-1.2.3/ blockwright-1.2.3/.gitattributes blockwright-1.2.3/CMakeLists.txt blockwright-1.2.3/packaging/ blockwright-1.2.3/packaging/aur/ blockwright-1.2.3/packaging/aur/blockwright/ blockwright-1.2.3/packaging/aur/blockwright/PKGBUILD blockwright-1.2.3/packaging/docs-commit blockwright-1.2.3/packaging/freebsd/ blockwright-1.2.3/packaging/freebsd/Makefile blockwright-1.2.3/packaging/freebsd/distinfo blockwright-1.2.3/packaging/freebsd/pkg-descr blockwright-1.2.3/packaging/freebsd/pkg-plist blockwright-1.2.3/packaging/tarball-commit blockwright-1.2.3/sub/ blockwright-1.2.3/sub/file.txt blockwright-1.2.3/sub/run.sh '
if [[ $list == "$want" ]]; then ok "the committed files only, under blockwright-1.2.3/"; else bad "the tarball holds: $list"; fi
mkdir -p "$tmp/x" && tar -xzf "$tarball" -C "$tmp/x"
if [[ $(cat "$tmp/x/blockwright-1.2.3/sub/file.txt") == tracked ]]; then
  ok "a file as committed, not as changed since"
else
  bad "sub/file.txt holds the uncommitted change"
fi
if [[ $(cat "$tmp/x/blockwright-1.2.3/packaging/tarball-commit") == "$short" ]]; then
  ok "packaging/tarball-commit holds the commit, $short"
else
  bad "packaging/tarball-commit holds '$(cat "$tmp/x/blockwright-1.2.3/packaging/tarball-commit")', not $short"
fi
if [[ -x $tmp/x/blockwright-1.2.3/sub/run.sh ]] && mode_is "$tmp/x/blockwright-1.2.3/sub/file.txt" 644; then
  ok "file modes 755 and 644"
else
  bad "file modes: $(ls -l "$tmp/x/blockwright-1.2.3/sub/run.sh" "$tmp/x/blockwright-1.2.3/sub/file.txt")"
fi
if [[ $(od -An -tx1 -j4 -N4 "$tarball" | tr -d ' \n') == 00000000 ]]; then
  ok "gzip records no time"
else
  bad "gzip header time: $(od -An -tx1 -j4 -N4 "$tarball")"
fi
if (cd "$tmp/a" && sha256sum -c --quiet blockwright-1.2.3.tar.gz.sha256); then
  ok "the .sha256 matches"
else
  bad "the .sha256 doesn't match"
fi

pkgbuild=$tmp/a/aur/blockwright/PKGBUILD
sum=$(cut -d' ' -f1 "$tmp/a/blockwright-1.2.3.tar.gz.sha256")
if [[ -f $pkgbuild ]] && grep -qx 'pkgver=1.2.3' "$pkgbuild" && grep -qx 'pkgrel=1' "$pkgbuild" &&
   grep -qx "sha256sums=('$sum')" "$pkgbuild" && grep -qx 'source=("x.tar.gz")' "$pkgbuild"; then
  ok "the AUR PKGBUILD names this version and the tarball's sum"
else
  bad "the AUR PKGBUILD: $(cat "$pkgbuild" 2>&1)"
fi

port=$tmp/a/freebsd/blockwright
size=$(wc -c < "$tmp/a/blockwright-1.2.3.tar.gz" | tr -d ' ')
want_distinfo="TIMESTAMP = $(git -C "$repo" show -s --format=%ct v1.2.3)
SHA256 (blockwright-1.2.3.tar.gz) = $sum
SIZE (blockwright-1.2.3.tar.gz) = $size"
if [[ $(printf 'PORTNAME=\tblockwright\nDISTVERSION=\t1.2.3') == "$(cat "$port/Makefile" 2>&1)" ]] &&
   [[ $(cat "$port/distinfo" 2>&1) == "$want_distinfo" ]] &&
   [[ $(cat "$port/pkg-descr" "$port/pkg-plist" 2>&1) == $'descr\nbin/blockwright' ]] &&
   [[ $(LC_ALL=C ls "$port") == $'Makefile\ndistinfo\npkg-descr\npkg-plist' ]]; then
  ok "the FreeBSD port names this version, and its distinfo the tarball's sum and size"
else
  bad "the FreeBSD port: $(ls -A "$port" 2>&1; cat "$port/Makefile" "$port/distinfo" 2>&1)"
fi

# the .SRCINFO comes from makepkg where it runs; where makepkg fails, as it
# does for root even with --printsrcinfo, the release still succeeds, without
# a .SRCINFO and with a note (stand-in makepkg on PATH, so the host's doesn't
# matter)
fake=$tmp/fakebin
mkdir -p "$fake/ok" "$fake/root"
printf '#!/bin/sh\n[ "$1" = --printsrcinfo ] || exit 1\necho "pkgbase = blockwright"\n' > "$fake/ok/makepkg"
printf '#!/bin/sh\necho "==> ERROR: Running makepkg as root is not allowed as it can cause permanent," >&2\nexit 10\n' > "$fake/root/makepkg"
chmod 755 "$fake/ok/makepkg" "$fake/root/makepkg"
if PATH=$fake/ok:$PATH bash "$release" --no-help-bundle --repo "$repo" --out "$tmp/s" 1.2.3 >"$tmp/s.log" 2>&1 &&
   [[ $(cat "$tmp/s/aur/blockwright/.SRCINFO" 2>/dev/null) == 'pkgbase = blockwright' ]] &&
   [[ -z $(find "$tmp/s/aur/blockwright" -name '.SRCINFO.*') ]]; then
  ok "the .SRCINFO is makepkg --printsrcinfo's"
else
  bad "the .SRCINFO with a working makepkg: $(cat "$tmp/s.log"; ls -A "$tmp/s/aur/blockwright" 2>&1)"
fi
# a stale .SRCINFO from an earlier run must not survive a failed makepkg
if PATH=$fake/root:$PATH bash "$release" --no-help-bundle --repo "$repo" --out "$tmp/s" 1.2.3 >"$tmp/r.log" 2>&1 &&
   [[ -f $tmp/s/aur/blockwright/PKGBUILD ]] &&
   [[ $(ls -A "$tmp/s/aur/blockwright") == PKGBUILD ]] &&
   grep -q 'makepkg --printsrcinfo failed with 10: ==> ERROR: Running makepkg as root' "$tmp/r.log"; then
  ok "makepkg refusing (as root) leaves the PKGBUILD alone, with a note"
else
  bad "a refusing makepkg: exit or files wrong: $(cat "$tmp/r.log"; ls -A "$tmp/s/aur/blockwright" 2>&1)"
fi

# the same commit by hash, later, under a git configuration that would change
# modes and abbreviations
sleep 1
printf '[tar]\n\tumask = 0077\n[core]\n\tabbrev = 12\n' >> "$GIT_CONFIG_GLOBAL"
if bash "$release" --no-help-bundle --repo "$repo" --out "$tmp/b" 1.2.3 "$(git -C "$repo" rev-parse HEAD)" >"$tmp/b.log" 2>&1 &&
   cmp -s "$tarball" "$tmp/b/blockwright-1.2.3.tar.gz"; then
  ok "the same bytes again, by hash and under another git configuration"
else
  bad "a second release differs: $(cat "$tmp/b.log")"
fi

if bash "$release" --no-help-bundle --repo "$repo" --out "$tmp/c" 1.2.4 HEAD >"$tmp/c.log" 2>&1; then
  bad "a version the tree isn't was released"
elif grep -q 'is version 1.2.3' "$tmp/c.log" && [[ ! -e $tmp/c/blockwright-1.2.4.tar.gz ]]; then
  ok "a version the tree isn't is refused"
else
  bad "the wrong version: $(cat "$tmp/c.log")"
fi
if bash "$release" --no-help-bundle --repo "$repo" --out "$tmp/d" 1.2.5 >"$tmp/d.log" 2>&1; then
  bad "a version with no tag was released"
else
  ok "no tag v1.2.5, no release"
fi

# The help bundle: --help-bundle or --no-help-bundle, one of them
if bash "$release" --repo "$repo" --out "$tmp/n" 1.2.3 >"$tmp/n.log" 2>&1; then
  bad "a release with neither --help-bundle nor --no-help-bundle"
elif grep -q 'pass --help-bundle DIR' "$tmp/n.log" && [[ ! -e $tmp/n/blockwright-1.2.3.tar.gz ]]; then
  ok "neither --help-bundle nor --no-help-bundle is refused"
else
  bad "neither flag: $(cat "$tmp/n.log")"
fi
help=$tmp/help
mkdir -p "$help/tutorials" "$help/samples/x"
bundle_json() { # <docsCommit> <checkedWith> <contents>
  printf '{"format": 1, "docsCommit": "%s", "checkedWith": "%s", "contents": %s}\n' "$1" "$2" "$3" > "$help/bundle.json"
}
good_contents='[{"title": "Tutorials", "page": "tutorials/index.html", "pages": []}]'
bundle_json "${pin:0:7}" "1.2.3 ($short)" "$good_contents"
printf '<h1>T</h1>\n' > "$help/tutorials/index.html"
printf 'ans\n' > "$help/samples/x/a.ans"
printf 'tutorials/index.html\n' > "$help/.blockwright-docs-manifest"
if bash "$release" --help-bundle "$help" --no-help-bundle --repo "$repo" --out "$tmp/n" 1.2.3 >"$tmp/n.log" 2>&1; then
  bad "a release with both --help-bundle and --no-help-bundle"
else
  ok "both --help-bundle and --no-help-bundle are refused"
fi
if bash "$release" --help-bundle "$help" --repo "$repo" --out "$tmp/h" 1.2.3 >"$tmp/h.log" 2>&1; then
  ok "a release with the help bundle"
else
  bad "a release with the help bundle: $(cat "$tmp/h.log")"
fi
htar=$tmp/h/blockwright-1.2.3.tar.gz
hlist=$(tar -tzf "$htar" 2>/dev/null | LC_ALL=C sort | tr '\n' ' ')
want_h=$(printf '%s\n' $want blockwright-1.2.3/help/bundle.json blockwright-1.2.3/help/samples/x/a.ans \
  blockwright-1.2.3/help/tutorials/index.html | LC_ALL=C sort | tr '\n' ' ')
if [[ $hlist == "$want_h" ]]; then
  ok "the bundle's files under blockwright-1.2.3/help/, beside the tracked files, without the docs build's manifest"
else
  bad "the tarball with the help bundle holds: $hlist"
fi
rm -rf "$tmp/hx" && mkdir -p "$tmp/hx" && tar -xzf "$htar" -C "$tmp/hx" 2>/dev/null
if cmp -s "$help/bundle.json" "$tmp/hx/blockwright-1.2.3/help/bundle.json" &&
   cmp -s "$help/samples/x/a.ans" "$tmp/hx/blockwright-1.2.3/help/samples/x/a.ans" &&
   mode_is "$tmp/hx/blockwright-1.2.3/help/bundle.json" 644; then
  ok "the bundle's files as they are, mode 644"
else
  bad "the bundle's files in the tarball differ: $(ls -lR "$tmp/hx/blockwright-1.2.3/help" 2>&1)"
fi
hentry=$(python3 -I -c 'import sys, tarfile
m = tarfile.open(sys.argv[1]).getmember("blockwright-1.2.3/help/bundle.json")
print(m.uid, m.gid, m.mtime, oct(m.mode))' "$htar" 2>&1)
if [[ $hentry == "0 0 $(git -C "$repo" show -s --format=%ct v1.2.3) 0o644" ]]; then
  ok "the bundle's files owned by 0, with the commit's time"
else
  bad "the bundle's owner, time and mode: $hentry"
fi
sleep 1
touch "$help/bundle.json" "$help/tutorials/index.html"
chmod 600 "$help/samples/x/a.ans"
if bash "$release" --help-bundle "$help" --repo "$repo" --out "$tmp/h2" 1.2.3 >"$tmp/h2.log" 2>&1 &&
   cmp -s "$htar" "$tmp/h2/blockwright-1.2.3.tar.gz"; then
  ok "the same bytes again with the bundle, its files touched and their modes changed"
else
  bad "a second release with the bundle differs: $(cat "$tmp/h2.log")"
fi
chmod 644 "$help/samples/x/a.ans"
# refused: a bundle of another docs commit, checked with another Blockwright,
# with no contents, without bundle.json, or holding a symlink; a commit that
# pins no docs commit
refused_bundle() { # <name> <wanted message>
  if bash "$release" --help-bundle "$help" --repo "$repo" --out "$tmp/r" 1.2.3 >"$tmp/r.log" 2>&1; then
    bad "$1 was released"
  elif grep -q "$2" "$tmp/r.log" && [[ ! -e $tmp/r/blockwright-1.2.3.tar.gz ]]; then
    ok "$1 is refused"
  else
    bad "$1: $(cat "$tmp/r.log")"
  fi
}
bundle_json abcdef0 "1.2.3 ($short)" "$good_contents"
refused_bundle "a bundle of another docs commit" "names docs commit 'abcdef0', not $pin"
bundle_json "${pin:0:7}" "1.2.2 ($short)" "$good_contents"
refused_bundle "a bundle checked with another Blockwright" "not this release, '1.2.3 ($short)'"
bundle_json "${pin:0:7}" "1.2.3 ($short)" "[]"
refused_bundle "a bundle with no contents" "has no contents"
bundle_json "${pin:0:7}" "1.2.3 ($short)" "{}"
refused_bundle "a bundle whose contents aren't a list" "has no contents"
bundle_json "${pin:0:7}" "1.2.3 ($short)" "$good_contents"
ln -s /etc/passwd "$help/samples/x/link.ans"
refused_bundle "a bundle holding a symlink" "neither a file nor a directory"
rm "$help/samples/x/link.ans"
mv "$help/bundle.json" "$help/bundle.json.away"
refused_bundle "a directory without bundle.json" "no bundle.json there"
mv "$help/bundle.json.away" "$help/bundle.json"
git -C "$repo" rm -q packaging/docs-commit && git -C "$repo" commit -qm "no pin" && git -C "$repo" tag v1.2.3-nopin
if bash "$release" --help-bundle "$help" --repo "$repo" --out "$tmp/r" 1.2.3 v1.2.3-nopin >"$tmp/r.log" 2>&1; then
  bad "a commit that pins no docs commit was released with a bundle"
elif grep -q "pins no docs commit" "$tmp/r.log"; then
  ok "a commit that pins no docs commit is refused with a bundle"
else
  bad "no pin: $(cat "$tmp/r.log")"
fi
exit $((fails > 0))
