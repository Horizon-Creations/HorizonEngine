#!/usr/bin/env python3
"""underwater_reference_compare — how much of a floor picture is caustic line, measured the same way on
the human's reference photo and on our shots (Thema 205 step 4).

Per picture: luminance (Rec. 709 on 8-bit sRGB values) over a box, then
  cell  = 25th percentile   (the floor between the lines)
  peak  = 99th percentile   (the brightest line)
  lines = share of pixels brighter than cell + 0.5 * (peak - cell)   — how much of the floor reads as "line"
  contrast = peak / cell
No numpy: JPG/PNG go through `sips` (or PIL where there is no sips) to a BMP first, read with struct.

  python3 scripts/underwater_reference_compare.py ref.jpg:0,0,1,0.93 /tmp/uw/reference.png /tmp/uw/floor.png

`path:x0,y0,x1,y1` crops to a box in fractions of the frame (the photo has a caption bar at the bottom).
"""
import pathlib, shutil, struct, subprocess, sys, tempfile


def read_bmp(path):
    b = pathlib.Path(path).read_bytes()
    off, = struct.unpack_from("<I", b, 10)
    w, h, _, bpp = struct.unpack_from("<iiHH", b, 18)
    top_down = h < 0
    h = abs(h)
    step = bpp // 8
    stride = (w * step + 3) & ~3
    rows = []
    for y in range(h):
        r = off + (y if top_down else h - 1 - y) * stride
        rows.append(b[r:r + w * step])
    return w, h, step, rows


def load(path):
    if path.lower().endswith(".bmp"):
        return read_bmp(path)
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / "x.bmp"
        if shutil.which("sips"):
            subprocess.run(["sips", "-s", "format", "bmp", path, "--out", str(out)], capture_output=True, check=True)
        else:                                  # Windows/Linux: PIL writes the same 24-bit BMP
            from PIL import Image
            Image.open(path).convert("RGB").save(out)
        return read_bmp(out)


def measure(spec):
    path, _, box = spec.rpartition(":")
    if not path or not box[:1].isdigit():   # no box, or only a Windows drive letter ("C:/...")
        path, box = spec, ""
    x0, y0, x1, y1 = (float(v) for v in box.split(",")) if box else (0.0, 0.0, 1.0, 1.0)
    w, h, step, rows = load(path)
    lum = []
    for y in range(int(y0 * h), int(y1 * h)):
        row = rows[y]
        for x in range(int(x0 * w), int(x1 * w)):
            bb, g, r = row[x * step], row[x * step + 1], row[x * step + 2]
            lum.append(0.2126 * r + 0.7152 * g + 0.0722 * bb)
    lum.sort()
    n = len(lum)
    cell, peak = lum[n // 4], lum[min(n - 1, n * 99 // 100)]
    thr = cell + 0.5 * (peak - cell)
    lines = sum(1 for v in lum if v > thr) / n
    return dict(name=pathlib.Path(path).name, cell=round(cell, 1), peak=round(peak, 1),
                contrast=round(peak / max(cell, 1.0), 2), lines_pct=round(100 * lines, 1))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    print(f"{'picture':50s} {'cell':>6s} {'peak':>6s} {'contrast':>8s} {'lines %':>8s}")
    for spec in sys.argv[1:]:
        m = measure(spec)
        print(f"{m['name']:50s} {m['cell']:6.1f} {m['peak']:6.1f} {m['contrast']:8.2f} {m['lines_pct']:8.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
