#!/usr/bin/env bash
# blockwright --list-actions with no display and an empty profile:
# list_actions_test.sh <scratch dir> <python3> <blockwright>...
# It prints a JSON array of actions with unique ids and exits 0, shows no
# window (there is no display to show one on), lists all 21 glyph sets though
# its ANSI document hides 17-21, and leaves the profile empty;
# with a file named it is a usage error. Its standard keys are a generic
# Linux desktop's under the Qt it runs on (the second line of its --version),
# and the desktop's platform, theme and style don't change a byte of it.
set -uo pipefail
tmp=$1; py=$2; shift 2
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

profile=$tmp/profile
rm -rf "$tmp" && mkdir -p "$profile"
env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM= HOME="$profile/home" XDG_CONFIG_HOME="$profile/config" \
    XDG_DATA_HOME="$profile/data" XDG_CACHE_HOME="$profile/cache" XDG_STATE_HOME="$profile/state" \
    "$@" --list-actions >"$tmp/out.json" 2>"$tmp/err.log"
st=$?
if [[ $st -eq 0 ]]; then ok "--list-actions exits 0"; else bad "--list-actions (exit $st): $(cat "$tmp/err.log")"; fi
qt=$(env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM= "$@" --version 2>/dev/null | sed -n '2s/^Qt \([0-9][0-9.]*\)$/\1/p')
if [[ -n $qt ]]; then ok "runs on Qt $qt"; else bad "--version names no Qt: $("$@" --version 2>&1 | head -n2)"; fi
if summary=$(BW_QT_VERSION=$qt "$py" -I - "$tmp/out.json" <<'PY'
import json, os, sys
actions = json.load(open(sys.argv[1], encoding="utf-8"))
assert isinstance(actions, list) and actions, "not a non-empty array"
ids = [a["id"] for a in actions]
assert all(ids), "an empty id"
assert len(ids) == len(set(ids)), "a repeated id"
for a in actions:
    for k in ("menu", "path", "text", "shortcuts", "statusTip", "toolTip", "checkable", "checked", "inSheet"):
        assert k in a, f"{a['id']} has no {k}"
keys = {a["id"]: a["shortcuts"] for a in actions}
# Qt 6.10 added the Undo and Redo keys to the generic desktop's table.
qt = tuple(int(n) for n in os.environ["BW_QT_VERSION"].split(".")[:2])
media = qt >= (6, 10)
want = {"quit": ["Ctrl+Q"], "undo": ["Ctrl+Z", "F14"] + ["Undo"] * media, "redo": ["Ctrl+Shift+Z"] + ["Redo"] * media,
        "saveAs": ["Ctrl+Shift+S"], "selectAll": ["Ctrl+A"]}
for id, k in want.items():
    assert keys[id] == k, f"{id} has {keys[id]}, not {k}"
# Every glyph set, though the dump's ANSI document hides 17-21 (#400, #399).
sets = [i for i in ids if i.startswith("glyphSet") and i[8:].isdigit()]
assert sets == ["glyphSet%d" % n for n in range(1, 22)], f"glyph sets {sets}"
offered = [a["id"] for a in actions if a["id"] in sets and a.get("offeredWhen") == "in IRC and UTF-8 documents"]
assert offered == sets[16:], f"offeredWhen on {offered}"
set21 = next(a for a in actions if a["id"] == "glyphSet21")
assert set21["text"] == "Set 21: \u2726\u2727\u2736\u2737\u2738\u2739\u22c6\u2731\u273a\u2756", f"set 21 {set21['text']!r}"
assert set21["shortcuts"] == [] and not set21["inSheet"], f"set 21 keys {set21['shortcuts']}"
tools = [a for a in actions if "tool" in a]
assert tools and all(a.get("help") == "tools/" + a["tool"] for a in tools), "a tool without its help id"
print(len(actions), "actions,", sum(a["inSheet"] for a in actions), "with a key,", len(tools), "tools,",
      len(sets), "glyph sets")
PY
); then ok "JSON: $summary"; else bad "JSON: $(head -c 400 "$tmp/out.json")"; fi
written=$(cd "$profile" && find . -mindepth 1 ! -path ./cache ! -path ./cache/fontconfig ! -path './cache/fontconfig/*' | sort)
if [[ -z $written ]]; then ok "the profile is left alone"; else bad "wrote into the profile: $written"; fi

# A theme that needs a display (gtk3) or has keys of its own (kde), another
# platform and a style: the same bytes. A theme plugin that isn't installed
# changes nothing either.
for setting in QT_QPA_PLATFORMTHEME=gtk3 QT_QPA_PLATFORMTHEME=kde QT_QPA_PLATFORMTHEME=qt6ct \
               QT_QPA_PLATFORM=xcb QT_STYLE_OVERRIDE=windows; do
    env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM= "$setting" HOME="$profile/home" \
        XDG_CONFIG_HOME="$profile/config" XDG_DATA_HOME="$profile/data" XDG_CACHE_HOME="$profile/cache" \
        XDG_STATE_HOME="$profile/state" "$@" --list-actions >"$tmp/env.json" 2>"$tmp/err.log"
    st=$?
    if [[ $st -eq 0 ]] && cmp -s "$tmp/out.json" "$tmp/env.json"; then
        ok "$setting: the same dump"
    else
        bad "$setting (exit $st): $(head -c 300 "$tmp/err.log"; diff "$tmp/out.json" "$tmp/env.json" | head -8)"
    fi
done

out=$(env -u DISPLAY -u WAYLAND_DISPLAY "$@" --list-actions a.ans 2>&1); st=$?
if [[ $st -eq 2 ]] && grep -q 'takes no file' <<<"$out"; then ok "with a file"; else bad "with a file (exit $st): $out"; fi

echo "$fails failure(s)"
[[ $fails -eq 0 ]]
