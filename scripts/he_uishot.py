#!/usr/bin/env python3
"""he_uishot — screenshots of the editor's UI, without a GPU or a window.

The engine's frame dump (scripts/he_shot.py) renders the SCENE. The editor's
chrome — panels, tooltips, the documentation reader — is Dear ImGui, and none of
it appears there, so until now it was the one part of the product that could
only be checked by a person sitting in front of it.

This drives the other half: he_tests contains a software rasteriser for ImGui's
draw data (tests/ImGuiSoftwareRaster.h) and a set of UI scenes
(tests/test_ui_shot.cpp). Run them with a dump directory and every scene is
written out, then converted to PNG here.

    scripts/he_uishot.py [OUTDIR] [--tests PATH] [--filter "ui shot*"]

Defaults: OUTDIR = build-wt/uishots, the test binary is looked for in the usual
build directories. Prints the files it produced.

To add a scene, add a TEST_CASE to tests/test_ui_shot.cpp — anything that can be
drawn with ImGui can be shot, including a single widget.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Where a test binary usually ends up. build-wt is the worktree build, the other
# two are what CLion and a plain `cmake -B build` produce.
CANDIDATES = [
    REPO / "build-wt" / "tests" / "he_tests",
    REPO / "cmake-build-release" / "tests" / "he_tests",
    REPO / "build" / "tests" / "he_tests",
]


def find_tests(explicit: str | None) -> Path:
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            sys.exit(f"error: no test binary at {p}")
        return p
    for c in CANDIDATES:
        if c.is_file():
            return c
    sys.exit("error: he_tests not found — build it, or pass --tests PATH")


def to_png(out_dir: Path) -> list[Path]:
    """Convert the dumped BMPs. The rasteriser writes BMP because the engine
    ships no PNG encoder; every viewer reads PNG, so convert here."""
    bmps = sorted(out_dir.glob("*.bmp"))
    if not bmps:
        return []
    try:
        from PIL import Image
    except ImportError:
        print("note: Pillow not installed — leaving the BMPs as they are")
        return bmps
    out = []
    for bmp in bmps:
        png = bmp.with_suffix(".png")
        with Image.open(bmp) as im:
            im.save(png)
        bmp.unlink()
        out.append(png)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("outdir", nargs="?", default=str(REPO / "build-wt" / "uishots"))
    ap.add_argument("--tests", default=None, help="path to the he_tests binary")
    ap.add_argument("--filter", default="ui shot*",
                    help='doctest test-case filter (default: "ui shot*")')
    args = ap.parse_args()

    tests = find_tests(args.tests)
    out_dir = Path(args.outdir)
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in out_dir.glob("*.bmp"):
        stale.unlink()

    env = dict(os.environ, HE_UI_DUMP_DIR=str(out_dir))
    print(f"running {tests} -tc=\"{args.filter}\"")
    result = subprocess.run([str(tests), f"-tc={args.filter}"], env=env)

    files = to_png(out_dir)
    for f in files:
        print(f"  {f}")
    if not files:
        print("no shots were produced — did the filter match any scene?")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
