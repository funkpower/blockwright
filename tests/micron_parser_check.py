#!/usr/bin/env python3
"""micron_parser_check.py <page.mu> <expected.json>: reads a Micron page with
NomadNet's own parser, as its browser does (strip_modifiers, then
MicronParser.markup_to_attrmaps), and checks that every row comes back with
the text, colours, formatting and alignment the expected file gives. Exits 77 when
nomadnet can't be imported.

The expected file is JSON: {"align": "left"|"center", "rows": [[cell...]...]},
each cell [text, fg, bg, formatting], fg and bg "#rrggbb" or null where the
cell doesn't show that colour (a blank's foreground, the background of a full
block that isn't italic), formatting the letters of the toggles on, in the
order "biu" (bold, italic, underline), "" for none."""
import json
import sys

try:
    import urwid
    import nomadnet
    import nomadnet.ui
    import nomadnet.ui.TextUI
    from nomadnet.util import strip_modifiers
    from nomadnet.ui.textui import MicronParser
except Exception as e:  # nomadnet, or one of its dependencies, is missing
    print(f"SKIP: nomadnet can't be imported: {e!r}")
    sys.exit(77)


class _Screen(urwid.BaseScreen):
    def draw_screen(self, size, canvas):
        pass


class _UI:
    def __init__(self):
        self.screen = _Screen()
        self.colormode = 2**24
        self.glyphs = {}


class _App:
    def __init__(self):
        self.config = {"textui": {"theme": nomadnet.ui.TextUI.THEME_DARK}}
        self.ui = _UI()


_app = _App()
nomadnet.NomadNetworkApp.get_shared_instance = staticmethod(lambda: _app)


def norm(c):
    """A Micron colour as its high_color has it: #rrggbb."""
    if len(c) == 3:
        return "#" + "".join(ch * 2 for ch in c.lower())
    if len(c) == 6:
        return "#" + c.lower()
    return c


_LETTERS = {"bold": "b", "italics": "i", "underline": "u"}


def style_parts(name):
    """fg, bg and formatting letters of a style make_style names
    "micron_<fg>_<bg>_<,bold,underline,italics>"."""
    parts = name.split("_", 3)
    on = {_LETTERS[f] for f in parts[3].split(",") if f} if len(parts) > 3 else set()
    return norm(parts[1]), norm(parts[2]), "".join(ch for ch in "biu" if ch in on)


def main():
    page, expected_path = sys.argv[1], sys.argv[2]
    with open(page, encoding="utf-8") as f:
        markup = f.read()
    with open(expected_path, encoding="utf-8") as f:
        expected = json.load(f)
    maps = MicronParser.markup_to_attrmaps(strip_modifiers(markup))
    rows = expected["rows"]
    fails = []
    if len(maps) != len(rows):
        fails.append(f"{len(maps)} rows drawn, {len(rows)} expected")
    for y, (m, cells) in enumerate(zip(maps, rows)):
        w = m.original_widget
        if not isinstance(w, urwid.Text):
            fails.append(f"row {y}: drawn as {type(w).__name__}, not text")
            continue
        if w.align != expected["align"]:
            fails.append(f"row {y}: aligned {w.align}, expected {expected['align']}")
        text, runs = w.get_text()
        want = "".join(c[0] for c in cells)
        if text != want:
            fails.append(f"row {y}: text {text!r}, expected {want!r}")
            continue
        per_char = []
        for name, n in runs:
            per_char += [name] * n
        if len(per_char) < len(text):  # urwid leaves the tail in the widget's own style
            per_char += [m.attr_map[None]] * (len(text) - len(per_char))
        i = 0
        for x, (t, fg, bg, fmt) in enumerate(cells):
            if not t:
                continue
            got_fg, got_bg, got_fmt = style_parts(per_char[i])
            if got_fmt != fmt:
                fails.append(f"cell {x},{y} {t!r}: formatting {got_fmt!r}, expected {fmt!r}")
            if fg is not None and got_fg != fg:
                fails.append(f"cell {x},{y} {t!r}: foreground {got_fg}, expected {fg}")
            if bg is not None and got_bg != bg:
                fails.append(f"cell {x},{y} {t!r}: background {got_bg}, expected {bg}")
            i += len(t)
    for f in fails[:20]:
        print("FAIL", f)
    if len(fails) > 20:
        print(f"... and {len(fails) - 20} more")
    print(f"{len(rows)} rows, {sum(len(r) for r in rows)} cells: {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
