#!/usr/bin/env python3
"""underwater_acceptance — the Thema 185 acceptance shots and tables, one backend at a time.

Drives the deployed HorizonEditor's headless frame dump (HE_DUMP_*, see
EditorApplication::dumpFrameHeadless) on the HE_DUMP_WATERLAKE=bowl witness: a real water body
(surface at world Y 302, the floor rising toward +Z), the camera at (0, 302 - depth, 28) looking
toward +Z (yaw 180, pitch -20), sun up (TOD 0.45). Per shot: private HE_CONFIG_DIR, one frame,
BMP kept next to the log, PNG made where `sips` exists (macOS). No PIL/numpy: BMPs are read with
struct, so this runs the same on macOS, Windows and Linux.

  python3 scripts/underwater_acceptance.py --rhi Metal  --out shots/metal
  python3 scripts/underwater_acceptance.py --rhi D3D12  --out shots/d3d12 [--editor PATH] [--jobs 3]
  python3 scripts/underwater_acceptance.py --compare shots/gl shots/metal      # per shot mean|d| A vs B

What it prints (the tables of docs/underwater-acceptance-2026-10-12.md):
  1. A/B at 2 m: fog only / net only / defaults against both-off (mean |d| of 255, % pixels > 2)
  2. the waterline crossing: frame-to-frame mean |d| for camera steps from +30 cm to -30 cm
  3. the caustic net's fade-in: net-only against both-off at 0.5 .. 100 cm depth
  4. the HE_DUMP_UWFOG / HE_DUMP_UWCAUSTICS log witness (the knobs really reached the body)

Reading the numbers: a smooth transition has no step much larger than its neighbours; the 1 cm step
over the line is the one to look at. Metal and OpenGL agree to ~0.1 mean |d| under water (2026-10-12);
a backend that is far off there is missing a stage or reconstructs the depth wrongly.
"""
import argparse, concurrent.futures as cf, os, pathlib, shutil, struct, subprocess, sys, tempfile, time

REPO = pathlib.Path(__file__).resolve().parent.parent
SURFACE = 302.0
COMMON = dict(TOD="0.45", COVERAGE="0.2", CLOUDMODE="0", AA="0", BLOOM="0", WATERLAKE="bowl",
              CAMX="0", CAMZ="28", YAW="180", PITCH="-20")
CLEAR_FOG = "0.06"                    # clear water, so the floor (and the net on it) shows


def cam(dy):                          # dy = camera height relative to the surface (< 0 = under water)
    return {"CAMY": f"{SURFACE + dy:.3f}"}


def shot_list():
    s = {}
    for d in (0.4, 1.0, 2.0, 4.0):
        s[f"depth_{d:.1f}m_clear"] = {"UWFOG": CLEAR_FOG, **cam(-d)}
    s["ab_fogonly"] = {"UWFOG": CLEAR_FOG, "UWCAUSTICS": "0", **cam(-2.0)}
    s["ab_netonly"] = {"UWFOG": "0", **cam(-2.0)}
    s["ab_bothoff"] = {"UWFOG": "0", "UWCAUSTICS": "0", **cam(-2.0)}
    s["ab_defaults"] = {**cam(-2.0)}
    for dy in (0.30, 0.10, 0.03, 0.005, -0.005, -0.03, -0.10, -0.30):
        s[f"cross_{'above' if dy > 0 else 'below'}_{abs(dy) * 100:05.1f}cm"] = {**cam(dy)}
    s["dry_3m_above"] = {**cam(3.0)}
    for d in (0.005, 0.10, 0.25, 0.50, 1.00):
        s[f"fade_net_{d * 100:05.1f}cm"] = {"UWFOG": "0", **cam(-d)}
        s[f"fade_off_{d * 100:05.1f}cm"] = {"UWFOG": "0", "UWCAUSTICS": "0", **cam(-d)}
    return s


# ── BMP (32-bit BGRA or 24-bit, top-down or bottom-up) ───────────────────────────────────────
def read_bmp(path):
    d = pathlib.Path(path).read_bytes()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    if d[:2] != b"BM" or bpp not in (24, 32):
        raise ValueError(f"{path}: not a 24/32-bit BMP")
    px = bpp // 8
    stride = (w * px + 3) & ~3
    bottom_up = h > 0
    h = abs(h)
    rows = []
    for y in range(h):
        sy = (h - 1 - y) if bottom_up else y
        rows.append(d[off + sy * stride: off + sy * stride + w * px])
    return w, h, px, rows


def diff(a, b, thr=2):
    w, h, px, ra = a
    rb = b[3]
    tot = mx = cnt = 0
    for y in range(h):
        x, z = ra[y], rb[y]
        if x == z:
            continue
        for i in range(0, w * px, px):
            m = max(abs(x[i] - z[i]), abs(x[i + 1] - z[i + 1]), abs(x[i + 2] - z[i + 2]))
            if m:
                tot += m
                cnt += m > thr
                mx = max(mx, m)
    return tot / (w * h), mx, 100.0 * cnt / (w * h)


# ── running the editor ───────────────────────────────────────────────────────────────────────
def shoot(editor, out_dir, name, rhi, kv, renderpath, timeout):
    cfg = tempfile.mkdtemp(prefix="uwcfg_")
    bmp = out_dir / f"{name}.bmp"
    if bmp.exists():
        bmp.unlink()
    env = dict(os.environ)
    env.update(HE_CONFIG_DIR=cfg, HE_DUMP_PATH=str(bmp), HE_DUMP_QUIT="1", HE_DUMP_SKYTEST="1",
               HE_DUMP_RHI=rhi, HE_SKY_TIME="5", HE_DUMP_RENDERPATH=str(renderpath))
    for k, v in {**COMMON, **kv}.items():
        env["HE_DUMP_" + k] = str(v)
    t0 = time.time()
    with open(out_dir / f"{name}.log", "wb") as lf:
        try:
            subprocess.run([str(editor)], env=env, stdout=lf, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            pass                                        # the BMP may be there anyway
    shutil.rmtree(cfg, ignore_errors=True)
    ok = bmp.exists() and bmp.stat().st_size > 1024
    if ok and shutil.which("sips"):
        subprocess.run(["sips", "-s", "format", "png", str(bmp), "--out", str(bmp.with_suffix(".png"))],
                       capture_output=True)
    print(("OK    " if ok else "FAILED"), name, f"({time.time() - t0:.0f}s)", flush=True)
    return ok


def tables(out_dir):
    def load(n):
        p = out_dir / f"{n}.bmp"
        return read_bmp(p) if p.exists() else None

    print("\n1. A/B at 2 m under water, against both-off (mean |d| of 255, max, % pixels > 2)")
    off = load("ab_bothoff")
    for n in ("ab_fogonly", "ab_netonly", "ab_defaults"):
        im = load(n)
        if off and im:
            md, mx, pct = diff(im, off)
            print(f"   {n:14s} mean|d|={md:8.3f}  max={mx:3d}  >2: {pct:6.2f}%")

    print("\n2. Waterline crossing, default knobs: frame-to-frame (camera steps in brackets)")
    names = ["cross_above_030.0cm", "cross_above_010.0cm", "cross_above_003.0cm", "cross_above_000.5cm",
             "cross_below_000.5cm", "cross_below_003.0cm", "cross_below_010.0cm", "cross_below_030.0cm"]
    steps = ["20 cm", "7 cm", "2.5 cm", "1 cm = THE LINE", "2.5 cm", "7 cm", "20 cm"]
    ims = [load(n) for n in names]
    for i in range(len(names) - 1):
        if ims[i] and ims[i + 1]:
            md, mx, pct = diff(ims[i], ims[i + 1])
            print(f"   {names[i]:20s} -> {names[i + 1]:20s} [{steps[i]:>15s}] mean|d|={md:7.3f} max={mx:3d} >2: {pct:6.2f}%")

    print("\n3. The net's fade-in: net-only against both-off at the same camera")
    for d in (0.005, 0.10, 0.25, 0.50, 1.00):
        a, b = load(f"fade_net_{d * 100:05.1f}cm"), load(f"fade_off_{d * 100:05.1f}cm")
        if a and b:
            md, mx, pct = diff(a, b)
            print(f"   depth {d * 100:6.1f} cm: mean|d|={md:6.3f} max={mx:3d} >2: {pct:5.2f}%")

    print("\n4. Log witness (the knobs reached the body)")
    lg = out_dir / "ab_fogonly.log"
    if lg.exists():
        for line in lg.read_text(errors="replace").splitlines():
            if "HE_DUMP_UWFOG" in line:
                print("   " + line.strip()[-200:])
                break


def compare(a_dir, b_dir):
    print(f"{'shot':26s} mean|d|   max  >2%      ({a_dir} vs {b_dir})")
    for p in sorted(pathlib.Path(a_dir).glob("*.bmp")):
        q = pathlib.Path(b_dir) / p.name
        if q.exists():
            md, mx, pct = diff(read_bmp(p), read_bmp(q))
            print(f"{p.stem:26s} {md:7.3f} {mx:5d} {pct:6.2f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rhi", help="OpenGL | Metal | D3D11 | D3D12 | Vulkan (HE_DUMP_RHI)")
    ap.add_argument("--out", help="output directory")
    ap.add_argument("--editor", default=str(REPO / "out" / "deploy" / "Editor" /
                                              ("HorizonEditor.exe" if os.name == "nt" else "HorizonEditor")))
    ap.add_argument("--renderpath", default="1", help="0 forward, 1 deferred (default)")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--only", default="", help="comma-separated substrings of shot names")
    ap.add_argument("--tables-only", action="store_true", help="skip the shooting, just print the tables")
    ap.add_argument("--compare", nargs=2, metavar=("DIR_A", "DIR_B"))
    a = ap.parse_args()
    if a.compare:
        compare(*a.compare)
        return 0
    if not a.rhi or not a.out:
        ap.error("--rhi and --out are required")
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    if not a.tables_only:
        todo = {k: v for k, v in shot_list().items()
                if not a.only or any(p in k for p in a.only.split(","))}
        with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            res = list(ex.map(lambda kv: shoot(a.editor, out, kv[0], a.rhi, kv[1], a.renderpath, a.timeout),
                              todo.items()))
        if not all(res):
            print("some shots failed — see the .log files", file=sys.stderr)
    tables(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
