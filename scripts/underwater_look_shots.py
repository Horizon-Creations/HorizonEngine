#!/usr/bin/env python3
"""underwater_look_shots — the Thema 205 inventory views: why the underwater look reads cheap.

Same witness and headless dump as scripts/underwater_acceptance.py (Thema 185): HE_DUMP_WATERLAKE=bowl,
surface at world Y 302, floor 0.004 r^2 - 6 below the landscape at Y 300 (8 m deep in the middle, shore at
r = 44.7 m), TOD 0.45, AA/bloom off, 1280x720. The acceptance script measures the pass; this one frames
what a player sees: the underside of the surface from below, the horizon, the floor, the shore, and the
shallow water seen from above (where the caustic net never runs, the camera is dry). Every view also with
the fog off (UWFOG=0), so the net and the underside show without the tint over them.

  python3 scripts/underwater_look_shots.py --editor out/deploy/Editor/HorizonEditor --out /tmp/uwlook --rhi Metal

Writes <name>.bmp/.png/.log per shot and contact.txt with the camera of each.
"""
import argparse, os, pathlib, shutil, subprocess, sys, tempfile, time

COMMON = dict(TOD="0.45", COVERAGE="0.2", CLOUDMODE="0", AA="0", BLOOM="0", WATERLAKE="bowl")

# name: (camx, camy, camz, yaw, pitch)   yaw 180 looks toward +Z, where the bowl's floor rises to the shore
VIEWS = {
    "up":      (0.0, 300.0, 20.0, 180.0,  35.0),   # 2 m deep, looking up at the underside (Snell's window?)
    "horizon": (0.0, 300.0, 20.0, 180.0,   0.0),   # 2 m deep, level: the fog's look toward the far wall
    "floor":   (0.0, 300.0, 28.0, 180.0, -20.0),   # the Thema 185 acceptance framing (floor 4.9 m below)
    "shore":   (0.0, 301.0, 36.0, 180.0, -12.0),   # 1 m deep, toward the shore: floor 2.8 m -> 0 m
    "dry_shallow": (0.0, 305.0, 30.0, 180.0, -40.0),  # 3 m above the surface, looking down into the shallows
    # Thema 205 step 2: from above the 8 m deep middle out toward the shore, floor 7 m -> 2 m deep in frame.
    # With the fog off this is the witness for the depth fade (faint over the deep middle, full in the shallows).
    "deep_to_shore": (0.0, 301.0, 4.0, 180.0, -22.0),
    # Thema 205 step 3 (the underside): straight up from 2 m, the whole Snell's window in frame; and up
    # from 5 m deep (floor 294.4), where the fog must cover the rim and the mirror beyond it.
    "zenith":  (0.0, 300.0, 20.0, 180.0,  85.0),
    "deep_up": (0.0, 297.0, 10.0, 180.0,  50.0),
}


def shoot(editor, out_dir, name, rhi, kv, timeout=180):
    cfg = tempfile.mkdtemp(prefix="uwlook_")
    bmp = out_dir / f"{name}.bmp"
    if bmp.exists():
        bmp.unlink()
    env = dict(os.environ)
    env.update(HE_CONFIG_DIR=cfg, HE_DUMP_PATH=str(bmp), HE_DUMP_QUIT="1", HE_DUMP_SKYTEST="1",
               HE_DUMP_RHI=rhi, HE_SKY_TIME="5", HE_DUMP_RENDERPATH="1", HE_COLLAB_OFFLINE="1")
    for k, v in {**COMMON, **kv}.items():
        env["HE_DUMP_" + k] = str(v)
    t0 = time.time()
    with open(out_dir / f"{name}.log", "wb") as lf:
        proc = subprocess.Popen([str(editor)], env=env, stdout=lf, stderr=subprocess.STDOUT,
                                cwd=str(pathlib.Path(editor).parent))
        try:
            rc = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            rc = "timeout"
    shutil.rmtree(cfg, ignore_errors=True)
    ok = bmp.exists() and bmp.stat().st_size > 1024
    if ok and shutil.which("sips"):
        subprocess.run(["sips", "-s", "format", "png", str(bmp), "--out", str(bmp.with_suffix(".png"))],
                       capture_output=True)
    print(("OK    " if ok else "FAILED"), name, f"({time.time() - t0:.0f}s, exit {rc})", flush=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--editor", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rhi", default="Metal")
    ap.add_argument("--only", help="comma list of view names")
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    names = a.only.split(",") if a.only else list(VIEWS)
    lines, bad = [], 0
    for n in names:
        x, y, z, yaw, pitch = VIEWS[n]
        cam = dict(CAMX=x, CAMY=y, CAMZ=z, YAW=yaw, PITCH=pitch)
        for suffix, kv in (("", {}), ("_nofog", {"UWFOG": "0"})):
            bad += not shoot(a.editor, out, n + suffix, a.rhi, {**cam, **kv})
            lines.append(f"{n + suffix}: cam ({x}, {y}, {z}) yaw {yaw} pitch {pitch} {kv or 'defaults'}")
    (out / "contact.txt").write_text("\n".join(lines) + "\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
