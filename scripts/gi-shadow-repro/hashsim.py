# float32 emulation of the GI shadow kernel's hash + temporal + blur chain
# (gi_shadow.comp giHash2/giConeSample, gi_temporal.frag, and the 3x3 box of
# gi_blur.frag, which Thema 134 replaced with gi_atrous.frag — not emulated) on a
# static straight shadow edge, to separate seed-precision effects from the
# filter design. Static camera/occluder -> reprojection always accepted.
import numpy as np, sys
f32 = np.float32
W, H = 400, 225              # half-res of an 800x450 viewport
gy, gx = np.mgrid[0:H, 0:W].astype(np.uint32)

def hash2(seed):
    s = f32(seed) * f32(13.37)
    px = gx.astype(f32) + s; py = gy.astype(f32) + s
    d1 = px * f32(12.9898) + py * f32(78.233)      # float32 dot
    d2 = px * f32(39.3468) + py * f32(11.1352)
    a = np.sin(d1, dtype=f32) * f32(43758.5453)
    b = np.sin(d2, dtype=f32) * f32(24634.6345)
    return (a - np.floor(a)).astype(f32), (b - np.floor(b)).astype(f32), d1

# Penumbra model: edge along y, penumbra half-width PW pixels: the cone sample
# displaces the occluder edge by r*cos(phi)*PW pixels (uniform disk).
PW = 6.0
xc = gx.astype(np.float64) - W/2
def raw_vis(seed):
    u, v, _ = hash2(seed)
    r = np.sqrt(u.astype(np.float64)); phi = 2*np.pi*v
    return (xc + 0.5 + r*np.cos(phi)*PW > 0).astype(np.float64)
# ground truth: fraction of the unit disk with x' > -(xc+0.5)/PW
T = 1.0 - np.clip(((np.arccos(np.clip((xc+0.5)/PW,-1,1)) - np.clip((xc+0.5)/PW,-1,1)*np.sqrt(1-np.clip((xc+0.5)/PW,-1,1)**2))/np.pi), 0, 1)

def box3(a):
    p = np.pad(a, 1, mode='edge'); s = 0
    for dy in (-1,0,1):
        for dx in (-1,0,1):
            s = s + p[1+dy:1+dy+H, 1+dx:1+dx+W]
    return s/9
def nminmax(a):
    p = np.pad(a, 1, mode='edge'); mn = a.copy(); mx = a.copy()
    for dy in (-1,0,1):
        for dx in (-1,0,1):
            q = p[1+dy:1+dy+H, 1+dx:1+dx+W]; mn = np.minimum(mn,q); mx = np.maximum(mx,q)
    return mn, mx

def run(seed0, frames=60, clamp=True):
    hist = None; outs = []
    for k in range(frames):
        raw = raw_vis(seed0 + 1 + k)
        if hist is None: hist = raw
        else:
            h = hist
            if clamp:
                mn, mx = nminmax(raw); h = np.clip(hist, mn, mx)
            hist = raw*0.1 + h*0.9
        outs.append(box3(hist))
    return outs

band = np.abs(xc) < PW+2     # penumbra columns only
for seed0 in [0, 3000, 20000, 100000, 400000]:
    _, _, d1 = hash2(seed0)
    ulp = float(np.spacing(np.abs(d1).max()))
    u, v, _ = hash2(seed0+1)
    # pairs of horizontally/vertically adjacent pixels with identical xi
    dup = np.mean((u[:,1:]==u[:,:-1])&(v[:,1:]==v[:,:-1])) + np.mean((u[1:,:]==u[:-1,:])&(v[1:,:]==v[:-1,:]))
    uniq = len(np.unique(np.round(u[band],6)))
    for clamp in (True, False):
        o = run(seed0, 60, clamp)
        last = o[-1]
        err  = np.sqrt(np.mean((last[band]-T[band])**2))
        flick = np.sqrt(np.mean((o[-1][band]-o[-2][band])**2))
        print(f"seed0={seed0:7d} (~{seed0/144/60:6.1f} min @144fps) dot-ulp={ulp:6.3f} dupAdj={dup:.4f} uniqU={uniq:6d} clamp={int(clamp)}  RMSerr={err:.4f} frame2frame={flick:.4f}")
