#!/usr/bin/env python3
"""packaging/debian/copyright gives every file of the source the licence it
really has:  debian_copyright_test.py <source dir>

Each file (git ls-files in a checkout, every file of an unpacked release
tarball but a package build's debian/, .pc/ and CMake build trees) is
resolved against the Files: stanzas the DEP-5 way: `*` matches any run of
characters, `/` included, `?` one character, and the last stanza that matches
wins. The licence it gets must be the one the file states:

- a comment line in its first 15 lines saying `licensed under <licence>` or
  `SPDX-License-Identifier: <licence>`;
- otherwise, in a directory whose README.md has a `**Licence:**` line (a
  vendored third-party directory), that licence for every file but the
  README itself, which is funkpower's text under GPL-3+;
- otherwise nothing is checked (the file falls to `Files: *`).

A file that matches no stanza, or a stanza whose licence has no `License:`
paragraph, fails too; a licence of alternatives (`A or B`) needs a paragraph
for each.
"""
import os
import re
import subprocess
import sys

# How a header or README names a licence -> the copyright file's short name.
NAMES = [
    (re.compile(r"OFL-1\.1 or GPL-2\+ with Font exception"), "OFL-1.1 or GPL-2+ with Font exception"),
    (re.compile(r"CC BY-SA 4\.0|CC-BY-SA-4\.0|Attribution-ShareAlike 4\.0"), "CC-BY-SA-4.0"),
    (re.compile(r"SIL Open Font License,? (Version )?1\.1|OFL-1\.1"), "OFL-1.1"),
    (re.compile(r"GPL-3\.0-or-later|GPL-3\+"), "GPL-3+"),
    (re.compile(r"CC0-1\.0"), "CC0-1.0"),
    (re.compile(r"\bMIT\b|Expat"), "Expat"),
]
STATED = re.compile(r"(?:licensed under|SPDX-License-Identifier:)\s*(.+)", re.I)
COMMENT = re.compile(r"\s*(//|#|/\*|\*|<!--|;)")
README_LICENCE = re.compile(r"^\s*-\s*\*\*Licen[cs]e:\*\*\s*(.+)$", re.M)
# A package build runs inside the unpacked tarball: leave out debian/ and
# quilt's .pc/ at the top, and any CMake build tree (obj-*, redhat-linux-build).
TOP_SKIP = {".git", "debian", ".pc"}


def licence_name(text):
    for rx, name in NAMES:
        if rx.search(text):
            return name
    return None


def parse_copyright(path):
    stanzas, licences = [], set()
    paras = re.split(r"\n\s*\n", open(path, encoding="utf-8").read())
    for para in paras:
        fields, key = {}, None
        for line in para.splitlines():
            if line[:1] in (" ", "\t") and key:
                fields[key] += "\n" + line.strip()
            elif ":" in line:
                key, _, val = line.partition(":")
                key = key.strip()
                fields[key] = val.strip()
        if "Files" in fields:
            stanzas.append((fields["Files"].split(), fields.get("License", "").split("\n")[0].strip()))
        elif "License" in fields and "\n" in fields["License"]:
            licences.add(fields["License"].split("\n")[0].strip())
    return stanzas, licences


def glob_rx(pattern):
    out = []
    for ch in pattern:
        out.append(".*" if ch == "*" else "." if ch == "?" else re.escape(ch))
    return re.compile("".join(out) + r"\Z", re.S)


def files_of(src):
    if os.path.exists(os.path.join(src, ".git")):
        out = subprocess.run(["git", "-C", src, "ls-files", "-z"], check=True,
                             capture_output=True).stdout.decode()
        return sorted(f for f in out.split("\0") if f)
    found = []
    for root, dirs, names in os.walk(src):
        top = os.path.samefile(root, src)
        dirs[:] = [d for d in dirs
                   if not (top and d in TOP_SKIP)
                   and not os.path.exists(os.path.join(root, d, "CMakeCache.txt"))]
        for n in names:
            found.append(os.path.relpath(os.path.join(root, n), src).replace(os.sep, "/"))
    return sorted(found)


def stated_licence(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            head = [next(fh, "") for _ in range(15)]
    except OSError:
        return None
    for line in head:
        if not COMMENT.match(line):
            continue
        m = STATED.search(line)
        if m:
            name = licence_name(m.group(1))
            if name:
                return name
    return None


def main():
    src = sys.argv[1]
    stanzas, licences = parse_copyright(os.path.join(src, "packaging/debian/copyright"))
    compiled = [([glob_rx(p) for p in pats], lic, pats) for pats, lic in stanzas]
    fails = 0

    for _, lic, pats in compiled:
        for alt in lic.split(" or "):
            if alt not in licences:
                print(f"FAIL stanza {' '.join(pats)}: no 'License: {alt}' paragraph")
                fails += 1

    dir_licence = {}
    files = files_of(src)
    for f in files:
        if f.rsplit("/", 1)[-1] == "README.md" and "/" in f:
            m = README_LICENCE.search(open(os.path.join(src, f), encoding="utf-8").read())
            if m and licence_name(m.group(1)):
                dir_licence[f.rsplit("/", 1)[0]] = licence_name(m.group(1))

    checked = 0
    for f in files:
        got = None
        for rxs, lic, _ in compiled:
            if any(rx.match(f) for rx in rxs):
                got = lic
        if got is None:
            print(f"FAIL {f}: matches no Files: stanza")
            fails += 1
            continue
        want, why = stated_licence(os.path.join(src, f)), "its header"
        if want is None:
            d = f.rsplit("/", 1)[0] if "/" in f else ""
            while d and d not in dir_licence:
                d = d.rsplit("/", 1)[0] if "/" in d else ""
            if d:
                if f == d + "/README.md":
                    want, why = "GPL-3+", "funkpower's README of a vendored directory"
                else:
                    want, why = dir_licence[d], f"{d}/README.md"
        if want is None:
            continue
        checked += 1
        if got != want:
            print(f"FAIL {f}: resolves to {got}, {why} says {want}")
            fails += 1

    if checked == 0:
        print("FAIL no file states a licence; the header or README patterns are broken")
        fails += 1
    print(f"{len(files)} files, {checked} with a stated licence, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
