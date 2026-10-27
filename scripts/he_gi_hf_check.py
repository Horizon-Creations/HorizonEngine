#!/usr/bin/env python3
"""Pixel A/B of the GI landscape height-field shadow path (Windows, D3D11, no window).

Renders one frame of the HE_DUMP_GIHFTEST witness landscape (hills + crates) through the headless frame dump
(HE_DUMP_PATH, the window is never shown) once per variant and diffs the pictures:

  ref  : HE_GI_HEIGHTFIELD=0, HE_GI_CASTER_RANGE=0  - terrain chunks traced as meshes, nothing thinned
  hf   : HE_GI_HEIGHTFIELD=1, HE_GI_CASTER_RANGE=0  - chunks answered by the height field
  neg  : HE_GI_HEIGHTFIELD=0, HE_GI_CASTER_RANGE=5  - negative control: far casters dropped, shadows must change

  python scripts/he_gi_hf_check.py --project <x.heproj> --editor <deploy>/Editor/HorizonEditor.exe \
      --out <dir> --cam x,y,z,yawDeg,pitchDeg [--cam ...] [--set Key=Value ...]

Prints, per camera: mean |diff| and the share of pixels differing by more than 8/255 for hf-vs-ref and
neg-vs-ref (the second must be clearly larger than the first, else the picture does not show shadows at all),
writes PNGs of the three variants and of the hf-vs-ref difference (x8).
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from he_perf_capture import write_config  # noqa: E402


def read_bmp(path):
    b = Path(path).read_bytes()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h, _, bpp = struct.unpack_from("<iiHH", b, 18)
    top_down = h < 0
    h = abs(h)
    bpp //= 8
    stride = (w * bpp + 3) & ~3
    rows = []
    for y in range(h):
        r = b[off + y * stride: off + y * stride + w * bpp]
        rows.append(r)
    if not top_down:
        rows.reverse()
    px = bytearray()
    for r in rows:
        for x in range(w):
            px += bytes((r[x * bpp + 2], r[x * bpp + 1], r[x * bpp]))   # BGR(A) -> RGB
    return w, h, bytes(px)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def diff(a, b, skip_top=0.0, width=1280):
    """Mean |d| over the bytes from skip_top (fraction of the height) down: the animated sky differs run to run."""
    start = int(len(a) / (width * 3) * skip_top) * width * 3
    a, b = a[start:], b[start:]
    n = len(a)
    tot = big = 0
    out = bytearray(n)
    for i in range(0, n, 3):
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        tot += d
        big += d > 8
        v = min(255, d * 8)
        out[i] = out[i + 1] = out[i + 2] = v
    return tot / (n / 3), 100.0 * big / (n / 3), bytes(out)


def run(editor, project, cfgdir, out_bmp, env_extra, sets, cam, frames, rhi):
    write_config(Path(cfgdir), project, sets, None, rhi)
    x, y, z, yaw, pitch = (float(v) for v in cam.split(","))
    env = dict(os.environ)
    env.update({"HE_DUMP_GIHFTEST": "1", "HE_DUMP_SKYTEST": "1", "HE_DUMP_TOD": os.environ.get("HE_DUMP_TOD", "0.3"),
                "HE_DUMP_CAMX": str(x), "HE_DUMP_CAMY": str(y), "HE_DUMP_CAMZ": str(z),
                "HE_DUMP_YAW": str(yaw), "HE_DUMP_PITCH": str(pitch)})
    env.update({"HE_CONFIG_DIR": str(cfgdir), "HE_DUMP_PATH": str(out_bmp), "HE_DUMP_QUIT": "1",
                "HE_DUMP_RHI": rhi, "HE_DUMP_FRAMES": str(frames), "HE_DUMP_GI": "1",
                "HE_COLLAB_OFFLINE": "1", "HE_NET_LOOPBACK_ONLY": "1",
                "HE_DUMP_GIREFL": "0"})   # reflections on would switch the caster-range cut (the negative control) off
    env.update(env_extra)
    if Path(out_bmp).exists():
        Path(out_bmp).unlink()
    log = editor.parent / "HorizonEngine.log"
    if log.exists():
        log.unlink()
    proc = subprocess.Popen([str(editor)], cwd=str(editor.parent), env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < 240:
        if Path(out_bmp).exists() and proc.poll() is not None:
            break
        if Path(out_bmp).exists() and time.time() - t0 > 20 and log.exists() and "frame dumped" in log.read_text(errors="ignore"):
            time.sleep(3)
            break
        time.sleep(1)
    if proc.poll() is None:
        proc.kill()
    txt = log.read_text(errors="ignore") if log.exists() else ""
    return txt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True)
    ap.add_argument("--editor", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cam", action="append", required=True)
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--rhi", default="D3D11", choices=["D3D11", "D3D12", "Vulkan", "OpenGL"])
    a = ap.parse_args()
    editor = Path(a.editor).resolve()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    cfg = out / "cfg"
    variants = {
        "ref": {"HE_GI_HEIGHTFIELD": "0", "HE_GI_CASTER_RANGE": "0"},
        "hf": {"HE_GI_HEIGHTFIELD": "1", "HE_GI_CASTER_RANGE": "0"},
        "neg": {"HE_GI_HEIGHTFIELD": "0", "HE_GI_CASTER_RANGE": "5"},
        "mute": {"HE_GI_HEIGHTFIELD": "1", "HE_GI_CASTER_RANGE": "0", "HE_GI_HEIGHTFIELD_MUTE": "1"},   # OpenGL only: field uploaded, chunks out, field not marched
    }
    sets = ["GlobalIlluminationEnabled=true"] + a.set
    for ci, cam in enumerate(a.cam):
        px = {}
        for name, env in variants.items():
            bmp = out / f"cam{ci}_{name}.bmp"
            log = run(editor, Path(a.project).resolve(), cfg, bmp, env, sets, cam, a.frames, a.rhi)
            if not bmp.exists():
                print(f"cam{ci} {name}: no frame written")
                sys.exit(3)
            w, h, rgb = read_bmp(bmp)
            px[name] = rgb
            write_png(out / f"cam{ci}_{name}.png", w, h, rgb)
            engaged = "GI height field" in log
            print(f"cam{ci} {name}: {w}x{h}, height field uploaded: {engaged}")
        m1, p1, dimg = diff(px["hf"], px["ref"], 0.5, w)
        m2, p2, _ = diff(px["neg"], px["ref"], 0.5, w)
        m3, p3, _ = diff(px["mute"], px["ref"], 0.5, w)
        print(f"cam{ci}: muted field vs ref  mean|d| {m3:.3f}  >8: {p3:.3f} %  (OpenGL negative control: terrain shadows must vanish)")
        write_png(out / f"cam{ci}_diff_hf_ref_x8.png", w, len(dimg) // (w * 3), dimg)
        print(f"cam{ci}: hf vs ref (lower half) mean|d| {m1:.3f}  >8: {p1:.3f} %   |   neg vs ref  mean|d| {m2:.3f}  >8: {p2:.3f} %")


if __name__ == "__main__":
    main()
