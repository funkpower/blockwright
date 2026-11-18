#!/usr/bin/env bash
# The bundled library through a bind mount: cli_library_bind_test.sh <blockwright-cli> <tests/data> <scratch dir>
# A bind mount gives the bundled folder a second path that resolves to a
# different string, so only file identity tells them apart. It runs in an
# unprivileged user and mount namespace (`unshare -rm`), and skips where
# there is none (macOS, FreeBSD, and the CI containers, whose seccomp
# profile refuses unprivileged user namespaces), so it runs on the dev
# machine. test_library.cpp covers the same checks everywhere, with
# sameFolderHook standing in for the mount.
set -uo pipefail
cli=$1 data=$2 tmp=$3
if [[ -n ${BW_CLI_WINDOWS:-} ]] || ! unshare -rm true 2>/dev/null; then
  echo "skip: no unprivileged mount namespace (unshare -rm) here"
  exit 77
fi
chmod -R u+rwx "$tmp" 2>/dev/null
rm -rf "$tmp" && mkdir -p "$tmp/home/data" "$tmp/top/bundled" "$tmp/alias" "$tmp/alias-top" "$tmp/alias-sky" "$tmp/user/sky"
cp -R "$data/library/sky" "$tmp/top/bundled/sky"
cp "$data/library/sky/small-star.ans" "$tmp/pine.ans"
export HOME=$tmp/home XDG_DATA_HOME=$tmp/home/data BLOCKWRIGHT_USER_LIBRARY=$tmp/lib \
  BLOCKWRIGHT_BUNDLED_LIBRARY=$tmp/top/bundled
before=$(cd "$tmp/top/bundled" && find . -type f -exec cksum {} + | sort)
dirs_before=$(cd "$tmp/top/bundled" && find . -type d | sort)

# Everything below runs inside the namespace, where the mounts exist; it
# prints ok/FAIL lines and exits with the number of failures.
cat >"$tmp/inside.sh" <<'INSIDE'
set -u
cli=$1 tmp=$2
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
expect() {
  local name=$1 want=$2 pat=$3; shift 4
  local out st
  out=$("$@" 2>&1); st=$?
  if [[ $st -ne $want ]]; then bad "$name: exit $st, wanted $want: $out"; return; fi
  if ! grep -Eq -- "$pat" <<<"$out"; then bad "$name: output doesn't match /$pat/: $out"; return; fi
  ok "$name"
}
mount --bind "$tmp/top/bundled" "$tmp/alias" || { echo "FAIL mount --bind"; exit 1; }
mount --bind "$tmp/top" "$tmp/alias-top" || { echo "FAIL mount --bind top"; exit 1; }
# is, is inside (an existing folder and one not made yet), holds
for d in "$tmp/alias" "$tmp/alias/sky" "$tmp/alias/sky/new" "$tmp/alias-top"; do
  expect "new refuses $d" 1 'the bundled library.*never changed' -- "$cli" library new --dir "$d" Planted
  expect "delete refuses $d" 1 'the bundled library' -- "$cli" library delete --dir "$d" --yes --permanent sky
  expect "rename refuses $d" 1 'the bundled library' -- "$cli" library rename --dir "$d" sky Heavens
  expect "add refuses $d" 1 'the bundled library' -- "$cli" library add --dir "$d" sky "$tmp/pine.ans"
  expect "edit refuses $d" 1 'the bundled library' -- "$cli" library edit --dir "$d" --name X "$tmp/alias/sky/small-star.ans"
  expect "remove refuses $d" 1 'the bundled library' -- "$cli" library remove --dir "$d" --yes --permanent "$tmp/alias/sky/small-star.ans"
done
expect "new says which: is" 1 "alias is the bundled library" -- "$cli" library new --dir "$tmp/alias" Planted
expect "new says which: inside" 1 "alias/sky is inside the bundled library" -- "$cli" library new --dir "$tmp/alias/sky" Planted
expect "new says which: holds" 1 "alias-top holds the bundled library" -- "$cli" library new --dir "$tmp/alias-top" Planted
# A folder inside it at a second path: as the library, and as a collection
# of a user library (each folder a command writes in is checked).
mount --bind "$tmp/top/bundled/sky" "$tmp/alias-sky" || { echo "FAIL mount --bind sky"; exit 1; }
expect "new refuses a bind of a bundled folder" 1 "alias-sky is inside the bundled library" -- \
  "$cli" library new --dir "$tmp/alias-sky" Planted
mount --bind "$tmp/top/bundled/sky" "$tmp/user/sky" || { echo "FAIL mount --bind user/sky"; exit 1; }
expect "edit refuses a collection bound to a bundled one" 1 "user/sky is inside the bundled library" -- \
  "$cli" library edit --dir "$tmp/user" --name Changed "$tmp/user/sky/small-star.ans"
expect "add refuses a collection bound to a bundled one" 1 "user/sky is inside the bundled library" -- \
  "$cli" library add --dir "$tmp/user" sky "$tmp/pine.ans"
expect "remove refuses a collection bound to a bundled one" 1 "user/sky is inside the bundled library" -- \
  "$cli" library remove --dir "$tmp/user" --yes --permanent "$tmp/user/sky/small-star.ans"
expect "rename refuses a collection bound to a bundled one" 1 "user/sky is inside the bundled library" -- \
  "$cli" library rename --dir "$tmp/user" sky Heavens
expect "delete refuses a collection bound to a bundled one" 1 "user/sky is inside the bundled library" -- \
  "$cli" library delete --dir "$tmp/user" --yes --permanent sky
# replay's saveToLibrary into a collection bound to a bundled one
cat >"$tmp/save.json" <<'JSON'
{"version": 1, "start": {"blank": [20, 10]}, "steps": [{"name": "ink", "action": "keyboard", "at": [1, 1], "text": "hi"}, {"name": "pick", "action": "select", "rect": [1, 1, 2, 1]}, {"name": "save", "action": "saveToLibrary", "stamp": "Hi", "category": "sky"}]}
JSON
expect "replay saveToLibrary refuses a collection bound to a bundled one" 1 "user/sky is inside the bundled library" -- \
  "$cli" replay "$tmp/save.json" -o "$tmp/out/save" --user-library "$tmp/user"
# a plain save into a folder not made yet, inside the bundled library (by the
# mount, and by path), is refused before any folder is made there
sed 's/"category": "sky"/"category": "misc"/' "$tmp/save.json" >"$tmp/plain.json"
mkdir -p "$tmp/user2/trees"
mount --bind "$tmp/top/bundled/sky" "$tmp/user2/trees" || { echo "FAIL mount --bind user2/trees"; exit 1; }
expect "replay save into a new folder through a bind mount is refused" 1 "user2/trees/new is inside the bundled library" -- \
  "$cli" replay "$tmp/plain.json" -o "$tmp/out/a" --user-library "$tmp/user2/trees/new"
expect "replay save into a new folder inside the bundled library is refused" 1 "top/bundled/sky/deeper/new is inside the bundled library" -- \
  "$cli" replay "$tmp/plain.json" -o "$tmp/out/b" --user-library "$tmp/top/bundled/sky/deeper/new"
if [[ -e $tmp/out/a || -e $tmp/out/b ]]; then bad "a refused save wrote captures"; fi
# A bind mount below a collection folder: rename and delete act on all of it
mkdir -p "$tmp/user/trees/nested"
mount --bind "$tmp/top/bundled/sky" "$tmp/user/trees/nested" || { echo "FAIL mount --bind nested"; exit 1; }
printf 'Trees\n' >"$tmp/user/trees/.collection" 2>/dev/null
expect "rename refuses a bind mount below the collection" 1 "trees/nested is inside the bundled library" -- \
  "$cli" library rename --dir "$tmp/user" Trees Woods
expect "delete refuses a bind mount below the collection" 1 "trees/nested is inside the bundled library" -- \
  "$cli" library delete --dir "$tmp/user" --yes --permanent Trees
# a save that replaces a stamp below the collection writes in that stamp's folder
sed 's/"stamp": "Hi", "category": "sky"/"stamp": "Small star", "category": "Trees"/' "$tmp/save.json" >"$tmp/replace.json"
expect "replay saveToLibrary refuses to replace a stamp below the collection" 1 "trees/nested is inside the bundled library" -- \
  "$cli" replay "$tmp/replace.json" -o "$tmp/out/replace" --user-library "$tmp/user"
# A library beside it, through the same mount, is the user's own.
mkdir -p "$tmp/top/user"
expect "a library beside it works" 0 "^$tmp/alias-top/user/rocks\$" -- "$cli" library new --dir "$tmp/alias-top/user" Rocks
exit "$fails"
INSIDE

unshare -rm bash "$tmp/inside.sh" "$cli" "$tmp"
fails=$?
after=$(cd "$tmp/top/bundled" && find . -type f -exec cksum {} + | sort)
if [[ $before == "$after" ]]; then echo "ok   the bundled library is unchanged"
else echo "FAIL the bundled library changed"; fails=$((fails + 1)); fi
dirs_after=$(cd "$tmp/top/bundled" && find . -type d | sort)
if [[ $dirs_before == "$dirs_after" ]]; then echo "ok   the bundled library has no new folders"
else echo "FAIL the bundled library gained or lost folders: $dirs_after"; fails=$((fails + 1)); fi
if [[ -e $tmp/top/bundled/planted ]]; then echo "FAIL a collection was planted"; fails=$((fails + 1)); fi
echo "$fails failure(s)"
[[ $fails -eq 0 ]]
