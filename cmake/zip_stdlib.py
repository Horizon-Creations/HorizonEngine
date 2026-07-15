#!/usr/bin/env python3
"""Zip the pure-Python CPython standard library for shipping a game with embedded
Python. Called from src/HE_Game/CMakeLists.txt at build time as:

    python zip_stdlib.py <stdlib_dir> <out.zip> [store]

Modules are stored at the archive ROOT (paths relative to <stdlib_dir>), which is
what embedded Python expects for a `pythonXY.zip` on sys.path. Only .py sources are
included (embedded Python recompiles them); __pycache__ and large packages a game
never needs (tests, IDLE, tkinter, ...) are excluded so the bundle stays small
(~3 MB deflated vs. ~200 MB for a naive shutil.make_archive of the whole stdlib).

Pass "store" as the 3rd arg to write an UNCOMPRESSED (ZIP_STORED) archive (~10 MB).
This is REQUIRED on macOS/Linux: a DEFLATE zip can't be read during interpreter
bootstrap there because `zlib` is a separate C-extension in lib-dynload that isn't
loaded yet, so embedded Python dies with "Failed to import encodings module /
zlib not available". Windows freezes zlib into python3XY.dll, so it uses DEFLATE.

Note: C extension modules (.pyd/.so in DLLs/lib-dynload) are NOT bundled — scripts
are limited to the pure-Python stdlib + modules frozen/built into the interpreter
(json, math, random, datetime, enum, dataclasses, typing, ...). socket/ssl/sqlite3/
ctypes need their compiled module shipped separately (follow-up).
"""
import os
import sys
import zipfile

# Directories skipped anywhere in the tree: build/editor tooling + huge unused packages.
EXCLUDE_DIRS = {
    "__pycache__", "test", "tests", "idlelib", "tkinter", "turtledemo",
    "lib2to3", "ensurepip", "site-packages", "venv", "distutils", "__phello__",
}


def main() -> int:
    if len(sys.argv) not in (3, 4):
        sys.stderr.write("usage: zip_stdlib.py <stdlib_dir> <out.zip> [store]\n")
        return 2
    src, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    compression = zipfile.ZIP_STORED if (len(sys.argv) == 4 and sys.argv[3] == "store") \
        else zipfile.ZIP_DEFLATED
    if not os.path.isdir(src):
        sys.stderr.write("zip_stdlib.py: stdlib dir not found: %s\n" % src)
        return 1
    count = 0
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with zipfile.ZipFile(out, "w", compression) as z:
        for root, dirs, files in os.walk(src):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for f in files:
                if f.endswith(".py"):
                    full = os.path.join(root, f)
                    z.write(full, os.path.relpath(full, src))
                    count += 1
    sys.stderr.write("zip_stdlib.py: wrote %d modules to %s (%s)\n"
                     % (count, out, "stored" if compression == zipfile.ZIP_STORED else "deflated"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
