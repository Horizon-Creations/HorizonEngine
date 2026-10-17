import sys, re, os
sys.path.insert(0, r"C:\hw150")
from img import load, diff, CAP

BAND = (200, 720)
pairs = [
    ("d12_mt_0", "d12_mt_1", "MATERIALTEST=matte: D3D12 forward <-> deferred"),
    ("d12_ml_0", "d12_ml_1", "MANYLIGHTS=16: D3D12 forward <-> deferred (clustered)"),
    ("d12_ml_nc_0", "d12_ml_nc_1", "MANYLIGHTS=16, HE_FORWARD_CLUSTER=0: forward <-> deferred (8-light)"),
    ("d12_ml_1", "d12_ml_nc_1", "control: deferred clustered <-> deferred 8-light"),
    ("d12_gi_0", "d12_gi_1", "MATERIALTEST=1 GIBLEED=3 GI on: forward <-> deferred"),
    ("d12_gi_1", "d12_gioff_1", "control: deferred GI on <-> GI off"),
    ("d12_land_0", "d12_land_1", "LANDSCAPELAYERS=1: forward <-> deferred"),
    ("d12_lsp_0", "d12_lsp_1", "LOCALSHADOW=point: D3D12 forward <-> deferred"),
    ("d12_lss_0", "d12_lss_1", "LOCALSHADOW=spot: D3D12 forward <-> deferred"),
    ("gl_lsp_1", "d12_lsp_1", "LOCALSHADOW=point: GL deferred <-> D3D12 deferred"),
    ("gl_lsp_0", "gl_lsp_1", "LOCALSHADOW=point: GL forward <-> GL deferred (ref)"),
    ("gl_mt_1", "d12_mt_1", "MATERIALTEST=matte: GL deferred <-> D3D12 deferred"),
    ("gl_mt_0", "d12_mt_0", "MATERIALTEST=matte: GL forward <-> D3D12 forward"),
    ("d11_mt_1", "d12_mt_1", "MATERIALTEST=matte: D3D11 deferred <-> D3D12 deferred"),
    ("d11_mt_0", "d12_mt_0", "MATERIALTEST=matte: D3D11 forward <-> D3D12 forward"),
    ("gl_ml_1", "d12_ml_1", "MANYLIGHTS=16: GL deferred <-> D3D12 deferred"),
    ("d11_ml_1", "d12_ml_1", "MANYLIGHTS=16: D3D11 deferred <-> D3D12 deferred"),
    ("gl_lss_1", "d12_lss_1", "LOCALSHADOW=spot: GL deferred <-> D3D12 deferred"),
    ("d11_lsp_1", "d12_lsp_1", "LOCALSHADOW=point: D3D11 deferred <-> D3D12 deferred"),
    ("gl_lsp_0", "d12_lsp_0", "LOCALSHADOW=point: GL forward <-> D3D12 forward"),
    ("gl_land_1", "d12_land_1", "LANDSCAPELAYERS: GL deferred <-> D3D12 deferred"),
    ("d11_land_1", "d12_land_1", "LANDSCAPELAYERS: D3D11 deferred <-> D3D12 deferred"),
    ("gl_si_1", "d12_si_1", "SHADOWINSTTEST (built-in): GL deferred <-> D3D12 deferred"),
    ("d12_si_0", "d12_si_1", "SHADOWINSTTEST (built-in): D3D12 forward <-> deferred"),
]
for v in range(1, 5):
    pairs.append((f"gl_sigb{v}", f"d12_sigb{v}", f"SHADOWINSTTEST G-buffer view {v}: GL <-> D3D12"))

for a, b, what in pairs:
    if not (os.path.exists(f"{CAP}\\{a}.bmp") and os.path.exists(f"{CAP}\\{b}.bmp")):
        print(f"MISSING {a} / {b}")
        continue
    m, mx, big = diff(a, b, BAND, out=f"{CAP}\\diff_{a}_{b}.png")
    print(f"{what:70s} mean {m:6.3f}/255  max {mx:3d}  >8: {big:6.3f}%")

print()
for f in sorted(os.listdir(CAP)):
    if not (f.startswith("d12_") and f.endswith(".log")): continue
    t = open(os.path.join(CAP, f), encoding="utf-8", errors="replace").read()
    dl = [l for l in t.splitlines() if "D3D12 debug layer" in l]
    errs = [l for l in dl if "[ERROR]" in l or "ERROR" in l.split("]")[1]]
    slot3 = [l for l in dl if "slot 3" in l]
    clear = [l for l in dl if "clear values do not match" in l]
    other = [l for l in dl if l not in slot3 and l not in clear]
    dfr = re.findall(r"deferred frame \(([^)]*)\)", t)
    ready = re.findall(r"deferred path ready \(([^)]*)\)", t)
    sess = re.findall(r"Session summary: (.*)", t)
    print(f"{f:22s} debug={len(dl)} (slot3={len(slot3)} clear={len(clear)} other={len(other)} err={len(errs)}) "
          f"ready={ready[:1]} frame={dfr[:1]} | {sess[-1] if sess else '?'}")
    for l in other[:3]:
        print("     ", l[l.find('D3D12 debug layer'):][:220])
