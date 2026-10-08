"""Thema 173, Schritt 2: sky in the GI-reflection mirror (HE_DUMP_AUTOLANDMIRROR=1).

    python3 ana173sky.py A.bmp [B.bmp ...]

Capture with cap158auto.sh (see docs/gi-reflexionen-himmel-autoland-ursache-2026-10-08.md
section 2/6), camera CAMX=-40 CAMY=304 CAMZ=14 PITCH=-4. Per image:

  * the five probe pixels of step 1 (mirror front top/bottom, yawed top/bottom, real sky),
  * the sky part of the camera-facing mirror (x 500..780, y 190..320): mean RGB and the
    standard deviation of luma. A cloudless cubemap is a smooth gradient (low spread);
    the real sky with clouds is not. The same box on the REAL sky (x 500..780, y 20..150)
    is printed as the reference spread.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ana158auto import read_bmp  # noqa: E402

PROBES = [("front-top", 640, 220), ("front-bottom", 640, 380), ("yaw-top", 250, 280),
          ("yaw-bottom", 250, 390), ("sky", 900, 120)]


def box_stats(w, px, x0, x1, y0, y1):
    vals = [px[y * w + x] for y in range(y0, y1) for x in range(x0, x1)]
    n = len(vals)
    mean = tuple(sum(v[c] for v in vals) / n for c in range(3))
    lum = [0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2] for v in vals]
    lm = sum(lum) / n
    sd = (sum((l - lm) ** 2 for l in lum) / n) ** 0.5
    return mean, sd


def main(paths):
    for p in paths:
        w, h, px = read_bmp(p)
        probes = "  ".join("%s %d,%d,%d" % ((name,) + px[y * w + x]) for name, x, y in PROBES)
        (mr, mg, mb), msd = box_stats(w, px, 500, 780, 190, 320)
        (_, _, _), ssd = box_stats(w, px, 500, 780, 20, 150)
        print("%s\n  %s\n  mirror-sky mean %.0f,%.0f,%.0f luma-sd %.1f | real-sky luma-sd %.1f"
              % (os.path.basename(p), probes, mr, mg, mb, msd, ssd))


if __name__ == "__main__":
    main(sys.argv[1:])
