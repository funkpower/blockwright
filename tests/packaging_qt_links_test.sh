#!/usr/bin/env bash
# Every THIRD-PARTY file under packaging/ links Qt's source where it stays,
# for the Qt the package bundles:
#   packaging_qt_links_test.sh <source dir>
#
# download.qt.io/official_releases/qt/ keeps only the current minors, so a
# link there dies once the bundled Qt's minor is superseded. The packaging
# scripts fill the bundled Qt's version in, so a file names it only through
# @QT_VERSION@ and @QT_MINOR@: each download.qt.io link, with or without a
# scheme and in any letter case, must be exactly
# https://download.qt.io/archive/qt/@QT_MINOR@/@QT_VERSION@/submodules/, and a
# file that names a fixed Qt version fails, since it would drift from the Qt
# the package carries: "Qt" or "Qt6" in any letter case, then any run of
# characters that are neither letters nor digits (spaces, a tab, a
# non-breaking space in UTF-8 or Latin-1), at most one word such as "version"
# with its own run, and an X.Y. A THIRD-PARTY* that is not a regular file (a
# directory or a symlink), and a symlink to a directory anywhere under
# packaging/, fail too, since the check would not read what they hold. Files
# are read as bytes and as text, whatever the locale and whatever bytes they
# hold. No network access.
#
# Any failure, including an error in this script, exits non-zero: the only
# way to exit 0 is to reach the end with nothing counted.
if [ -z "${BASH_VERSINFO:-}" ] || [ "${BASH_VERSINFO:-0}" -lt 4 ]; then
  echo "FAIL needs bash 4 or later (this is ${BASH_VERSION:-not bash})"
  exit 1
fi
set -euo pipefail
# bytes: a lone Latin-1 byte is then a character that is not a letter
export LC_ALL=C
finished=0
trap 'rc=$?; if [[ $finished != 1 ]]; then echo "FAIL stopped before checking every file (rc $rc)"; exit 1; fi' EXIT

src=${1:?usage: packaging_qt_links_test.sh <source dir>}
template='https://download.qt.io/archive/qt/@QT_MINOR@/@QT_VERSION@/submodules/'
fails=0
checked=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
# grep that tells no match (rc 1, empty output) from an error (rc 2 or more),
# which stops the check, and reads every file as text: on a file with a NUL
# byte grep would otherwise print only "binary file matches", on stderr
g() {
  local rc=0
  grep -a "$@" || rc=$?
  if (( rc > 1 )); then echo "FAIL grep $* failed (rc $rc)" >&2; exit 1; fi
  return 0
}

files=()
# Every THIRD-PARTY* whatever its type, so a directory or a symlink fails
# below instead of being skipped, and every symlink, since find does not go
# into a symlinked directory
list=$(cd "$src" && find packaging \( -name 'THIRD-PARTY*' -o -type l \) | LC_ALL=C sort)
while IFS= read -r f; do
  if [[ -z $f ]]; then
    continue
  elif [[ ${f##*/} == THIRD-PARTY* ]]; then
    files+=("$f")
  elif [[ -d $src/$f ]]; then
    bad "$f is a symlink to a directory, which the check does not go into"
  fi
done <<<"$list"
if (( ${#files[@]} == 0 )); then
  bad "no THIRD-PARTY file under packaging/"
fi

for f in "${files[@]}"; do
  checked=$((checked + 1))
  if [[ -L $src/$f ]]; then
    bad "$f is a symlink, not a regular file"
    continue
  fi
  if [[ ! -f $src/$f ]]; then
    bad "$f is not a regular file"
    continue
  fi
  if [[ ! -r $src/$f ]]; then
    bad "$f cannot be read"
    continue
  fi
  dead=$(g -ni 'official_releases' "$src/$f")
  if [[ -n $dead ]]; then
    bad "$f links under official_releases, which drops old minors: $(tr -s ' ' <<<"$dead")"
    continue
  fi
  fixed=$(g -niE '(^|[^[:alnum:]_])qt6?[^[:alnum:]]+([a-z]+[^[:alnum:]]+)?[0-9]+\.[0-9]+' "$src/$f")
  fixed=$(tr -s ' ' <<<"$fixed")
  if [[ -n $fixed ]]; then
    bad "$f names a fixed Qt version instead of @QT_VERSION@: $(tr '\n' ' ' <<<"$fixed")"
  fi
  links=$(g -oiE '([a-z][a-z0-9+.-]*://)?download\.qt\.io[^[:space:]]*' "$src/$f")
  n=0
  wrong=
  while IFS= read -r l; do
    [[ -n $l ]] || continue
    n=$((n + 1))
    [[ $l == "$template" ]] || wrong+=" $l"
  done <<<"$links"
  if [[ -n $wrong ]]; then
    bad "$f has a Qt source link other than $template:$wrong"
  elif [[ -z $fixed ]]; then
    if (( n > 0 )); then
      ok "$f links Qt's source under download.qt.io/archive/qt/ ($n)"
    else
      ok "$f has no Qt source link"
    fi
  fi
done

(( checked == ${#files[@]} )) || bad "checked $checked of ${#files[@]} files"
finished=1
if (( fails > 0 )); then
  exit 1
fi
exit 0
