#!/usr/bin/env bash
# blockwright --version, --help and usage errors, with no display: gui_version_test.sh <blockwright>
# BW_NOTICES_DIR names where --version says the notices are (default share/doc/blockwright/);
# BW_LOGO is branding/logo.txt, which --version prints under the version line.
# BW_UNIFONT_CREDIT is the Unifont credit's first line a BLOCKWRIGHT_SYSTEM_UNIFONT
# build prints (default the bundled 18.0.01's).
set -uo pipefail
bin=$1
notices_dir=${BW_NOTICES_DIR:-share/doc/blockwright/}
unifont_credit=${BW_UNIFONT_CREDIT:-'Includes GNU Unifont 18.0.01, (c) 1998-2026 Roman Czyborra, Paul Hardy et al.,'}
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# No display at all: --version must not need one.
out=$(env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM= "$bin" --version 2>&1); st=$?
first=$(head -n1 <<<"$out")
if [[ $st -eq 0 && $first =~ ^blockwright\ 0\.1\.0\ \([0-9a-f]{7,}(-dirty)?\)$|^blockwright\ 0\.1\.0\ \(unknown\)$ ]]; then
  ok "--version: $first"
else
  bad "--version (exit $st): $out"
fi
if grep -q 'IBM VGA 8x16 and IBM PC BIOS 8x8 fonts, (c) 2016-2020 VileR,' <<<"$out" && grep -q 'CC BY-SA 4.0, https://int10h.org/oldschool-pc-fonts/' <<<"$out" && grep -qF "notices in $notices_dir" <<<"$out"; then
  ok "--version credits the font"
else
  bad "--version has no font credit: $out"
fi
if grep -qxF "$unifont_credit" <<<"$out" && grep -q 'SIL OFL 1.1, https://unifoundry.com/unifont/' <<<"$out"; then
  ok "--version credits Unifont"
else
  bad "--version has no Unifont credit: $out"
fi
if grep -q 'Includes icons from Tabler Icons, MIT licence, https://tabler.io/icons;' <<<"$out"; then
  ok "--version credits the icons"
else
  bad "--version has no icon credit: $out"
fi
if grep -q '^Includes Noto Sans, (c) 2022 The Noto Project Authors, SIL OFL 1.1,$' <<<"$out" \
   && grep -q '^https://github.com/notofonts/latin-greek-cyrillic; notices in ' <<<"$out"; then
  ok "--version credits Noto Sans"
else
  bad "--version has no Noto Sans credit: $out"
fi
logo=$(tr -d '\r' <"${BW_LOGO:?BW_LOGO is unset}")
if [[ $(tr -d '\r' <<<"$out") == *"$logo"* ]]; then ok "--version shows the logo"; else bad "--version has no logo: $out"; fi
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$bin" --help 2>&1); st=$?
if [[ $st -eq 0 ]] && grep -q 'Usage: blockwright' <<<"$out"; then ok "--help"; else bad "--help (exit $st): $out"; fi
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$bin" --bogus 2>&1); st=$?
if [[ $st -eq 2 ]] && grep -q 'unknown option' <<<"$out"; then ok "unknown option"; else bad "unknown option (exit $st): $out"; fi
out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$bin" a.ans b.ans 2>&1); st=$?
if [[ $st -eq 2 ]] && grep -q 'one file at a time' <<<"$out"; then ok "two files"; else bad "two files (exit $st): $out"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
