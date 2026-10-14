"""Thema 204: measure GI lines in cap204.ps1 captures.

  python ana204.py cols  <shot.bmp> ...   column luminance (rows 100..600 averaged), px 470..710
  python ana204.py seams <shot.bmp> ...   thin horizontal dark lines per row (rows > 300)

cols: the AUTOLAND witness is constant along Z, so a lighting line on the
ramp is a dip in the column mean. The DDGI line sits at px ~590 in the
top-down shot (world x ~ -6.4 at y ~ 323 after the perspective correction:
the ramp is 80 m from the camera, the plain 100 m).

seams: a pixel counts when it is at least 5 luma darker than BOTH pixels
3 rows above and below. Chunk seams on the flat plain of the MOUNTAINTEST
witness show up as rows with many such pixels; GI off is the control.
"""
import sys

import numpy as np
from PIL import Image


def lum(path):
    return np.asarray(Image.open(path).convert('L'), dtype=float)


def cols(paths):
    for p in paths:
        c = lum(p)[100:600].mean(0)
        print('%-34s %s' % (p.replace('\\', '/').split('/')[-1],
                            ' '.join('%d:%.0f' % (x, c[x]) for x in range(470, 720, 10))))


def seams(paths, t=5.0, min_px=6):
    for p in paths:
        a = lum(p)
        c = a[3:-3]
        m = (c < a[:-6] - t) & (c < a[6:] - t)
        rows = m.sum(1)
        hits = [(y + 3, int(k)) for y, k in enumerate(rows) if k >= min_px and y + 3 > 300]
        print('%-34s total=%5d rows=%s' % (p.replace('\\', '/').split('/')[-1],
                                            int(m[297:].sum()), hits[:12]))


if __name__ == '__main__':
    {'cols': cols, 'seams': seams}[sys.argv[1]](sys.argv[2:])
