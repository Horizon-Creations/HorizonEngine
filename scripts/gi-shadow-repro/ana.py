# Edge-band metrics for GI shadow captures (Thema 131).
# usage: ana.py REF.bmp A_f60.bmp A_f61.bmp [label]
# band = pixels on the floor where the REFERENCE has a shadow-edge gradient.
import sys, struct, numpy as np
def bmp(p):
    d = open(p,'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]; w, h = struct.unpack_from('<ii', d, 18); bpp = struct.unpack_from('<H', d, 28)[0]
    stride = ((w*bpp//8)+3)//4*4
    a = np.frombuffer(d, np.uint8, count=stride*abs(h), offset=off).reshape(abs(h), stride)[:, :w*bpp//8].reshape(abs(h), w, bpp//8)
    if h > 0: a = a[::-1]
    return a[..., :3].astype(np.float64)  # BGR
def lum(p):
    a = bmp(p); return 0.2126*a[...,2] + 0.7152*a[...,1] + 0.0722*a[...,0]
def box(a, r):
    k = 2*r+1; p = np.pad(a, r, mode='edge'); c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1,0),(1,0)))
    return (c[k:,k:] - c[:-k,k:] - c[k:,:-k] + c[:-k,:-k]) / (k*k)
ref, a0, a1 = lum(sys.argv[1]), lum(sys.argv[2]), lum(sys.argv[3])
H, W = ref.shape
sm = box(ref, 2)
gy, gx = np.gradient(sm)
g = np.hypot(gx, gy)
band = (g > 1.5); band[:80] = False          # floor region only (cubes/sky at top)
band &= box(band.astype(float), 0) > 0
flat = (g < 0.15); flat[:250] = False         # lit/umbra flat floor away from edges
flick = np.abs(a1 - a0)
hf = np.abs(a0 - box(a0, 2))
lab = sys.argv[4] if len(sys.argv) > 4 else sys.argv[2]
print(f"{lab:28s} band_px={band.sum():6d} flicker_edge={flick[band].mean():6.3f} (p99 {np.percentile(flick[band],99):5.1f}) "
      f"hf_edge={hf[band].mean():6.3f} | flat: flicker={flick[flat].mean():6.3f} hf={hf[flat].mean():6.3f} | meanL_edge={a0[band].mean():6.1f}")
