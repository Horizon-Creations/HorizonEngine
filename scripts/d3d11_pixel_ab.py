#!/usr/bin/env python3
"""d3d11_pixel_ab - headless D3D11 witness frames for a before/after pixel comparison.

Runs the deployed editor of ONE deploy dir through a fixed list of HE_DUMP_* witness scenes
(graph materials forward + deferred, landscape weightmap, transparent water, SSAO/GI/shadow
pre-passes, clustered lights, UI, TAA, bloom) on D3D11 and writes <out>/<label>-<scene>.bmp.
Every capture gets a scratch APPDATA and a hidden window; the process is stopped by PID.

  python scripts/d3d11_pixel_ab.py capture --editor C:/hd11base/Editor/HorizonEditor.exe --out C:/ab --label A --repeat 2
  python scripts/d3d11_pixel_ab.py capture --editor C:/hd11b/deploy/Editor/HorizonEditor.exe --out C:/ab --label B --repeat 2
  python scripts/d3d11_pixel_ab.py compare --out C:/ab A B

compare prints, per scene, the differing-pixel count and max channel delta of A0 vs A1 (the
noise floor: the animated sky) and of A0 vs B0, over the whole frame and over the lower
three quarters (no sky band).
"""
import argparse, os, pathlib, shutil, subprocess, sys, tempfile, time

SKY = dict(SKYTEST="1", NOSKY="1", TOD="0.4", RENDERPATH="0", AA="0", BLOOM="0")
MAT = dict(MATERIALTEST="chrome", MATTESTPOS="-3.5,2.5,-8", SSRTEST="1", CAMY="3.5", CAMZ="6", PITCH="-12")
SCENES = {
    "mat-forward":  {**SKY, **MAT},
    "mat-deferred": {**SKY, **MAT, "RENDERPATH": "1"},
    "mat-matte-bloom-fxaa": {**SKY, **MAT, "MATERIALTEST": "matte", "BLOOM": "1", "AA": "1"},
    "landscape-gi": {**SKY, "LANDSCAPELAYERS": "1", "GI": "1", "CAMY": "392", "CAMZ": "0", "PITCH": "-89"},
    "water-shore":  {**SKY, "WATERLAKE": "bowl", "CAMY": "375", "CAMZ": "48", "PITCH": "-60"},
    "shadowinst-ssao": {**SKY, "SHADOWINSTTEST": "1", "SSAO": "1", "CAMY": "3.5", "CAMZ": "3"},
    "shadowinst-gi":   {**SKY, "SHADOWINSTTEST": "1", "GI": "1", "CAMY": "3.5", "CAMZ": "3"},
    "manylights":   {**SKY, "MANYLIGHTS": "16", "TOD": "0", "CAMY": "207", "CAMZ": "2", "PITCH": "-38",
                     "COVERAGE": "0", "CLOUDMODE": "0"},
    "taa-motion":   {**SKY, "SHADOWINSTTEST": "1", "AA": "3", "MBYAWSTEP": "0.3", "CAMY": "3.5", "CAMZ": "3",
                     "FRAMES": "24"},
    "ui":           dict(UITEST="1"),
}


def run_one(editor, scene, out_bmp, timeout=120):
    env = {k: v for k, v in os.environ.items() if not k.startswith("HE_DUMP_")}
    appdata = tempfile.mkdtemp(prefix="heab_")
    env.update(APPDATA=appdata, HE_HIDDEN_WINDOW="1", HE_BACKGROUND_FPS="0", HE_COLLAB_OFFLINE="1",
               HE_NET_LOOPBACK_ONLY="1", HE_CONFIG_DIR=appdata, HE_DUMP_PATH=str(out_bmp), HE_DUMP_QUIT="1",
               HE_DUMP_RHI="D3D11", HE_DUMP_FRAMES="16", HE_SKY_TIME="6.2832")
    for k, v in scene.items():
        env["HE_DUMP_" + k] = v
    if out_bmp.exists():
        out_bmp.unlink()
    p = subprocess.Popen([str(editor)], env=env, cwd=str(pathlib.Path(editor).parent),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0, last, stable = time.time(), -1, 0
    while time.time() - t0 < timeout:
        time.sleep(0.5)
        if out_bmp.exists():
            sz = out_bmp.stat().st_size
            stable = stable + 1 if sz == last and sz > 0 else 0
            last = sz
            if stable >= 3:
                break
        if p.poll() is not None:
            break
    if p.poll() is None:
        subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"], capture_output=True)
    shutil.rmtree(appdata, ignore_errors=True)
    return out_bmp.exists() and out_bmp.stat().st_size > 0


def capture(a):
    out = pathlib.Path(a.out); out.mkdir(parents=True, exist_ok=True)
    names = a.scenes.split(",") if a.scenes else list(SCENES)
    for n in names:
        for r in range(a.repeat):
            f = out / f"{a.label}{r}-{n}.bmp"
            ok = run_one(a.editor, SCENES[n], f)
            print(f"{'ok ' if ok else 'FAIL'} {f.name}", flush=True)


def read_bmp(path):
    b = path.read_bytes()
    off = int.from_bytes(b[10:14], "little")
    w = int.from_bytes(b[18:22], "little", signed=True)
    h = int.from_bytes(b[22:26], "little", signed=True)
    bpp = int.from_bytes(b[28:30], "little")
    return b[off:], w, abs(h), bpp // 8


def diff(p, q):
    import numpy as np
    A, w, h, bp = read_bmp(p); B, w2, h2, _ = read_bmp(q)
    if (w, h) != (w2, h2):
        return None
    a = np.frombuffer(A, np.uint8)[: w * h * bp].reshape(h, w, bp).astype(int)
    b = np.frombuffer(B, np.uint8)[: w * h * bp].reshape(h, w, bp).astype(int)
    d = np.abs(a - b).max(axis=2)
    # BMP rows are bottom-up: the sky (top of the picture) is the END of the pixel array
    low = d[: (h * 3) // 4]
    return dict(all_px=int((d > 0).sum()), all_max=int(d.max()),
                low_px=int((low > 0).sum()), low_max=int(low.max()), size=(w, h))


def compare(a):
    out = pathlib.Path(a.out)
    for n in SCENES:
        fa0, fa1 = out / f"{a.a}0-{n}.bmp", out / f"{a.a}1-{n}.bmp"
        fb0 = out / f"{a.b}0-{n}.bmp"
        if not (fa0.exists() and fb0.exists()):
            print(f"{n:24s} missing"); continue
        noise = diff(fa0, fa1) if fa1.exists() else None
        ab = diff(fa0, fb0)
        fmt = lambda d: "n/a" if d is None else f"all {d['all_px']:>7d}px max{d['all_max']:>3d} | lower3/4 {d['low_px']:>7d}px max{d['low_max']:>3d}"
        print(f"{n:24s} noise[{fmt(noise)}]  A-vs-B[{fmt(ab)}]")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("--editor", required=True); c.add_argument("--out", required=True)
    c.add_argument("--label", required=True); c.add_argument("--repeat", type=int, default=1)
    c.add_argument("--scenes", default="")
    d = sub.add_parser("compare")
    d.add_argument("--out", required=True); d.add_argument("a"); d.add_argument("b")
    a = ap.parse_args()
    capture(a) if a.cmd == "capture" else compare(a)
