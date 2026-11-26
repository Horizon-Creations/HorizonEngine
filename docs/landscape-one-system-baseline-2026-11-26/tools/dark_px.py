import sys, numpy as np
from PIL import Image
def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(float)
for p in sys.argv[1:]:
    im = load(p); h = im.shape[0]
    lum = im.mean(axis=2)
    ground = lum[int(h*0.5):]            # below the horizon
    dark = (ground < 70)
    # grid lines are thin: erode by requiring 5x5 neighbourhood dark
    from numpy.lib.stride_tricks import sliding_window_view
    k = 5
    win = sliding_window_view(dark, (k, k)).all(axis=(2, 3))
    print(f"{p}: ground dark px {int(dark.sum())}, solid dark px (5x5) {int(win.sum())}")
