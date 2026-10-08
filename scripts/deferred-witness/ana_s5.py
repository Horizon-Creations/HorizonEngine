# Thema 150 Schritt 5: analysis of the run_s5.ps1 captures (C:\hw150\cap).
#   python ana_s5.py dec   -> decal probes (shadowed decal / lit decal / floor) per capture
#   python ana_s5.py pairs -> band/disc comparisons for SSR, decals, post chain
import os, sys, warnings
warnings.filterwarnings("ignore")
sys.path.insert(0, r"C:\hw150")
from img import load, diff, CAP

def box(px, x, y, r=4):
    s = [0, 0, 0]; n = 0
    for yy in range(y - r, y + r + 1):
        for xx in range(x - r, x + r + 1):
            for k in range(3): s[k] += px[yy][xx][k]
            n += 1
    return tuple(round(v / n) for v in s)

# DECALTEST + sphere at (2.5, 4, -6.5), camera CAMY 13 / CAMZ -1 / PITCH -65 (1280x720):
# the sphere's CSM shadow lies across the left half of the red decal.
DEC_PROBES = {"decal in shadow": (640, 320), "decal in sun": (560, 400), "floor in sun": (400, 520)}

def dec():
    for n in ["gl_dec_0", "gl_dec_1", "d11_dec_0", "d11_dec_1", "d12_dec_0", "d12_dec_1", "vk_dec_0", "vk_dec_1"]:
        if not os.path.exists(f"{CAP}\\{n}.bmp"): print("MISSING", n); continue
        _, _, px = load(n)
        vals = {k: box(px, *v) for k, v in DEC_PROBES.items()}
        sh, sun = vals["decal in shadow"], vals["decal in sun"]
        ratio = sum(sh) / max(1, sum(sun))
        print(f"{n:10s} " + "  ".join(f"{k}={v}" for k, v in vals.items()) + f"  shadow/sun={ratio:.2f}")

PAIRS = [
    ("d11_ssr_1", "d11_ssroff_1", "SSR: D3D11 deferred on <-> off"),
    ("d12_ssr_1", "d12_ssroff_1", "SSR: D3D12 deferred on <-> off"),
    ("vk_ssr_1", "vk_ssroff_1", "SSR: Vulkan deferred on <-> off"),
    ("d11_ssr_1", "d12_ssr_1", "SSR: D3D11 deferred <-> D3D12 deferred"),
    ("d12_ssr_1", "vk_ssr_1", "SSR: D3D12 deferred <-> Vulkan deferred"),
    ("d12_ssr_0", "vk_ssr_0", "SSR: D3D12 forward <-> Vulkan forward (ref)"),
    ("d12_ssrq2_1", "vk_ssrq2_1", "SSR q2: D3D12 deferred <-> Vulkan deferred"),
    ("d12_ssrq2_0", "vk_ssrq2_0", "SSR q2: D3D12 forward <-> Vulkan forward (ref)"),
    ("gl_dec_1", "d11_dec_1", "DEC: GL deferred <-> D3D11 deferred"),
    ("gl_dec_1", "d12_dec_1", "DEC: GL deferred <-> D3D12 deferred"),
    ("gl_dec_1", "vk_dec_1", "DEC: GL deferred <-> Vulkan deferred"),
    ("d11_dec_1", "d12_dec_1", "DEC: D3D11 deferred <-> D3D12 deferred"),
    ("d12_dec_0", "d12_dec_1", "DEC: D3D12 forward <-> deferred"),
    ("gl_dec_0", "gl_dec_1", "DEC: GL forward <-> deferred (ref)"),
    ("gl_post_1", "d11_post_1", "POST: GL deferred <-> D3D11 deferred"),
    ("gl_post_1", "d12_post_1", "POST: GL deferred <-> D3D12 deferred"),
    ("gl_post_1", "vk_post_1", "POST: GL deferred <-> Vulkan deferred"),
    ("d11_post_0", "d11_post_1", "POST: D3D11 forward <-> deferred"),
    ("d12_post_0", "d12_post_1", "POST: D3D12 forward <-> deferred"),
    ("vk_post_0", "vk_post_1", "POST: Vulkan forward <-> deferred"),
    ("vk_post_1", "vk_plain_1", "POST: Vulkan deferred bloom+SMAA <-> plain (control)"),
    ("d11_post_1", "d11_plain_1", "POST: D3D11 deferred bloom+SMAA <-> plain (control)"),
    ("d12_post_1", "d12_plain_1", "POST: D3D12 deferred bloom+SMAA <-> plain (control)"),
]

def pairs():
    for a, b, what in PAIRS:
        if not (os.path.exists(f"{CAP}\\{a}.bmp") and os.path.exists(f"{CAP}\\{b}.bmp")):
            print(f"MISSING {a} / {b}"); continue
        m, mx, big = diff(a, b, (200, 720), out=f"{CAP}\\diff_{a}_{b}.png")
        print(f"{what:58s} mean {m:6.3f}/255  max {mx:3d}  >8: {big:6.3f}%")

if __name__ == "__main__":
    {"dec": dec, "pairs": pairs}[sys.argv[1]]()
