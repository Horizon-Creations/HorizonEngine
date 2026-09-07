#!/usr/bin/env python3
"""Turn a TTF into the C header the engine embeds it as.

The runtime bakes its font atlas from bytes compiled into HE_Core, so a font the
engine ships has to become a byte array. That was done by hand once (Roboto
Condensed Bold); this is the same output, repeatable, so the next face is one
command and not an afternoon.

    python3 scripts/embed_font.py EditorDeps/Fonts/Foo.ttf src/HE_Core/vendor/Foo_ttf.h Foo

The symbol pair is `<name>_size` / `<name>_data`, matching Roboto_ttf.h byte for
byte in shape — including the leading comment, which is the only place the file
says where it came from.
"""
import os
import sys


def emit(ttf_path: str, out_path: str, symbol: str, note: str) -> None:
    with open(ttf_path, "rb") as f:
        data = f.read()

    rel = os.path.relpath(ttf_path, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    lines = [
        f"// File: '{rel}' ({len(data)} bytes) — embedded for the runtime UI font atlas.",
        f"// {note}",
        "#pragma once",
        f"static const unsigned int {symbol}_size = {len(data)};",
        f"static const unsigned char {symbol}_data[{len(data)}] =",
        "{",
    ]

    # 100-ish columns of decimal bytes, the same shape the hand-made header has.
    row, width = [], 0
    for b in data:
        s = str(b)
        if width + len(s) + 1 > 100:
            lines.append("    " + ",".join(row) + ",")
            row, width = [], 0
        row.append(s)
        width += len(s) + 1
    if row:
        lines.append("    " + ",".join(row))
    lines.append("};")

    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{out_path}: {len(data)} bytes as {symbol}_data")


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    ttf, out, symbol = sys.argv[1], sys.argv[2], sys.argv[3]
    note = sys.argv[4] if len(sys.argv) > 4 else "Embedded font face."
    emit(ttf, out, symbol, note)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
