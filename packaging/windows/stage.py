#!/usr/bin/env python3
"""Completes a Windows install tree for the MSI and writes its file list.

    stage.py <stage> <files.wxs> --qt <Qt for Windows> --runtime <dir> \
             --readobj <llvm-readobj> --licence <file>... [--sbom <file>...]

<stage> is what `cmake --install` made (bin/, share/). This copies in the Qt
plugins the program loads, every DLL the executables and plugins import that
is found in the Qt bin directory or a --runtime directory (and theirs, until
nothing new turns up), and the licence texts that go with them, then writes
<files.wxs>: one component per file under INSTALLDIR, each with a GUID made
from its path so it stays the same from one build to the next.
"""

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

# Plugins by directory; None takes every DLL the directory has.
PLUGINS = {
    "platforms": ["qwindows.dll"],
    "styles": ["qmodernwindowsstyle.dll"],
    "imageformats": None,
    "iconengines": ["qsvgicon.dll"],
}

# A DLL Windows has itself. Anything else an executable imports must be found
# in the Qt or runtime directories, or the staging fails.
SYSTEM = re.compile(
    r"^(api-ms-win-.*|ext-ms-.*|advapi32|bcrypt|comctl32|comdlg32|crypt32|d2d1|d3d11|d3d12|d3d9|dcomp|dwmapi|"
    r"dwrite|dxgi|gdi32|imm32|kernel32|mpr|msvcrt|netapi32|ntdll|ole32|oleaut32|setupapi|shcore|shell32|shlwapi|"
    r"user32|userenv|uxtheme|version|winhttp|winmm|winspool\.drv|ws2_32|wtsapi32|authz|dnsapi|iphlpapi|ncrypt|"
    r"secur32|cfgmgr32|opengl32|uiautomationcore|dbghelp|propsys|runtimeobject)(\.dll)?$",
    re.I,
)

NAMESPACE = uuid.UUID("4844bef3-dc55-4186-a9b8-d858decb36a8")  # the UpgradeCode


def imports(readobj, path):
    out = subprocess.run([readobj, "--coff-imports", str(path)], check=True, capture_output=True, text=True).stdout
    return re.findall(r"^\s*Name: (\S+\.dll)\s*$", out, re.M | re.I)


def deploy(stage, qt, runtimes, readobj):
    bin_dir = stage / "bin"
    for sub, names in PLUGINS.items():
        src = qt / "plugins" / sub
        found = sorted(p.name for p in src.glob("*.dll")) if names is None else names
        for name in found:
            if not (src / name).is_file():
                sys.exit(f"stage.py: no Qt plugin {sub}/{name} in {qt}")
            (bin_dir / sub).mkdir(parents=True, exist_ok=True)
            shutil.copy2(src / name, bin_dir / sub / name)

    # The runtime directories first: Qt's bin has its own libc++.dll, from the
    # compiler Qt was built with, which can lack what newer compilers import.
    search = [*runtimes, qt / "bin"]
    pending = sorted(bin_dir.rglob("*.exe")) + sorted(bin_dir.rglob("*.dll"))
    seen = {p.name.lower() for p in bin_dir.glob("*.dll")}
    missing = set()
    while pending:
        for name in imports(readobj, pending.pop()):
            key = name.lower()
            if key in seen or SYSTEM.match(key):
                continue
            src = next((d / name for d in search if (d / name).is_file()), None)
            if src is None:
                missing.add(name)
                continue
            seen.add(key)
            shutil.copy2(src, bin_dir / name)
            pending.append(bin_dir / name)
    if missing:
        sys.exit("stage.py: imported but found nowhere: " + ", ".join(sorted(missing)))


def licences(stage, files, sboms):
    dest = stage / "share" / "doc" / "blockwright" / "third-party"
    dest.mkdir(parents=True, exist_ok=True)
    for f in files:
        shutil.copy2(f, dest / f.name)
    if sboms:
        (dest / "qt-sbom").mkdir(exist_ok=True)
        for f in sboms:
            shutil.copy2(f, dest / "qt-sbom" / f.name)


def ident(prefix, rel):
    return prefix + hashlib.sha1(rel.encode()).hexdigest()[:24]


def xml(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def wxs(stage, out):
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             '<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">', "  <Fragment>"]
    refs = []

    def walk(directory, rel, depth):
        pad = "  " * depth
        for entry in sorted(directory.iterdir(), key=lambda p: (p.is_dir(), p.name)):
            r = f"{rel}/{entry.name}" if rel else entry.name
            if entry.is_dir():
                lines.append(f'{pad}<Directory Id="{ident("d", r)}" Name="{xml(entry.name)}">')
                walk(entry, r, depth + 1)
                lines.append(f"{pad}</Directory>")
            else:
                cid = ident("c", r)
                guid = str(uuid.uuid5(NAMESPACE, r.lower())).upper()
                lines.append(f'{pad}<Component Id="{cid}" Guid="{guid}" Win64="yes">')
                lines.append(f'{pad}  <File Id="{ident("f", r)}" Name="{xml(entry.name)}" '
                             f'Source="{xml(str(entry))}" KeyPath="yes"/>')
                lines.append(f"{pad}</Component>")
                refs.append(cid)

    lines.append('    <DirectoryRef Id="INSTALLDIR">')
    walk(stage, "", 3)
    lines.append("    </DirectoryRef>")
    lines.append('    <ComponentGroup Id="InstallFiles">')
    lines += [f'      <ComponentRef Id="{c}"/>' for c in refs]
    lines += ["    </ComponentGroup>", "  </Fragment>", "</Wix>", ""]
    out.write_text("\n".join(lines), encoding="utf-8")
    return len(refs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stage", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--qt", type=Path, required=True)
    ap.add_argument("--runtime", type=Path, action="append", default=[])
    ap.add_argument("--readobj", required=True)
    ap.add_argument("--licence", type=Path, action="append", default=[])
    ap.add_argument("--sbom", type=Path, action="append", default=[])
    a = ap.parse_args()
    deploy(a.stage, a.qt, a.runtime, a.readobj)
    licences(a.stage, a.licence, a.sbom)
    n = wxs(a.stage, a.out)
    print(f"stage.py: {n} files in {a.out}")


if __name__ == "__main__":
    main()
