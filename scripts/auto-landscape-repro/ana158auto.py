"""Thema 158, Schritt 5: evaluate auto landscape witness captures (cap158auto.sh / .ps1).

    python3 ana158auto.py masks  A.bmp [B.bmp ...]   region oracle of =masks / =ground
    python3 ana158auto.py diff   REF.bmp B.bmp ...   backend comparison against REF
    python3 ana158auto.py repeat A.bmp [B.bmp ...]   tiling repetition on the plain
    python3 ana158auto.py shadow ON.bmp OFF.bmp [ON2.bmp OFF2.bmp ...]
                                                     cast shadow at the ramp foot (Schritt 6)

The HE_DUMP_AUTOLAND terrain is analytic (EditorApplication.cpp): 128 m at
y=300, constant along Z, height h(x) = 40 * smoothstep(-24, 8, x) -- a flat
plain, a ramp up to ~62 degrees and back, a 40 m plateau. The default capture
looks straight down from (0, 400, 0) with a 60 degree vertical FOV at 1280x720,
so a world column x at height h lands on screen column

    u(x) = 0.5 + x / (400 - 300 - h(x)) / (2 tan 30deg * 1280/720)

and every region below is a known x range, not something read off the image:

    plain    x -60..-30   flat, y=300        grass/dirt, puddles, no rock, no snow
    foot     x -23..-21   slope ~20..27 deg  the dirt belt below the rock
    rock     x -16..-9    slope ~55..62 deg  rock, below the snow line
    plateau  x  16..60    flat, y=340        snow (snow line y=320)

masks: per region the mean of R/G/B (0..255) and the share of pixels > 128.
  =masks  R = rock, G = snow, B = standing water
  =ground R = dirt, G = wet ground, B = flat (no snow)
  Expectations are checked and printed as ok/FAIL.
diff: mean|d| and share of pixels |d| > 8 over the terrain columns, per region
  and overall (tolerance of docs §7.5: mean|d| <= 1.0, <= 0.5 % > 8).
repeat: on the plain, mean|I(p) - I(p + k)| for horizontal and vertical shifts
  k = 3..40 px. A tiling texture dips at every multiple of its period (1 m
  checker cell ~6 px, 2 m tile ~12.5 px here); bombing fills the dips. Printed:
  the local minima and the oscillation of the curve (mean distance of D(k) from
  its neighbours' mean) -- near 0 = no visible repetition.
shadow: pairs of captures of ONE backend, the default shadow distance first,
  HE_DUMP_SHADOW=0.1 second. At TOD 0.4 the sun stands at +x, 54 degrees up in
  the x/y plane (RenderExtractor: a = (tod - 0.25) * 2 pi, toward
  (cos a, sin a, 0.45); the terrain is constant along Z, so only x/y counts).
  The ramp is steeper than that (up to 62 degrees), so the concave foot below
  it lies in the ramp's CAST shadow although it faces the sun -- computed here
  by marching the sun ray over h(x). Printed: the zone, the luminance inside it
  (1 m in from each edge, clear of the penumbra) with and without shadow, and
  the plain/plateau change. Shadow present = inside darkens to < 0.7 of the
  shadowless value while plain and plateau stay within mean|d| 1.0.
  Override the time of day with HE_TOD (default 0.4).

No third-party modules -- BMP read with struct.
"""
import math
import os
import struct
import sys

W_EXPECT, H_EXPECT = 1280, 720
CAM_Y, BASE_Y, PLATEAU = 400.0, 300.0, 40.0
TAN = math.tan(math.radians(30.0))
REGIONS = [("plain", -60.0, -30.0), ("foot", -23.0, -21.0), ("rock", -16.0, -9.0), ("plateau", 16.0, 60.0)]
ROWS = (40, 680)


def read_bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    step = bpp // 8
    stride = (w * step + 3) & ~3
    flip = h > 0
    h = abs(h)
    px = [None] * (w * h)
    for y in range(h):
        row = (h - 1 - y) if flip else y
        base = off + row * stride
        for x in range(w):
            i = base + x * step
            px[y * w + x] = (d[i + 2], d[i + 1], d[i])
    return w, h, px


def height(x):
    t = min(max((x + 24.0) / 32.0, 0.0), 1.0)
    return PLATEAU * t * t * (3.0 - 2.0 * t)


def column(x, w, h):
    d = CAM_Y - BASE_Y - height(x)
    u = 0.5 + (x / d) / (2.0 * TAN * w / h)
    return int(round(u * w))


def region_cols(w, h, x0, x1):
    c0, c1 = column(x0, w, h), column(x1, w, h)
    return max(0, min(c0, w - 1)), max(0, min(c1, w - 1))


def region_stats(img, x0, x1):
    w, h, px = img
    c0, c1 = region_cols(w, h, x0, x1)
    s = [0.0, 0.0, 0.0]
    hi = [0, 0, 0]
    n = 0
    for y in range(ROWS[0], ROWS[1]):
        for x in range(c0, c1 + 1):
            p = px[y * w + x]
            for k in range(3):
                s[k] += p[k]
                hi[k] += p[k] > 128
            n += 1
    return [v / n for v in s], [v / n for v in hi], (c0, c1)


# (region, channel, test, threshold) — share of pixels > 128 for "high"/"low",
# share range for "some".
EXPECT = {
    "masks": [
        ("plain", 0, "low", 0.02), ("plain", 1, "low", 0.0), ("plain", 2, "some", (0.02, 0.6)),
        ("rock", 0, "high", 0.95), ("rock", 1, "low", 0.0), ("rock", 2, "low", 0.0),
        ("plateau", 0, "low", 0.02), ("plateau", 1, "high", 0.95), ("plateau", 2, "low", 0.0),
    ],
    "ground": [
        ("plain", 0, "some", (0.05, 0.8)), ("plain", 1, "some", (0.02, 0.8)), ("plain", 2, "high", 0.98),
        ("foot", 0, "high", 0.8),
        ("plateau", 1, "low", 0.0), ("plateau", 2, "low", 0.0),
    ],
}


def cmd_masks(paths):
    for path in paths:
        img = read_bmp(path)
        kind = "ground" if "ground" in path else "masks"
        names = ("dirt", "wet", "flat") if kind == "ground" else ("rock", "snow", "water")
        print(f"{path}  ({kind}: R={names[0]} G={names[1]} B={names[2]})")
        stats = {}
        for name, x0, x1 in REGIONS:
            mean, hi, cols = region_stats(img, x0, x1)
            stats[name] = hi
            print(f"  {name:8s} x {x0:6.1f}..{x1:6.1f} cols {cols[0]:4d}..{cols[1]:4d}  "
                  f"mean {mean[0]:6.1f} {mean[1]:6.1f} {mean[2]:6.1f}   >128 {hi[0]:6.1%} {hi[1]:6.1%} {hi[2]:6.1%}")
        bad = 0
        for region, ch, test, thr in EXPECT[kind]:
            v = stats[region][ch]
            ok = (v <= thr) if test == "low" else (v >= thr) if test == "high" else (thr[0] <= v <= thr[1])
            bad += not ok
            print(f"    {'ok  ' if ok else 'FAIL'} {region:8s} {names[ch]:6s} {test:4s} {thr}  got {v:.1%}")
        print(f"  => {'all expectations met' if not bad else str(bad) + ' FAILED'}")


def terrain_cols(w, h):
    return column(-64.0, w, h), w - 1


def cmd_diff(paths):
    ref = read_bmp(paths[0])
    w, h, _ = ref
    print(f"reference {paths[0]}")
    for path in paths[1:]:
        img = read_bmp(path)
        if img[0] != w or img[1] != h:
            print(f"  {path}: size {img[0]}x{img[1]} != {w}x{h}")
            continue
        out = []
        spans = [("all", terrain_cols(w, h))] + [(n, region_cols(w, h, a, b)) for n, a, b in REGIONS]
        for name, (c0, c1) in spans:
            s = 0.0
            big = 0
            n = 0
            for y in range(ROWS[0], ROWS[1]):
                for x in range(c0, c1 + 1):
                    a = ref[2][y * w + x]
                    b = img[2][y * w + x]
                    d = (abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])) / 3.0
                    s += d
                    big += max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) > 8
                    n += 1
            out.append(f"{name} {s / n:.3f}/{big / n:.3%}")
        print(f"  {path}: mean|d| / >8:  " + "  ".join(out))


def lum(p):
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]


def cmd_repeat(paths):
    for path in paths:
        w, h, px = read_bmp(path)
        c0, c1 = region_cols(w, h, -60.0, -30.0)
        L = [lum(p) for p in px]
        curves = []
        for axis in ("h", "v"):
            D = {}
            for k in range(3, 41):
                s = 0.0
                n = 0
                for y in range(ROWS[0], ROWS[1] - (k if axis == "v" else 0), 2):
                    for x in range(c0, c1 + 1 - (k if axis == "h" else 0), 2):
                        q = y * w + x + (k if axis == "h" else k * w)
                        s += abs(L[y * w + x] - L[q])
                        n += 1
                D[k] = s / n
            # A repeating texture makes D(k) dip at every multiple of its period
            # (here the 1 m checker cell ~6 px and the 2 m tile ~12.5 px); without
            # repetition D only rises with k. Printed: the local minima, and the
            # oscillation = mean |D(k) - (D(k-1) + D(k+1)) / 2| over k = 4..39.
            minima = [k for k in range(4, 40) if D[k] < D[k - 1] and D[k] < D[k + 1]]
            osc = sum(abs(D[k] - 0.5 * (D[k - 1] + D[k + 1])) for k in range(4, 40)) / 36.0
            curves.append(f"{axis}: oscillation {osc:5.2f}, minima at {minima}")
        print(f"{path}: " + " | ".join(curves))


def cast_shadow_zone(tod):
    """x range [x0, x1] of the terrain that faces the sun but lies in the cast
    shadow of the terrain further toward +x (None if there is none)."""
    a = (tod - 0.25) * 2.0 * math.pi
    k = math.sin(a) / math.cos(a)            # sun ray slope in the x/y plane (sun at +x)
    def slope(x):
        return (height(x + 1e-3) - height(x - 1e-3)) / 2e-3
    def shadowed(x0):
        y0 = height(x0)
        x = x0
        while x < 64.0:
            x += 0.01
            if height(x) > y0 + k * (x - x0) + 1e-6:
                return True
        return False
    zone = [i * 0.05 for i in range(-1280, 1280)
            if slope(i * 0.05) < k and shadowed(i * 0.05)]
    return (min(zone), max(zone)) if zone else None


def mean_lum(img, c0, c1):
    w, _, px = img
    s = 0.0
    n = 0
    for y in range(ROWS[0], ROWS[1]):
        for x in range(c0, c1 + 1):
            s += lum(px[y * w + x])
            n += 1
    return s / n


def cmd_shadow(paths):
    if len(paths) % 2:
        print("shadow needs pairs: ON.bmp OFF.bmp ...")
        sys.exit(2)
    tod = float(os.environ.get("HE_TOD", "0.4"))
    zone = cast_shadow_zone(tod)
    if not zone:
        print(f"TOD {tod}: no cast-shadow zone on this profile")
        return
    x0, x1 = zone
    print(f"TOD {tod}: cast shadow x {x0:.1f}..{x1:.1f} "
          f"(cols {column(x0, W_EXPECT, H_EXPECT)}..{column(x1, W_EXPECT, H_EXPECT)} at {W_EXPECT}x{H_EXPECT})")
    for on_path, off_path in zip(paths[0::2], paths[1::2]):
        on, off = read_bmp(on_path), read_bmp(off_path)
        w, h, _ = on
        c0, c1 = region_cols(w, h, x0 + 1.0, x1 - 1.0)
        l_on, l_off = mean_lum(on, c0, c1), mean_lum(off, c0, c1)
        ratio = l_on / max(l_off, 1e-6)
        drift = []
        for name, a, b in REGIONS:
            if name in ("plain", "plateau"):
                p0, p1 = region_cols(w, h, a, b)
                drift.append(abs(mean_lum(on, p0, p1) - mean_lum(off, p0, p1)))
        present = ratio < 0.7 and max(drift) <= 1.0
        print(f"  {on_path} vs {off_path}: inside cols {c0}..{c1} lum {l_on:6.1f} / {l_off:6.1f}"
              f" = {ratio:.2f}, plain/plateau drift {max(drift):.3f}"
              f"  => cast shadow {'PRESENT' if present else 'MISSING'}")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("masks", "diff", "repeat", "shadow"):
        print(__doc__)
        sys.exit(2)
    {"masks": cmd_masks, "diff": cmd_diff, "repeat": cmd_repeat,
     "shadow": cmd_shadow}[sys.argv[1]](sys.argv[2:])
