#!/usr/bin/env bash
# CLI library collection tests: cli_library_test.sh <blockwright-cli> <tests/data> <scratch dir>
# library new, delete, rename, add, remove, edit and list's collections, on
# a scratch user library, with HOME and XDG_DATA_HOME in the scratch dir so
# the trash is a scratch one too.
set -uo pipefail
cli=$1 data=$2 tmp=$3
case "${BW_CLI_WINDOWS:-}$(uname -s)" in
  1* | Darwin*)
    echo "skip: delete and remove use the system trash here (test_library.cpp covers the core)"
    exit 77
    ;;
esac
chmod -R u+rwx "$tmp" 2>/dev/null
rm -rf "$tmp" && mkdir -p "$tmp/home/data" "$tmp/bundled"
cp -R "$data/library/sky" "$tmp/bundled/sky"
export HOME=$tmp/home XDG_DATA_HOME=$tmp/home/data BLOCKWRIGHT_USER_LIBRARY=$tmp/lib \
  BLOCKWRIGHT_BUNDLED_LIBRARY=$tmp/bundled
lib=$tmp/lib
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# expect <name> <exit status> <stdout+stderr pattern> -- command...
expect() {
  local name=$1 want=$2 pat=$3; shift 4
  local out st
  out=$("$@" 2>&1); st=$?
  if [[ $st -ne $want ]]; then bad "$name: exit $st, wanted $want: $out"; return; fi
  if ! grep -Eq -- "$pat" <<<"$out"; then bad "$name: output doesn't match /$pat/: $out"; return; fi
  ok "$name"
}
gone() { if [[ -e $2 ]]; then bad "$1: $2 is still there"; else ok "$1"; fi; }
there() { if [[ -e $2 ]]; then ok "$1"; else bad "$1: no $2"; fi; }

expect "--help lists library new" 0 'library new <collection>' -- "$cli" --help
expect "--help lists library edit" 0 'library edit <stamp.ans>' -- "$cli" --help

# new
expect "new prints the folder" 0 "^$lib/my-trees\$" -- "$cli" library new "My Trees"
there "new makes the folder" "$lib/my-trees"
expect "new refuses a name in use" 1 'library new: a collection named "My Trees" already exists' -- \
  "$cli" library new "My Trees"
expect "new needs a name" 2 'library new takes a collection' -- "$cli" library new
expect "new refuses --yes" 2 '--yes isn.t a library new option' -- "$cli" library new --yes Rocks
expect "--dir is a library option only" 2 '--dir is a library option, not a info option' -- \
  "$cli" info --dir "$lib" "$data/library/sky/small-star.ans"

# list: the empty collection, from a fresh run
expect "list shows the empty collection" 0 "^collection	My Trees	0	$lib/my-trees\$" -- "$cli" library list
expect "list --dir shows it too" 0 "^collection	My Trees	0	" -- "$cli" library list --dir "$lib"
expect "list takes a dir or --dir" 2 'not both' -- "$cli" library list --dir "$lib" "$lib"

# add: a folder of stamps, a plain .ans named from its file
expect "add copies a folder's stamps" 0 "^added	Small star	$lib/my-trees/small-star\.ans\$" -- \
  "$cli" library add "My Trees" "$data/library/sky"
printf '\033[31mAB\r\n' > "$tmp/Big Moon.ans"
expect "add names a plain .ans from its file" 0 "^added	Big Moon	$lib/my-trees/big-moon\.ans\$" -- \
  "$cli" library add "My Trees" "$tmp/Big Moon.ans"
expect "add refuses a clash, adds the rest" 1 'already holds a stamp named "Small star"' -- \
  "$cli" library add "My Trees" "$data/library/sky/small-star.ans" "$data/library/figures"
# a folder is its own .ans files: not its subfolders' or hidden (dot-prefixed, AppleDouble ._ files too), and a folder with none is refused by name while the rest are added
mkdir -p "$tmp/tree/deeper" "$tmp/tree-empty"
printf '\033[31mAA\r\n' > "$tmp/tree/Top.ans"
printf '\033[32mBB\r\n' > "$tmp/tree/.Hid.ans"
printf 'AppleDouble' > "$tmp/tree/._Top.ans"
printf '\033[33mCC\r\n' > "$tmp/tree/deeper/Low.ans"
printf 'not art' > "$tmp/tree-empty/notes.txt"
"$cli" library new Folders >/dev/null
expect "add takes a folder's own .ans files" 1 "^added	Top	$lib/folders/top\.ans\$" -- \
  "$cli" library add Folders "$tmp/tree-empty" "$tmp/tree"
expect "add refuses a folder with no .ans file, naming it" 1 "tree-empty: no \.ans files in the folder" -- \
  "$cli" library add Folders "$tmp/tree-empty"
gone "add left a hidden .ans" "$lib/folders/hid.ans"
gone "add left an AppleDouble ._Top.ans, and Top kept its name" "$lib/folders/top-2.ans"
gone "add didn't stamp the AppleDouble file as ._Top" "$lib/folders/._top.ans"
gone "add left a subfolder's .ans" "$lib/folders/low.ans"
# a folder that can't be listed is refused once, naming it once
if [[ $(id -u) -ne 0 ]]; then
  mkdir "$tmp/locked" && chmod 000 "$tmp/locked"
  out=$("$cli" library add Folders "$tmp/locked" 2>&1); st=$?
  chmod 700 "$tmp/locked"
  if [[ $st -eq 1 && $out == *"locked: can't be listed: "* && $(grep -o "$tmp/locked" <<<"$out" | wc -l) -eq 1 ]]; then
    ok "add names a folder it can't list once"
  else
    bad "add names a folder it can't list once: exit $st: $out"
  fi
fi
# an error line keeps a tab or newline in a path or reason to one field, as list does
tab=$(printf '\t_'); tab=${tab%_}
printf '  ' > "$tmp/bad${tab}name.ans"
expect "add's error line has no raw tab" 1 'bad.*name\.ans: ' -- "$cli" library add Folders "$tmp/bad${tab}name.ans"
out=$("$cli" library add Folders "$tmp/bad${tab}name.ans" 2>&1)
case $out in
  *"$tab"*) bad "add's stderr holds a raw tab" ;;
  *) ok "add's stderr holds no raw tab" ;;
esac
expect "add needs a collection that exists" 1 'no collection named "Nope"' -- \
  "$cli" library add Nope "$tmp/Big Moon.ans"
expect "add needs a file" 2 'library add takes' -- "$cli" library add "My Trees"
# one call adds a batch, refusing a title it added itself
mkdir -p "$tmp/twin-a" "$tmp/twin-b"
printf '\033[32mTW\r\n' > "$tmp/twin-a/Twin.ans"
printf '\033[33mTW\r\n' > "$tmp/twin-b/Twin.ans"
expect "add in one call adds the first of two titles" 1 "^added	Twin	$lib/my-trees/twin\.ans\$" -- \
  "$cli" library add "My Trees" "$tmp/twin-a/Twin.ans" "$tmp/twin-b/Twin.ans"
there "add in one call wrote the first Twin" "$lib/my-trees/twin.ans"
gone "add in one call didn't write the second Twin" "$lib/my-trees/twin-2.ans"
expect "add in one call refuses the second, naming the first" 1 \
  "twin-b/Twin\.ans: the collection \"My Trees\" already holds a stamp named \"Twin\" \($lib/my-trees/twin\.ans\)" -- \
  "$cli" library add "My Trees" "$tmp/twin-b/Twin.ans"
expect "list counts the stamps" 0 "^collection	My Trees	[0-9]+	" -- "$cli" library list
expect "list shows a stamp in its collection" 0 "^stamp	My Trees	Big Moon	2x1	" -- "$cli" library list --dir "$lib"

# rename
expect "rename prints the new folder" 0 "^$lib/evergreens\$" -- "$cli" library rename "My Trees" Evergreens
gone "rename moves the folder" "$lib/my-trees"
expect "rename rewrites the category" 0 "^stamp	Evergreens	Big Moon	" -- "$cli" library list
expect "rename refuses an unknown collection" 1 'no collection named "My Trees"' -- \
  "$cli" library rename "My Trees" Other
expect "rename takes two names" 2 'library rename takes' -- "$cli" library rename Evergreens

# edit
moon=$lib/evergreens/big-moon.ans
expect "edit renames in place" 0 "^$moon\$" -- "$cli" library edit "$moon" --name "Full Moon" --tags night,sky
expect "edit's name and tags show in the list" 0 "^stamp	Evergreens	Full Moon	2x1	[0-9]+	-	night,sky	" -- \
  "$cli" library list
"$cli" library new Rocks >/dev/null
expect "edit's category moves the stamp to that collection" 0 "^$lib/rocks/full-moon\.ans\$" -- \
  "$cli" library edit "$moon" --category Rocks
moon=$lib/rocks/full-moon.ans
expect "edit needs a change" 2 'library edit needs --name, --category or --tags' -- "$cli" library edit "$moon"
expect "edit refuses a name CP437 can't hold" 1 'CP437 has no glyph' -- "$cli" library edit "$moon" --name "木"
expect "edit refuses a stamp outside the library" 1 "isn't in the library" -- \
  "$cli" library edit "$data/library/sky/small-star.ans" --tags x
expect "edit refuses --yes" 2 '--yes isn.t a library edit option' -- "$cli" library edit "$moon" --yes --tags x

# remove
expect "remove without --yes refuses" 1 'run again with --yes' -- "$cli" library remove "$moon"
there "remove without --yes keeps the stamp" "$moon"
expect "remove --yes removes it" 0 "	$moon\$" -- "$cli" library remove --yes "$moon"
gone "remove takes the art" "$moon"
gone "remove takes the sidecar" "${moon%.ans}.json"
expect "remove refuses a bundled stamp" 1 "isn't in the library" -- \
  "$cli" library remove --yes "$data/library/sky/small-star.ans"
there "the bundled stamp is untouched" "$data/library/sky/small-star.ans"
expect "remove needs a stamp" 2 'library remove takes' -- "$cli" library remove --yes

# delete
expect "delete without --yes says what would go" 1 "would remove $lib/evergreens and its [0-9]+ stamps" -- \
  "$cli" library delete Evergreens
there "delete without --yes keeps it" "$lib/evergreens"
expect "delete --yes removes it" 0 "(moved to the trash|deleted)	$lib/evergreens	[0-9]+ stamps" -- \
  "$cli" library delete --yes Evergreens
gone "delete takes the folder" "$lib/evergreens"
expect "delete refuses an unknown collection" 1 'no collection named "Evergreens"' -- \
  "$cli" library delete --yes Evergreens
expect "delete takes one name" 2 'library delete takes' -- "$cli" library delete --yes

# --dir instead of the user library
expect "--dir works on that library" 0 "^$tmp/other/signs\$" -- "$cli" library new --dir "$tmp/other" Signs
gone "--dir leaves the user library alone" "$lib/signs"
expect "an unknown subcommand is a usage error" 2 "unknown library subcommand 'frob'" -- "$cli" library frob

# Fix round 1 of #500's review.
fff=$(printf '\357\277\275')  # U+FFFD
"$cli" library new Trees >/dev/null
printf '\033[31mAB\r\n' > "$tmp/pine.ans"
printf '\033[32mCD\r\n' > "$tmp/Birch.ans"

# A trash that refuses removes nothing, and --permanent is the only delete.
if [[ $(id -u) -ne 0 ]]; then
  "$cli" library add Trees "$tmp/pine.ans" >/dev/null
  mkdir -p "$lib/trees/sub" && cp "$lib/trees/pine.ans" "$lib/trees/sub/s.ans" && cp "$lib/trees/pine.json" "$lib/trees/sub/s.json"
  chmod 555 "$lib"
  expect "delete --yes: a library that can't be written removes nothing" 1 "nothing was removed" -- \
    "$cli" library delete --yes Trees
  there "delete --yes: the stamp is still there" "$lib/trees/pine.ans"
  there "delete --yes: the subfolder's stamp too" "$lib/trees/sub/s.ans"
  expect "delete --permanent: a library that can't be written removes nothing" 1 "nothing was removed" -- \
    "$cli" library delete --yes --permanent Trees
  there "delete --permanent: the stamp is still there" "$lib/trees/pine.ans"
  chmod 755 "$lib"
  chmod 555 "$lib/trees/sub"
  expect "delete --permanent: a subfolder that can't be emptied removes nothing" 1 "sub can't be emptied.*nothing was removed" -- \
    "$cli" library delete --yes --permanent Trees
  there "delete --permanent: the top stamp is still there" "$lib/trees/pine.ans"
  chmod 755 "$lib/trees/sub"
  chmod 555 "$lib/trees"
  expect "remove --yes: a folder that can't be written removes nothing" 1 "nothing was removed" -- \
    "$cli" library remove --yes "$lib/trees/pine.ans"
  there "remove --yes: the stamp is still there" "$lib/trees/pine.ans"
  chmod 755 "$lib/trees"
  expect "remove --permanent deletes it" 0 "^deleted	$lib/trees/sub/s\.ans\$" -- \
    "$cli" library remove --yes --permanent "$lib/trees/sub/s.ans"
  gone "remove --permanent takes the sidecar" "$lib/trees/sub/s.json"
  "$cli" library remove --yes --permanent "$lib/trees/pine.ans" >/dev/null
fi
# No trash to be had (XDG_DATA_HOME a file): nothing goes, and --permanent
# is offered. A trash on the scratch dir's own volume may still take it.
"$cli" library add Trees "$tmp/pine.ans" >/dev/null
: > "$tmp/no-data"
out=$(XDG_DATA_HOME=$tmp/no-data "$cli" library delete --yes Trees 2>&1); st=$?
if [[ $st -eq 0 && $out == "moved to the trash"* ]]; then
  ok "delete --yes with no home trash (skipped: a trash on this volume took it)"
  "$cli" library new Trees >/dev/null
elif [[ $st -eq 1 && $out == *"can't be moved to the trash"*"nothing was removed; run again with --permanent"* && -e $lib/trees/pine.ans ]]; then
  ok "delete --yes with no trash removes nothing and offers --permanent"
  out=$(XDG_DATA_HOME=$tmp/no-data "$cli" library remove --yes "$lib/trees/pine.ans" 2>&1); st=$?
  if [[ $st -eq 1 && $out == *"run again with --permanent"* && -e $lib/trees/pine.ans ]]; then ok "remove --yes with no trash removes nothing"
  else bad "remove --yes with no trash: exit $st: $out"; fi
  expect "delete --permanent with no trash deletes it" 0 "^deleted	$lib/trees	1 stamp\$" -- \
    env XDG_DATA_HOME="$tmp/no-data" "$cli" library delete --yes --permanent Trees
  "$cli" library new Trees >/dev/null
else
  bad "delete --yes with no trash: exit $st: $out"
fi
expect "--permanent is delete's and remove's only" 2 '--permanent isn.t a library new option' -- \
  "$cli" library new --permanent Rocks2

# The bundled library is never changed, by --dir, a link or a folder in it.
before=$(cd "$tmp/bundled" && find . -type f -exec cksum {} + | sort)
ln -s "$tmp/bundled" "$tmp/bundled-link"
for d in "$tmp/bundled" "$tmp/bundled-link" "$tmp/bundled/sky"; do
  expect "new refuses $d" 1 'the bundled library.*never changed' -- "$cli" library new --dir "$d" Mine
  expect "delete refuses $d" 1 'the bundled library' -- "$cli" library delete --dir "$d" --yes sky
  expect "delete --permanent refuses $d" 1 'the bundled library' -- "$cli" library delete --dir "$d" --yes --permanent sky
  expect "rename refuses $d" 1 'the bundled library' -- "$cli" library rename --dir "$d" sky Heavens
  expect "add refuses $d" 1 'the bundled library' -- "$cli" library add --dir "$d" sky "$tmp/pine.ans"
  expect "remove refuses $d" 1 'the bundled library' -- "$cli" library remove --dir "$d" --yes "$d/sky/small-star.ans"
  expect "edit refuses $d" 1 'the bundled library' -- "$cli" library edit --dir "$d" --name X "$d/sky/small-star.ans"
done
after=$(cd "$tmp/bundled" && find . -type f -exec cksum {} + | sort)
if [[ $before == "$after" ]]; then ok "the bundled library is unchanged"; else bad "the bundled library changed"; fi
there "the bundled sky is still there" "$tmp/bundled/sky/small-star.ans"

# A stamp renamed in place still holds its title.
"$cli" library add Trees "$tmp/pine.ans" >/dev/null
expect "edit renames pine in place" 0 "^$lib/trees/pine\.ans\$" -- "$cli" library edit "$lib/trees/pine.ans" --name Birch
expect "add refuses the title a renamed stamp has" 1 "already holds a stamp named \"Birch\" \($lib/trees/pine\.ans\)" -- \
  "$cli" library add Trees "$tmp/Birch.ans"
gone "add wrote nothing" "$lib/trees/birch.ans"

# A collection name is capped.
long=$(printf '%0129d' 0 | tr 0 x)
expect "new refuses a name over 128 bytes" 1 'the collection name is longer than 128 bytes' -- "$cli" library new "$long"
expect "new takes a name of 128 bytes" 0 "^$lib/x+\$" -- "$cli" library new "${long:1}"

# Two folders going by one name: refused, naming both.
mkdir "$lib/zz" && cp "$lib/trees/pine.ans" "$lib/trees/pine.json" "$lib/zz/"
expect "delete refuses a name two folders go by" 1 "more than one folder goes by the collection name \"Trees\": $lib/trees, $lib/zz" -- \
  "$cli" library delete --yes --permanent Trees
there "the ambiguous delete removed nothing" "$lib/zz/pine.ans"
there "nor the other folder" "$lib/trees/pine.ans"
rm -r "$lib/zz"

# list: no raw tab or newline from a folder's name.
mkdir "$lib/a	b" "$lib/c
d"
out=$("$cli" library list 2>&1)
if grep -q "^collection	a${fff}b	0	$lib/a${fff}b\$" <<<"$out"; then ok "list shows a tab in a folder name as U+FFFD"
else bad "list's tab line: $out"; fi
if grep -q "^collection	c${fff}d	0	$lib/c${fff}d\$" <<<"$out"; then ok "list shows a newline in a folder name as U+FFFD"
else bad "list's newline line: $out"; fi
if grep -q '^d	' <<<"$out"; then bad "list split a line at a newline: $out"; else ok "list keeps every collection on one line"; fi
rm -r "$lib/a	b" "$lib/c
d"

# list: a user library that can't be read is a warning, exit 0.
if [[ $(id -u) -ne 0 ]]; then
  chmod 000 "$lib"
  expect "list warns about a library it can't list, exit 0" 0 "warning: .*$lib" -- "$cli" library list
  chmod 755 "$lib"
fi

# A Windows device name gets a folder Windows allows.
expect "new CON makes con-" 0 "^$lib/con-\$" -- "$cli" library new CON

# Fix round 2 of #500's review.
# A trash that refuses the sidecar takes nothing. The trash's info file is
# <name>.trashinfo, so a 241-byte stem fits for .ans (255 bytes) and not
# for .json (256).
"$cli" library new Long >/dev/null
"$cli" library add Long "$tmp/pine.ans" >/dev/null
stem=$(printf '%0241d' 0 | tr 0 l)
if mv "$lib/long/pine.ans" "$lib/long/$stem.ans" 2>/dev/null && mv "$lib/long/pine.json" "$lib/long/$stem.json" 2>/dev/null; then
  out=$("$cli" library remove --yes "$lib/long/$stem.ans" 2>&1); st=$?
  if [[ $st -eq 0 ]]; then
    ok "remove with a sidecar the trash refuses (skipped: this trash took both)"
  elif [[ $st -eq 1 && $out == *"$stem.json can't be moved to the trash"*"nothing was removed; run again with --permanent"* ]]; then
    ok "remove with a sidecar the trash refuses removes nothing and offers --permanent"
    there "the art is still there" "$lib/long/$stem.ans"
    there "the sidecar is still there" "$lib/long/$stem.json"
    expect "remove --permanent then takes the stamp" 0 "^deleted	$lib/long/$stem\.ans\$" -- \
      "$cli" library remove --yes --permanent "$lib/long/$stem.ans"
    gone "and its sidecar" "$lib/long/$stem.json"
  else
    bad "remove with a sidecar the trash refuses: exit $st: $out"
  fi
else
  ok "remove with a sidecar the trash refuses (skipped: this file system has no 246-byte names)"
fi

# remove without --yes says why a path isn't one it would take.
out=$("$cli" library remove "$tmp/bundled/sky/small-star.ans" "$lib/trees/nope.ans" 2>&1); st=$?
if [[ $st -eq 1 && $out == *"small-star.ans isn't in the library"* && $out == *"nope.ans doesn't exist"* && $out != *"would remove"* ]]; then
  ok "remove without --yes refuses what it wouldn't take"
else bad "remove without --yes on paths it wouldn't take: exit $st: $out"; fi
expect "remove without --yes names a stamp it would take" 1 "would remove $lib/trees/pine\.ans" -- \
  "$cli" library remove "$lib/trees/pine.ans"

# delete and remove print a tab in a folder's name as U+FFFD.
mkdir "$lib/t	x" && cp "$lib/trees/pine.ans" "$lib/trees/pine.json" "$lib/t	x/"
cp "$lib/trees/pine.ans" "$lib/t	x/fir.ans" && cp "$lib/trees/pine.json" "$lib/t	x/fir.json"
printf 'TX\n' > "$lib/t	x/.collection"  # its own name, not its stamps' category
out=$("$cli" library remove "$lib/t	x/fir.ans" "$lib/t	x/no.ans" 2>&1); st=$?
if [[ $st -eq 1 && $out == *"would remove $lib/t${fff}x/fir.ans"* && $out == *"$lib/t${fff}x/no.ans doesn't exist"* &&
  $out != *"	"* ]]; then
  ok "remove without --yes prints a tab in a path as U+FFFD"
else bad "remove without --yes, tab in a path: exit $st: $out"; fi
out=$("$cli" library delete TX 2>&1); st=$?
if [[ $st -eq 1 && $out == *"would remove $lib/t${fff}x and its 2 stamps"* && $out != *"	"* ]]; then
  ok "delete without --yes prints a tab in a path as U+FFFD"
else bad "delete without --yes, tab in a path: exit $st: $out"; fi
expect "remove prints a tab in the path as U+FFFD" 0 "^deleted	$lib/t${fff}x/fir\.ans\$" -- \
  "$cli" library remove --yes --permanent "$lib/t	x/fir.ans"
out=$("$cli" library delete --yes "N	O" 2>&1); st=$?
if [[ $st -eq 1 && $out == *"no collection named \"N${fff}O\""* && $out != *"	"* ]]; then
  ok "delete of no such collection prints a tab in its name as U+FFFD"
else bad "delete of no such collection, tab in its name: exit $st: $out"; fi
if [[ $(id -u) -ne 0 ]]; then
  # delete --yes refusals print the tab as U+FFFD too.
  chmod 555 "$lib/t	x"
  for how in "" --permanent; do
    out=$("$cli" library delete --yes $how TX 2>&1); st=$?
    if [[ $st -eq 1 && $out == *"$lib/t${fff}x can't be emptied"* && $out != *"	"* ]]; then
      ok "delete --yes ${how:+$how }refusal prints a tab in a path as U+FFFD"
    else bad "delete --yes ${how:+$how }refusal, tab in a path: exit $st: $out"; fi
  done
  chmod 755 "$lib/t	x"
fi
expect "delete prints a tab in the path as U+FFFD" 0 "^deleted	$lib/t${fff}x	1 stamp\$" -- \
  "$cli" library delete --yes --permanent TX
gone "delete took the tab-named folder" "$lib/t	x"

# Fix round 3 of #500's review.
if [[ $(id -u) -ne 0 ]]; then
  # A folder remove can't empty: the preview refuses it as --yes does.
  "$cli" library new RO >/dev/null
  "$cli" library add RO "$tmp/pine.ans" >/dev/null
  chmod 555 "$lib/ro"
  out=$("$cli" library remove "$lib/ro/pine.ans" 2>&1); st=$?
  if [[ $st -eq 1 && $out == *"$lib/ro can't be emptied"* && $out != *"would remove"* ]]; then
    ok "remove without --yes refuses a stamp in a folder it can't empty"
  else bad "remove without --yes, read-only folder: exit $st: $out"; fi
  expect "remove --yes refuses it the same way" 1 "$lib/ro can't be emptied" -- \
    "$cli" library remove --yes "$lib/ro/pine.ans"
  chmod 755 "$lib/ro"

  # A sidecar that can't be read couldn't be put back: the trash gets
  # neither file, and --permanent is offered.
  chmod 000 "$lib/ro/pine.json"
  out=$("$cli" library remove --yes "$lib/ro/pine.ans" 2>&1); st=$?
  if [[ $st -eq 1 && $out == *"pine.json can't be read"*"nothing was removed; run again with --permanent"* ]]; then
    ok "remove with a sidecar it can't read removes nothing and offers --permanent"
  else bad "remove with an unreadable sidecar: exit $st: $out"; fi
  there "the art is still there" "$lib/ro/pine.ans"
  there "the sidecar is still there" "$lib/ro/pine.json"
  expect "remove --permanent then takes the stamp" 0 "^deleted	$lib/ro/pine\.ans\$" -- \
    "$cli" library remove --yes --permanent "$lib/ro/pine.ans"
  gone "and its sidecar" "$lib/ro/pine.json"
fi

echo "$fails failed"
[[ $fails -eq 0 ]]
