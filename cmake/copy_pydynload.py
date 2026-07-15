#!/usr/bin/env python3
"""Copy the CPython lib-dynload C-extension modules (.so/.pyd) into a game bundle so
a shipped Python-language game can import struct/datetime/random/socket/json/select/…
(their C accelerators live here, not in the pure-Python stdlib zip). Called from
he_bundle_python_stdlib (macOS/Linux).

    copy_pydynload.py <src_lib-dynload> <dest_dir>

EXCLUDED modules — the only ones that pull in extra native libraries a game doesn't
need — so the bundle stays self-contained without chasing those deps:
  _tkinter / _curses / _curses_panel  → Tcl/Tk, ncurses (GUI/terminal toolkits)
  _ssl / _hashlib                     → OpenSSL (TLS + hashlib; documented limitation)
  _zstd                               → libzstd (the stdlib zstd wrapper)
Everything else here links only libSystem/glibc, so it copies verbatim and loads via
the game's already-relocated runtime. Missing this dir is NOT fatal (returns 0).
"""
import os
import shutil
import sys

EXCLUDE_STEMS = {"_tkinter", "_curses", "_curses_panel", "_ssl", "_hashlib", "_zstd"}


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: copy_pydynload.py <src_lib-dynload> <dest_dir>\n")
        return 2
    src, dest = sys.argv[1], sys.argv[2]
    if not os.path.isdir(src):
        sys.stderr.write("copy_pydynload.py: no lib-dynload dir at %s — skipping\n" % src)
        return 0
    os.makedirs(dest, exist_ok=True)
    copied = skipped = 0
    for f in os.listdir(src):
        if not (f.endswith(".so") or f.endswith(".pyd")):
            continue
        stem = f.split(".", 1)[0]
        if stem in EXCLUDE_STEMS:
            skipped += 1
            continue
        shutil.copy2(os.path.join(src, f), os.path.join(dest, f))
        copied += 1
    sys.stderr.write("copy_pydynload.py: copied %d modules (%d excluded) to %s\n"
                     % (copied, skipped, dest))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
