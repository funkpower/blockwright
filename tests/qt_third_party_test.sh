#!/usr/bin/env bash
# packaging/qt-third-party writes each NAME=value into its @NAME@ as it is,
# sed's special characters included, and fails, writing nothing, when a
# placeholder is left over (a name with a digit too), a value holds an @ or
# the template can't be read.
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

# refuses <case> <template text> NAME=value...: fails and writes nothing
refuses() {
  local c=$1 text=$2
  shift 2
  local t=$work/$c.in o=$work/$c.txt
  printf '%s\n' "$text" > "$t"
  rm -f "$o" "$o.tmp"
  if "$tool" "$qt" "$t" "$o" "$@" >/dev/null 2>"$work/$c.err"; then
    bad "$c: exit 0, wrote '$(cat "$o" 2>/dev/null)'"
  elif [[ -e $o || -e $o.tmp ]]; then
    bad "$c: fails but writes $o"
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

# No template to read, or a directory in its place: fails and leaves neither
# <out> nor <out>.tmp behind
for c in missing_template template_dir; do
  t=$work/$c.in o=$work/$c.txt
  rm -f "$o" "$o.tmp"
  if [[ -d $t ]]; then rmdir "$t"; else rm -f "$t"; fi
  [[ $c == template_dir ]] && mkdir -p "$t"
  if "$tool" "$qt" "$t" "$o" >/dev/null 2>"$work/$c.err"; then
    bad "$c: exit 0"
  elif [[ -e $o || -e $o.tmp ]]; then
    bad "$c: fails but leaves$( [[ -e $o ]] && echo " $o")$( [[ -e $o.tmp ]] && echo " $o.tmp")"
  elif ! grep -q "can't read the template $t\$" "$work/$c.err"; then
    bad "$c: fails without saying why: $(cat "$work/$c.err")"
  else
    ok "$c: fails and writes nothing ($(head -n 1 "$work/$c.err" | sed "s|$work|<work>|g"))"
  fi
done

finished=1
if (( fails > 0 )); then
  exit 1
fi
exit 0
