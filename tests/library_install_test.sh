#!/usr/bin/env bash
# The bundled Library installs to <datadir>/blockwright/library/ and the
# installed CLI finds it from its own directory:
#   library_install_test.sh <build dir> <scratch dir> <full bindir> <full datadir> [<config> [<source library>]]
# With the source library, every stamp in it must be installed.
set -uo pipefail
build=$1 tmp=$2 bindir=$3 datadir=$4 config=${5:-} src=${6:-}
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
if [[ $st -eq 0 && $out == *$'stamp\tsky\tSmall star\t3x3\t13\t-\tstar,night\t'"$lib"/sky/small-star.ans* ]]; then
  ok "the installed CLI lists the installed bundled library"
else
  bad "library list (exit $st): $out"
fi
n=1 m=0
if [[ -n $src ]]; then
  n=$(cd "$src" && find . -name '*.ans' | wc -l)
  m=$(cd "$src" && find . -name '*.style.json' | wc -l)
  n=$((n)) m=$((m))  # BSD wc pads the counts with spaces
  missing=$(cd "$src" && find . -type f \( -name '*.ans' -o -name '*.json' \) | while read -r f; do cmp -s "$f" "$lib/$f" || echo "$f"; done)
  if [[ $n -gt 1 && -z $missing ]]; then ok "all $n bundled stamps install"; else bad "not installed as in the source: $missing"; fi
fi
out=$("$cli" library check "$lib" 2>&1); st=$?
if [[ $st -eq 0 && $out == "$n stamp$( ((n == 1)) || echo s), $m text style$( ((m == 1)) || echo s), 0 problems" ]]; then ok "the installed library checks clean"; else bad "library check (exit $st): $out"; fi

# Run through a symlink, as the dev install's ~/.local/bin/blockwright-cli-dev
# is: the bundled library is found beside the real binary, not the link.
mkdir -p "$tmp/links"
if ! ln -sf "$cli" "$tmp/links/blockwright-cli-link" 2>/dev/null || [[ ! -L $tmp/links/blockwright-cli-link ]]; then
  echo "skip the symlink case: this filesystem can't hold symlinks"
else
  out=$(env -u BLOCKWRIGHT_BUNDLED_LIBRARY BLOCKWRIGHT_USER_LIBRARY="$tmp/user" "$tmp/links/blockwright-cli-link" library list 2>&1); st=$?
  if [[ $st -eq 0 && $out == *"$lib"/sky/small-star.ans* ]]; then
    ok "the installed CLI run through a symlink lists the bundled library"
  else
    bad "library list through a symlink (exit $st): $out"
  fi
fi

exit $((fails > 0))
