#!/usr/bin/env bash
# packaging/qt-third-party writes each NAME=value into its @NAME@ as it is,
# sed's special characters included, and fails, writing nothing, when a
# placeholder is left over (a name with a digit too), a value holds an @, the
# template or the Qt version can't be read or sed fails. Writing nothing means
# no <out> or <out>.tmp where there was none, and an <out> already there left
# byte for byte as it was.
# Runs under a Mac's bash 3.2 too:
#   qt_third_party_test.sh <source dir> <work dir>
set -euo pipefail
src=${1:?usage: qt_third_party_test.sh <source dir> <work dir>}
work=${2:?usage: qt_third_party_test.sh <source dir> <work dir>}
tool=$src/packaging/qt-third-party
finished=0
trap 'rc=$?; if [[ $finished != 1 ]]; then echo "FAIL stopped early (rc $rc)"; exit 1; fi' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

mkdir -p "$work"
qt=$work/qt
mkdir -p "$qt/lib/cmake/Qt6"
printf 'set(PACKAGE_VERSION "6.9.1")\n' > "$qt/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"

# fills <case> <template text> <want> NAME=value...: writes exactly <want>
fills() {
  local c=$1 text=$2 want=$3
  shift 3
  local t=$work/$c.in o=$work/$c.txt
  printf '%s\n' "$text" > "$t"
  rm -f "$o" "$o.tmp"
  if ! "$tool" "$qt" "$t" "$o" "$@" >/dev/null 2>"$work/$c.err"; then
    bad "$c: failed: $(cat "$work/$c.err")"
  elif [[ $(cat "$o") != "$want" ]]; then
    bad "$c: wrote '$(cat "$o")', wants '$want'"
  else
    ok "$c: writes '$want'"
  fi
}

# What a failing run must leave as it was in an <out> already there
pre=$work/pre-existing
printf 'PRE\tnot the notice\001\n\n' > "$pre"

# fails <case> <qt dir> <template> NAME=value...: the run fails twice, first
# with no <out>, then over a copy of $pre, and writes nothing; sets rc to the
# second run's exit code and why to what went wrong, or to nothing
fails() {
  local c=$1 q=$2 t=$3 o=$work/$1.txt
  shift 3
  why=
  rm -f "$o" "$o.tmp"
  rc=0
  "$tool" "$q" "$t" "$o" "$@" >/dev/null 2>"$work/$c.err" || rc=$?
  if (( rc == 0 )); then
    why="exit 0, wrote '$(cat "$o" 2>/dev/null)'"
    return 0
  elif [[ -e $o ]]; then
    why="fails but writes $o"
    return 0
  elif [[ -e $o.tmp ]]; then
    why="fails but leaves $o.tmp"
    return 0
  fi
  cp "$pre" "$o"
  rc=0
  "$tool" "$q" "$t" "$o" "$@" >/dev/null 2>"$work/$c.pre.err" || rc=$?
  if (( rc == 0 )); then
    why="exit 0 over an existing $o, which now holds '$(cat "$o" 2>/dev/null || true)'"
  elif [[ ! -e $o ]]; then
    why="fails and removes the existing $o"
  elif ! cmp -s "$pre" "$o"; then
    why="fails and changes the existing $o to '$(cat "$o")'"
  elif [[ -e $o.tmp ]]; then
    why="fails over an existing $o but leaves $o.tmp"
  fi
  return 0
}

# refuses <case> <template text> NAME=value...: fails and writes nothing
refuses() {
  local c=$1 text=$2
  shift 2
  local t=$work/$c.in
  printf '%s\n' "$text" > "$t"
  fails "$c" "$qt" "$t" "$@"
  if [[ -n $why ]]; then
    bad "$c: $why"
  else
    ok "$c: fails and writes nothing ($(head -n 1 "$work/$c.err" | sed "s|$work|<work>|g"))"
  fi
}

# sed's delimiter, its & and \ and a / come through as they are, and so do
# a backreference, a shell variable and a command substitution
value='a|b&c\d/e\1 &&\\ $HOME `id` $(id) *?['
fills literal 'V=@V@ Qt @QT_VERSION@ (@QT_MINOR@)' "V=$value Qt 6.9.1 (6.9)" "V=$value"
# without the | the unescaped value would not stop sed, but & and \ would
# change what it writes
fills amp_backslash 'V=@V@' 'V=x&y\z\\w' 'V=x&y\z\\w'
fills digit_name 'Y=@Y2@ Z=@Z_3@' 'Y=two Z=three' Y2=two Z_3=three
fills empty_value '[@E@]' '[]' E=

refuses leftover 'X=@X@ Y=@Y@' X=1
refuses leftover_digit 'X=@X@ Y=@Y2@' X=1
refuses leftover_unknown_name '@QT_VERSION@ @NOT_GIVEN@'
refuses value_at 'A=@A@ B=@B@' 'A=@B@' B=b
refuses value_at_mail 'A=@A@' 'A=x@example.org'
refuses lower_name 'a=@a@' a=1
refuses line_break 'A=@A@' "A=one
two"

# No template to read, or a directory in its place: fails, says why and
# writes nothing
for c in missing_template template_dir; do
  t=$work/$c.in
  if [[ -d $t ]]; then rmdir "$t"; else rm -f "$t"; fi
  [[ $c == template_dir ]] && mkdir -p "$t"
  fails "$c" "$qt" "$t"
  if [[ -n $why ]]; then
    bad "$c: $why"
  elif ! grep -q "can't read the template $t\$" "$work/$c.err"; then
    bad "$c: fails without saying why: $(cat "$work/$c.err")"
  else
    ok "$c: fails and writes nothing ($(head -n 1 "$work/$c.err" | sed "s|$work|<work>|g"))"
  fi
done

# No Qt version to read: fails and writes nothing
mkdir -p "$work/no_qt"
printf 'Qt @QT_VERSION@\n' > "$work/no_qt_version.in"
fails no_qt_version "$work/no_qt" "$work/no_qt_version.in"
if [[ -n $why ]]; then
  bad "no_qt_version: $why"
else
  ok "no_qt_version: fails and writes nothing ($(head -n 1 "$work/no_qt_version.err" | sed "s|$work|<work>|g"))"
fi

# sed failing on the template, as on a full disk: fails with sed's exit code
# and writes nothing. The fake sed runs the real one for every other call.
fake=$work/fake-sed
mkdir -p "$fake"
printf '%s\n' '#!/bin/sh' 'for a in "$@"; do last=$a; done' \
  "[ \"\${last:-}\" = '$work/sed_fails.in' ] && exit 4" \
  "exec '$(command -v sed)' \"\$@\"" > "$fake/sed"
chmod +x "$fake/sed"
printf 'V=@V@ Qt @QT_VERSION@\n' > "$work/sed_fails.in"
PATH=$fake:$PATH fails sed_fails "$qt" "$work/sed_fails.in" V=1
if [[ -n $why ]]; then
  bad "sed_fails: $why"
elif (( rc != 4 )); then
  bad "sed_fails: exits $rc, wants sed's 4"
else
  ok "sed_fails: fails with sed's exit code 4 and writes nothing"
fi

finished=1
if (( fails > 0 )); then
  exit 1
fi
exit 0
