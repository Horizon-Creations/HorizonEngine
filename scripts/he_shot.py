#!/usr/bin/env python3
"""he_shot — headless sky/scene screenshot for visual verification.

Drives the deployed HorizonEditor's headless frame-dump (HE_DUMP_* env vars,
see EditorApplication::dumpFrameHeadless) to render ONE frame off-screen and
write a PNG you can open/Read — so changes can be visually verified without a
human at the GUI.

Usage:
    scripts/he_shot.py OUT.png [KEY=VAL ...]

Each KEY=VAL becomes HE_DUMP_<KEY>. Common keys (see dumpFrameHeadless):
    TOD=0.30          time of day 0..1 (0 midnight, 0.25 sunrise, 0.5 noon)
    COVERAGE=0.6      cloud coverage 0..1
    CLOUDMODE=1       0 dome / 1 volumetric clouds
    PITCH=12 YAW=40   camera look (degrees); CAMX/CAMY/CAMZ position
    NEBULA=0.4 NEBQUALITY=2 MILKYWAY=1 AURORA=0.5 MOONPHASE=0.5
    STARSIZE=1 STARDENS=0.5 STARGLOW=1 CONTRAILS=0 CIRRUS=0
    RHI=Metal         backend to force (default Metal — the user's platform)

Examples:
    scripts/he_shot.py /tmp/a.png TOD=0.30 COVERAGE=0.7 PITCH=10
    scripts/he_shot.py /tmp/b.png TOD=0.0 NEBQUALITY=2 MILKYWAY=1 AURORA=0.6

Notes:
  * Renders at 1280x720. Forces Metal by default (HE_DUMP_RHI=Metal).
  * The editor currently segfaults on the dump-quit teardown AFTER writing the
    file — harmless; the BMP is flushed first. We validate the output and retry
    once if it's missing/short.
"""
import os, sys, subprocess, shutil, pathlib

REPO = pathlib.Path(__file__).resolve().parent.parent
EDITOR = REPO / "out" / "deploy" / "Editor" / "HorizonEditor"
TIMEOUT = 90


def run_once(out_png: str, kv: dict) -> bool:
    out_png = os.path.abspath(out_png)
    bmp = os.path.splitext(out_png)[0] + ".he.bmp"
    if os.path.exists(bmp):
        os.remove(bmp)
    env = dict(os.environ)
    env["HE_DUMP_PATH"] = bmp
    env["HE_DUMP_QUIT"] = "1"
    env["HE_DUMP_SKYTEST"] = "1"
    env.setdefault("HE_DUMP_RHI", "Metal")           # user's platform
    for k, v in kv.items():
        env[f"HE_DUMP_{k.upper()}"] = str(v)
    try:
        subprocess.run([str(EDITOR)], env=env, timeout=TIMEOUT,
                       capture_output=True)
    except subprocess.TimeoutExpired:
        print("  (editor timed out — checking for output anyway)", file=sys.stderr)
    # The dump may have crashed on teardown; the BMP is what matters.
    if not os.path.exists(bmp) or os.path.getsize(bmp) < 1024:
        return False
    r = subprocess.run(["sips", "-s", "format", "png", bmp, "--out", out_png],
                       capture_output=True)
    os.remove(bmp)
    return r.returncode == 0 and os.path.exists(out_png)


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    out_png = sys.argv[1]
    kv = {}
    for arg in sys.argv[2:]:
        if "=" not in arg:
            print(f"bad arg (need KEY=VAL): {arg}"); sys.exit(2)
        k, v = arg.split("=", 1)
        kv[k] = v
    if not EDITOR.exists():
        print(f"editor not built: {EDITOR}\n  build: cmake --build cmake-build-release --target HorizonEditor")
        sys.exit(1)
    for attempt in (1, 2):
        if run_once(out_png, kv):
            sz = os.path.getsize(out_png)
            print(f"OK  {out_png}  ({sz//1024} KB)  params={kv}")
            sys.exit(0)
        print(f"  attempt {attempt} produced no image; retrying..." if attempt == 1
              else "FAILED: no image after 2 attempts", file=sys.stderr)
    sys.exit(1)


if __name__ == "__main__":
    main()
