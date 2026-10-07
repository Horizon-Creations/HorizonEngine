# CSM sun-lag metric for the run146.ps1 captures (Thema 146).
# usage: ana146.py CAPDIR [rhi ...]       (CAPDIR = <root>\cap, rhi default vulkan)
# swept = floor pixels where S0 (static at TOD - step) and S (static at TOD)
# differ by > 30 grey levels: the area the shadow edges moved over. (A step of
# 0.02 also brightens the whole lit floor by ~9 levels, which is why the
# threshold is not the 8 of ana_motion.py: that would count the entire floor.)
#   lag   = mean |P1 - S| / mean |S0 - S| there: 0 = the captured frame's
#           shadows stand where this frame's sun puts them, 1 = they stand
#           where the previous frame's sun put them (one frame behind).
#   floor = the same ratio for S2 (a second static capture) instead of P1.
#   P1_nearer_S0 = share of swept pixels whose P1 value is closer to S0 than
#           to S (0 % = no lag, ~100 % = full lag).
#   mask  = shadow masks (luminance < 150; lit floor ~190-200, shadow ~100):
#           |P1 xor S| / |S0 xor S| — how much of the shadow moved with the sun.
import os, sys, struct, numpy as np
def lum(p):
    d = open(p, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]; w, h = struct.unpack_from('<ii', d, 18); bpp = struct.unpack_from('<H', d, 28)[0]
    stride = ((w*bpp//8)+3)//4*4
    a = np.frombuffer(d, np.uint8, count=stride*abs(h), offset=off).reshape(abs(h), stride)[:, :w*bpp//8].reshape(abs(h), w, bpp//8)
    if h > 0: a = a[::-1]
    a = a[..., :3].astype(np.float64)  # BGR
    return 0.2126*a[..., 2] + 0.7152*a[..., 1] + 0.0722*a[..., 0]
cap = sys.argv[1]
for rhi in (sys.argv[2:] or ['vulkan']):
    f = lambda n: lum(os.path.join(cap, f'{rhi}_{n}.bmp'))
    s, s2, s0, p1 = f('S'), f('S2'), f('S0'), f('P1')
    swept = np.abs(s0 - s) > 30.0
    swept[:80] = False                               # floor region only (cubes/sky at the top)
    ref = np.abs(s0 - s)[swept].mean()
    lag = np.abs(p1 - s)[swept].mean() / ref
    flo = np.abs(s2 - s)[swept].mean() / ref
    nearer_old = (np.abs(p1 - s0) < np.abs(p1 - s))[swept].mean() * 100.0
    m = lambda a: (a < 150.0)[80:]
    moved = (m(s0) ^ m(s)).sum()
    mask = (m(p1) ^ m(s)).sum() / max(moved, 1)
    print(f"{os.path.basename(os.path.dirname(cap.rstrip(os.sep))):5s} {rhi:7s} swept_px={swept.sum():6d} "
          f"|S0-S|={ref:6.2f} lag={lag:5.3f} floor={flo:5.3f} P1_nearer_S0={nearer_old:5.1f}% "
          f"mask={mask:5.3f} (moved {moved} px) max|P1-S|={np.abs(p1 - s)[80:].max():5.1f} "
          f"S==S2={np.array_equal(s, s2)} P1==S={np.array_equal(p1, s)}")
