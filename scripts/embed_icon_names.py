#!/usr/bin/env python3
"""Turn an icon font's .codepoints list into the name→codepoint table.

`<icon=home>` has to become U+E88A somewhere, and the somewhere is a table the
engine carries: the TTF itself knows only that E88A has an outline, never that
the outline is called "home". Material Icons ships the mapping beside the font,
so this reads that file rather than inventing names.

    python3 scripts/embed_icon_names.py EditorDeps/Fonts/X.codepoints \\
            src/HE_Core/vendor/MaterialIcons_names.h MaterialIcons

Sorted by name so the lookup is a binary search — 2000-odd entries walked
linearly per icon per frame would be a measurable cost for a table that never
changes.
"""
import os
import sys


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    src, out_path, symbol = sys.argv[1], sys.argv[2], sys.argv[3]

    pairs = []
    with open(src) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 2:
                continue
            name, hexcp = parts
            cp = int(hexcp, 16)
            # The list ends with a sentinel outside the Private Use Area; it is
            # not an icon and would only ever be a name nobody can draw.
            if not (0xE000 <= cp <= 0xF8FF):
                continue
            pairs.append((name, cp))
    pairs.sort(key=lambda p: p[0])

    rel = os.path.relpath(src, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    lines = [
        f"// Generated from '{rel}' by scripts/embed_icon_names.py — do not edit.",
        f"// {len(pairs)} icon names, sorted, for a binary search.",
        "#pragma once",
        "#include <cstdint>",
        "struct HEIconName { const char* name; std::uint32_t cp; };",
        f"static const HEIconName {symbol}_names[] = {{",
    ]
    for name, cp in pairs:
        lines.append(f'    {{ "{name}", 0x{cp:04X} }},')
    lines.append("};")
    lines.append(f"static const unsigned int {symbol}_name_count = {len(pairs)};")

    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{out_path}: {len(pairs)} names")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
