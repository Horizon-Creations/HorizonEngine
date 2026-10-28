#!/usr/bin/env python3
"""water_backend_diff — numeric A/B for two water_baseline_shots PNGs (Thema 206 step 5).

Mean|d| / max|d| / %px>2 over RGB, like underwater_acceptance.py's diff() but on PNG via
PIL instead of raw BMP (water_baseline_shots writes PNG on every platform). Can exclude a
top row band (the sky, which drifts on SDL_GetTicks on D3D11 independent of water changes).

Usage: scripts/water_backend_diff.py A.png B.png [--skip-top 220]
"""
import argparse, sys
from PIL import Image
import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--skip-top", type=int, default=0, help="ignore this many rows from the top (sky band)")
    a = ap.parse_args()
    ia = np.asarray(Image.open(a.a).convert("RGB"), dtype=np.int16)
    ib = np.asarray(Image.open(a.b).convert("RGB"), dtype=np.int16)
    if ia.shape != ib.shape:
        print(f"SIZE MISMATCH {a.a} {ia.shape} vs {a.b} {ib.shape}")
        return 1
    if a.skip_top:
        ia, ib = ia[a.skip_top:], ib[a.skip_top:]
    d = np.abs(ia - ib).max(axis=2)
    mean = float(d.mean())
    mx = int(d.max())
    pct = float((d > 2).mean() * 100.0)
    print(f"{a.a} vs {a.b}: mean|d|={mean:7.3f} max={mx:3d} >2: {pct:6.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
