# Thema 134 metrics for the Windows matrix (Thema 142, step 2): ana134.py /
# ana_motion.py over run142.ps1 captures, one row per (backend, case, variant).
# usage: ana142.py NEW_CAP_DIR STOCK_CAP_DIR [RHI ...]
#   NEW_CAP_DIR   = <root>\cap of the build with PR #86 (variants gtR, ABC, ABCr1, ...)
#   STOCK_CAP_DIR = <root>\cap of the build before #86 (variant stock)
# Every case is measured against the gtR reference of its static twin rendered by
# the SAME backend (motion cases: pan6/move6 -> s6, cpan6/cmove6 -> c6).
import sys, os, numpy as np
sys.argv, args = sys.argv[:1], sys.argv[1:]
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ana.py')).read().split('ref, a0, a1')[0])
new_dir, stock_dir = args[0], args[1]
rhis = args[2:] or ['D3D11', 'D3D12', 'Vulkan', 'OpenGL']
TWIN = {'pan6': 's6', 'move6': 's6', 'cpan6': 'c6', 'cmove6': 'c6'}
CASES = ['s6', 's05', 'c05', 'cnear', 'pan6', 'cpan6', 'move6', 'cmove6']
VARS = [('stock', stock_dir), ('ABC', new_dir), ('ABCr1', new_dir), ('ABCr4', new_dir), ('ABCsw', new_dir)]

def cap(d, rhi, case, var, f):
    p = os.path.join(d, f'{rhi}_{case}__{var}_f{f}.bmp')
    return p if os.path.exists(p) else None

def metrics(gt, a0, a1):
    sm = box(gt, 2); gy, gx = np.gradient(sm); g = np.hypot(gx, gy)
    band = (g > 1.5); band[:80] = False
    sharp = band & (g >= np.percentile(g[band], 90))
    flick = np.abs(a1 - a0); err = a0 - gt
    return dict(flicker=flick[band].mean(), p99=np.percentile(flick[band], 99),
                rmse=np.sqrt((err[band] ** 2).mean()), bias=np.abs(box(err, 3)[band]).mean(),
                sharp=np.sqrt((err[sharp] ** 2).mean()), band=int(band.sum()))

rows = []
for rhi in rhis:
    for case in CASES:
        g = cap(new_dir, rhi, TWIN.get(case, case), 'gtR', 60)
        if not g: continue
        gt = lum(g)
        for var, d in VARS:
            a0, a1 = cap(d, rhi, case, var, 60), cap(d, rhi, case, var, 61)
            if not (a0 and a1): continue
            m = metrics(gt, lum(a0), lum(a1))
            rows.append((rhi, case, var, m))
            print(f"{rhi:7s} {case:7s} {var:6s} band={m['band']:6d} flicker={m['flicker']:6.3f} (p99 {m['p99']:5.1f}) "
                  f"rmse={m['rmse']:6.3f} bias={m['bias']:6.3f} sharp_rmse={m['sharp']:6.3f}")
    # Reference self-flicker and run-to-run noise floor (ABC vs ctl, stock vs ctl, f60).
    for case in ['s6']:
        g0, g1 = cap(new_dir, rhi, case, 'gtR', 60), cap(new_dir, rhi, case, 'gtR', 61)
        if g0 and g1:
            m = metrics(lum(g0), lum(g0), lum(g1))
            print(f"{rhi:7s} {case:7s} gtR    reference self-flicker={m['flicker']:6.3f} (p99 {m['p99']:5.1f})")
        for var, d in [('ABC', new_dir), ('stock', stock_dir)]:
            a, c = cap(d, rhi, case, var, 60), cap(d, rhi, case, 'ctl', 60)
            if a and c:
                x, y = bmp(a), bmp(c)
                print(f"{rhi:7s} {case:7s} {var:6s} vs ctl (2nd run): mean|d|={np.abs(x - y).mean():.4f} max={np.abs(x - y).max():.0f}")
    # Ghost after a sun step (ana_motion.py): NEW = case f60, OLD = TOD-0.005, MOVED = TODSTEP.
    for base in ['s6', 's05']:
        for var, d in [('stock', stock_dir), ('ABC', new_dir)]:
            n, o, mv = cap(d, rhi, base, var, 60), cap(d, rhi, base + 'old', var, 60), cap(d, rhi, base + 'mov', var, 60)
            if not (n and o and mv): continue
            new, old, mov = lum(n), lum(o), lum(mv)
            swept = np.abs(old - new) > 8.0; swept[:80] = False
            print(f"{rhi:7s} {base:7s} {var:6s} ghost={np.abs(mov - new)[swept].mean() / np.abs(old - new)[swept].mean():5.3f} "
                  f"swept_px={swept.sum():6d} |old-new|={np.abs(old - new)[swept].mean():6.2f}")
