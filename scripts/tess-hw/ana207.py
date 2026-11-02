"""Thema 207 step 4: pixel A/B of the displace witness per backend (cap207.ps1)."""
import sys, os
import numpy as np
from PIL import Image

shots = sys.argv[1] if len(sys.argv) > 1 else r'C:\hw207\shots'
rhis = sys.argv[2].split(',') if len(sys.argv) > 2 else ['D3D11', 'D3D12', 'Vulkan', 'OpenGL']
pre = sys.argv[3] if len(sys.argv) > 3 else ''   # 'demo-' for cap207.ps1 -Asset (step 5)

def load(rhi, v):
    p = os.path.join(shots, f'{pre}{rhi}-{v}.bmp')
    return np.asarray(Image.open(p).convert('RGB')).astype(np.int16) if os.path.exists(p) else None

def diff(a, b, band=None):
    d = np.abs(a - b).max(axis=2)
    if band is not None:
        d = d[band]
    return int((d > 0).sum()), int(d.max())

for rhi in rhis:
    ref, ref2, disp, off, p0, p30 = (load(rhi, v) for v in ('ref', 'ref2', 'disp', 'off', 'p0', 'p30'))
    if ref is None:
        print(f'{rhi}: no ref'); continue
    h = ref.shape[0]
    low = (slice(h // 4, h), slice(None))  # below the sky band (animated clouds live up top)
    for name, img in (('ref2', ref2), ('off', off), ('disp', disp), ('p0', p0), ('p30', p30)):
        if img is None and name in ('p0', 'p30'):
            continue
        if img is None:
            print(f'{rhi:7} {name:5} missing'); continue
        n, m = diff(ref, img)
        nl, ml = diff(ref, img, low)
        print(f'{rhi:7} {name:5} vs ref: {n:7d} px differ (max {m:3d}); below sky band {nl:7d} px (max {ml:3d})')
    if p30 is not None and disp is not None:
        n, m = diff(disp, p30)
        print(f'{rhi:7} p30   vs disp: {n:7d} px differ (max {m:3d})  (the Height knob at run time)')
    if disp is not None:
        Image.fromarray(disp.astype(np.uint8)).save(os.path.join(shots, f'{pre}{rhi}-disp.png'))
        Image.fromarray(ref.astype(np.uint8)).save(os.path.join(shots, f'{pre}{rhi}-ref.png'))
