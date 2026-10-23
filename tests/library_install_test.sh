#!/usr/bin/env bash
# The bundled Library installs to <datadir>/blockwright/library/ and the
# installed CLI finds it from its own directory:
#   library_install_test.sh <build dir> <scratch dir> <full bindir> <full datadir> [<config>]
set -uo pipefail
build=$1 tmp=$2 bindir=$3 datadir=$4 config=${5:-}
rm -rf "$tmp" && mkdir -p "$tmp/user"
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
lib=$stage$datadir/blockwright/library
if [[ -f $lib/sky/small-star.ans && -f $lib/sky/small-star.json ]]; then
  ok "the bundled stamps install to share/blockwright/library"
else
  bad "no stamps in $lib"
fi

cli=$stage$bindir/blockwright-cli
out=$(env -u BLOCKWRIGHT_BUNDLED_LIBRARY BLOCKWRIGHT_USER_LIBRARY="$tmp/user" "$cli" library list 2>&1); st=$?
if [[ $st -eq 0 && $out == *$'sky\tSmall star\t5x3\t16\tstar,night\t'"$lib"/sky/small-star.ans* ]]; then
  ok "the installed CLI lists the installed bundled library"
else
  bad "library list (exit $st): $out"
fi
out=$("$cli" library check "$lib" 2>&1); st=$?
if [[ $st -eq 0 && $out == "1 stamp(s), 0 problem(s)" ]]; then ok "the installed library checks clean"; else bad "library check (exit $st): $out"; fi

exit $((fails > 0))
