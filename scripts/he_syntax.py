#!/usr/bin/env python3
"""Fast per-file syntax check against the configured CMake build tree.

Compiling a single translation unit with -fsyntax-only takes a few seconds,
where a full `cmake --build` takes minutes and serialises every parallel
worker on the one shared build directory. This finds the CMake target that
owns a source file, reuses that target's flags.make (defines + include dirs)
and runs the compiler in syntax-only mode.

Usage:  python3 scripts/he_syntax.py <source-file> [<source-file> ...]
Header files have no TU of their own; pass the .cpp/.mm files that include
them instead.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")


def find_target_dirs():
    for dirpath, dirnames, filenames in os.walk(BUILD):
        if "flags.make" in filenames and "build.make" in filenames:
            yield dirpath


def owning_target(src_abs):
    """Return the *.dir path of a target whose build.make compiles src_abs."""
    rel = os.path.relpath(src_abs, BUILD)
    base = os.path.basename(src_abs)
    fallback = None
    for tdir in find_target_dirs():
        try:
            with open(os.path.join(tdir, "build.make"), "r", errors="ignore") as fh:
                text = fh.read()
        except OSError:
            continue
        if src_abs in text or rel in text:
            return tdir
        if fallback is None and base in text:
            fallback = tdir
    return fallback


def flags_for(tdir, src_abs):
    """Parse flags.make -> (compiler-ish lang key, defines, includes, flags)."""
    lang = "OBJCXX" if src_abs.endswith(".mm") else "CXX"
    out = {"DEFINES": "", "INCLUDES": "", "FLAGS": ""}
    path = os.path.join(tdir, "flags.make")
    with open(path, "r", errors="ignore") as fh:
        content = fh.read()
    # Lines look like:  CXX_DEFINES = -DFOO ...   (may be continued with \)
    content = content.replace("\\\n", " ")
    for key in out:
        m = re.search(r"^%s_%s\s*=\s*(.*)$" % (lang, key), content, re.M)
        if not m:
            m = re.search(r"^CXX_%s\s*=\s*(.*)$" % key, content, re.M)
        if m:
            out[key] = m.group(1).strip()
    return lang, out


def check(src):
    src_abs = os.path.abspath(src)
    if not os.path.exists(src_abs):
        print("MISSING: %s" % src)
        return 2
    if src_abs.endswith((".h", ".hpp", ".inl")):
        print("SKIP (header, no TU): %s" % src)
        return 0
    tdir = owning_target(src_abs)
    if tdir is None:
        print("NO-TARGET: %s (not compiled by any CMake target?)" % src)
        return 3
    lang, f = flags_for(tdir, src_abs)
    compiler = "/usr/bin/c++"
    cmd = [compiler]
    if lang == "OBJCXX":
        cmd += ["-x", "objective-c++"]
    cmd += f["DEFINES"].split() + f["INCLUDES"].split() + f["FLAGS"].split()
    # -Werror would make unrelated pre-existing warnings look like our failure.
    cmd = [c for c in cmd if c != "-Werror"]
    cmd += ["-fsyntax-only", src_abs]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode == 0:
        print("OK: %s" % os.path.relpath(src_abs, ROOT))
    else:
        print("FAIL: %s" % os.path.relpath(src_abs, ROOT))
        sys.stdout.write(proc.stderr[-8000:])
    return proc.returncode


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    rc = 0
    for a in sys.argv[1:]:
        rc |= check(a)
    sys.exit(rc)
