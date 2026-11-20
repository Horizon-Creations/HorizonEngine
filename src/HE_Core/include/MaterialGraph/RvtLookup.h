#pragma once
// Runtime Virtual Texture lookup, shader side (Thema 333 step 3, docs/rvt-design-2026-11-04.md).
//
// ONE function, written once: the graph node RvtSample emits a call to it, and the tile landscape's hand-written fragment
// shader (kLandscapeFS, Thema 326) can paste the same text. It reads the page table and the three pool arrays with
// texelFetch ONLY, so no sampler register is spent (the D3D11 graph-material budget is exactly full, see
// MaterialShaderLibrary kHlslMaterialPins): the bilinear filter is done by hand, four taps per layer.
//
// Bindings (GLSL `set = 0`; the renderer binds t40..t43 on D3D11, a backend without RVT never sees a shader that uses them):
//   40 heRvtTable    R32_UINT, mip m = (pagesPerSide0 >> m)^2 entries, `slot | effectiveMip << 24`, 0xFFFFFFFF = none
//   41 heRvtAlbedo   RGBA8 array, one slice per pool slot, sRGB view (the Load returns linear)
//   42 heRvtNormal   RGBA8 array, world normal * 0.5 + 0.5
//   43 heRvtSurface  RGBA8 array, R = AO, G = roughness
//   44..46 heRvtAlbedoC/NormalC/SurfaceC  the same three as block-compressed arrays (HE_RVT_COLD; Metal: ASTC via sampler,
//          D3D11/D3D12/Vulkan: BC7 via texelFetch/Load, HE_RVT_COLD_FETCH), see the entry's bit 23
//
// Volume parameters, two vec4 (the node takes them as inputs, normally from Param Vector4 nodes):
//   A = (originX, originZ, extentM, metersPerTexel at mip 0)
//   B = (page payload px, border px, pages per side at mip 0, mip count)
#include <cstdint>

namespace HE
{
inline constexpr uint32_t kRvtTableBinding   = 40;
inline constexpr uint32_t kRvtAlbedoBinding  = 41;
inline constexpr uint32_t kRvtNormalBinding  = 42;
inline constexpr uint32_t kRvtSurfaceBinding = 43;
inline constexpr uint32_t kRvtAlbedoColdBinding  = 44;
inline constexpr uint32_t kRvtNormalColdBinding  = 45;
inline constexpr uint32_t kRvtSurfaceColdBinding = 46;

// The four declarations (included only by a shader that calls heRvtSample, so a material without the node declares nothing).
inline constexpr char kRvtDeclarationsGLSL[] =
    "layout(set = 0, binding = 40) uniform usampler2D heRvtTable;\n"
    "layout(set = 0, binding = 41) uniform sampler2DArray heRvtAlbedo;\n"
    "layout(set = 0, binding = 42) uniform sampler2DArray heRvtNormal;\n"
    "layout(set = 0, binding = 43) uniform sampler2DArray heRvtSurface;\n"
    // The cold pool (Thema 333 step 6j Metal / step 9 Windows): pages that have not changed for a while, block-compressed
    // (ASTC on Metal, BC7 on D3D11/D3D12/Vulkan). Declared only where a backend defines HE_RVT_COLD; the table entry's bit
    // 23 says "cold". The TYPE stays sampler2DArray everywhere (same as the hot arrays above, which are also read with
    // texelFetch only) — SPIRV-Cross derives Load vs Sample from how the function below actually reads it, not the type.
    "#ifdef HE_RVT_COLD\n"
    "layout(set = 0, binding = 44) uniform sampler2DArray heRvtAlbedoC;\n"
    "layout(set = 0, binding = 45) uniform sampler2DArray heRvtNormalC;\n"
    "layout(set = 0, binding = 46) uniform sampler2DArray heRvtSurfaceC;\n"
    "#endif\n";

// The normal layer's texel as a unit vector. Normally the three channels are the world normal * 0.5 + 0.5; a backend that packs the
// layer into two channels (Metal, HE_RVT_RGNORMAL) stores x and z in R and G (a landscape normal never points down, y >= 0) and
// the third component is rebuilt here.
inline constexpr char kRvtNormalGLSL[] =
    "#ifdef HE_RVT_RGNORMAL\n"
    "vec3 heRvtNormalOf(vec4 t) { vec2 xz = t.xy * 2.0 - 1.0; return vec3(xz.x, sqrt(max(0.0, 1.0 - dot(xz, xz))), xz.y); }\n"
    "#else\n"
    "vec3 heRvtNormalOf(vec4 t) { return t.xyz * 2.0 - 1.0; }\n"
    "#endif\n";

// Needs derivatives (dFdx/dFdy): a fragment function. `valid` is 0 where nothing can be shown (outside the volume, no page
// table yet); the outputs are then neutral and the caller decides what to do (the node exposes `valid`).
inline constexpr char kRvtLookupGLSL[] =
    "void heRvtSample(vec2 wxz, vec4 A, vec4 B,\n"
    "                 out vec3 base, out vec3 nrm, out float ao, out float rough, out float valid)\n"
    "{\n"
    "    base = vec3(0.5); nrm = vec3(0.0, 1.0, 0.0); ao = 1.0; rough = 0.8; valid = 0.0;\n"
    "    // Metres per pixel, taken before any early return so every pixel of a quad runs the derivatives.\n"
    "    vec2 dpx = dFdx(wxz), dpy = dFdy(wxz);\n"
    "    float mpp = sqrt(max(dot(dpx, dpx), dot(dpy, dpy)));\n"
    "    vec2 uv = (wxz - A.xy) / max(A.z, 1e-3);\n"
    "    if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) return;\n"
    "    int mipCount = max(int(B.w + 0.5), 1);\n"
    "    int pps0     = max(int(B.z + 0.5), 1);\n"
    "    // The mip whose texel is not finer than a pixel (one texel per pixel).\n"
    "    int mip = clamp(int(floor(log2(max(mpp / max(A.w, 1e-4), 1.0)))), 0, mipCount - 1);\n"
    "    int pps = max(pps0 >> mip, 1);\n"
    "    ivec2 pg = clamp(ivec2(uv * float(pps)), ivec2(0), ivec2(pps - 1));\n"
    "    uint e = texelFetch(heRvtTable, pg, mip).r;\n"
    "    if (e == 0xFFFFFFFFu) return;\n"
    "    int slot = int(e & 0x7FFFFFu);   // bit 23: the page is in the cold pool (HE_RVT_COLD backends only)\n"
    "    int eff  = int(e >> 24);\n"
    "    // The page that actually holds the picture may be a coarser one (the table's fallback): redo the page cell at ITS mip.\n"
    "    int ppsE = max(pps0 >> eff, 1);\n"
    "    vec2 g = uv * float(ppsE);\n"
    "    ivec2 pc = clamp(ivec2(g), ivec2(0), ivec2(ppsE - 1));\n"
    "    vec2 local = g - vec2(pc);\n"
    "    float ps = B.x, bd = B.y;\n"
    "    // Texel space of the physical page, texel centres at whole numbers. A page on the EDGE of the volume has nothing\n"
    "    // beyond it (its border was painted from a scene that holds only this volume: clear-black), so there the filter\n"
    "    // stays inside the payload; an inner page\'s border holds its neighbour\'s ground and is read as it is.\n"
    "    float full = ps + 2.0 * bd - 1.0;\n"
    "    vec2 pmin = vec2(pc.x == 0 ? bd : 0.0, pc.y == 0 ? bd : 0.0);\n"
    "    vec2 pmax = vec2(pc.x == ppsE - 1 ? bd + ps - 1.0 : full, pc.y == ppsE - 1 ? bd + ps - 1.0 : full);\n"
    "    vec2 p = clamp(vec2(bd) + local * ps - 0.5, pmin, pmax);\n"
    "    ivec2 i0 = ivec2(floor(p));\n"
    "    vec2 f = p - vec2(i0);\n"
    "    int hi = int(ps + 2.0 * bd + 0.5) - 1;\n"
    "    ivec2 a = clamp(i0, ivec2(0), ivec2(hi)), b = clamp(i0 + ivec2(1), ivec2(0), ivec2(hi));\n"
    "#ifdef HE_RVT_COLD\n"
    "    if ((e & 0x800000u) != 0u)\n"
    "    {\n"
    "#ifdef HE_RVT_COLD_FETCH\n"
    "        // D3D11's graph-material sampler budget is exactly full (kHlslMaterialPins): the cold arrays are block-compressed\n"
    "        // (BC7) but read with texelFetch/Load like the hot ones (the hardware decompresses the block either way), same\n"
    "        // four-tap hand filter, same a/b/f as the hot path above.\n"
    "        vec4 cc00 = texelFetch(heRvtAlbedoC, ivec3(a.x, a.y, slot), 0), cc10 = texelFetch(heRvtAlbedoC, ivec3(b.x, a.y, slot), 0);\n"
    "        vec4 cc01 = texelFetch(heRvtAlbedoC, ivec3(a.x, b.y, slot), 0), cc11 = texelFetch(heRvtAlbedoC, ivec3(b.x, b.y, slot), 0);\n"
    "        base = mix(mix(cc00, cc10, f.x), mix(cc01, cc11, f.x), f.y).rgb;\n"
    "        vec4 cn00 = texelFetch(heRvtNormalC, ivec3(a.x, a.y, slot), 0), cn10 = texelFetch(heRvtNormalC, ivec3(b.x, a.y, slot), 0);\n"
    "        vec4 cn01 = texelFetch(heRvtNormalC, ivec3(a.x, b.y, slot), 0), cn11 = texelFetch(heRvtNormalC, ivec3(b.x, b.y, slot), 0);\n"
    "        vec3 ccn = heRvtNormalOf(mix(mix(cn00, cn10, f.x), mix(cn01, cn11, f.x), f.y));\n"
    "        nrm = ccn / max(length(ccn), 1e-4);\n"
    "        vec4 cs00 = texelFetch(heRvtSurfaceC, ivec3(a.x, a.y, slot), 0), cs10 = texelFetch(heRvtSurfaceC, ivec3(b.x, a.y, slot), 0);\n"
    "        vec4 cs01 = texelFetch(heRvtSurfaceC, ivec3(a.x, b.y, slot), 0), cs11 = texelFetch(heRvtSurfaceC, ivec3(b.x, b.y, slot), 0);\n"
    "        vec2 ccs = mix(mix(cs00, cs10, f.x), mix(cs01, cs11, f.x), f.y).rg;\n"
    "        ao = ccs.x; rough = ccs.y;\n"
    "#else\n"
    "        // The cold page (Metal): same position p (texel centres at whole numbers, already clamped to the payload), three bilinear samples.\n"
    "        vec3 cuv = vec3((p + 0.5) / float(hi + 1), float(slot));\n"
    "        base = textureLod(heRvtAlbedoC, cuv, 0.0).rgb;\n"
    "        vec3 cn = heRvtNormalOf(textureLod(heRvtNormalC, cuv, 0.0));\n"
    "        nrm = cn / max(length(cn), 1e-4);\n"
    "        vec2 cs = textureLod(heRvtSurfaceC, cuv, 0.0).rg;\n"
    "        ao = cs.x; rough = cs.y;\n"
    "#endif\n"
    "        valid = 1.0;\n"
    "        return;\n"
    "    }\n"
    "#endif\n"
    "    vec4 c00 = texelFetch(heRvtAlbedo, ivec3(a.x, a.y, slot), 0), c10 = texelFetch(heRvtAlbedo, ivec3(b.x, a.y, slot), 0);\n"
    "    vec4 c01 = texelFetch(heRvtAlbedo, ivec3(a.x, b.y, slot), 0), c11 = texelFetch(heRvtAlbedo, ivec3(b.x, b.y, slot), 0);\n"
    "    base = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y).rgb;\n"
    "    vec4 n00 = texelFetch(heRvtNormal, ivec3(a.x, a.y, slot), 0), n10 = texelFetch(heRvtNormal, ivec3(b.x, a.y, slot), 0);\n"
    "    vec4 n01 = texelFetch(heRvtNormal, ivec3(a.x, b.y, slot), 0), n11 = texelFetch(heRvtNormal, ivec3(b.x, b.y, slot), 0);\n"
    "    vec3 nm = heRvtNormalOf(mix(mix(n00, n10, f.x), mix(n01, n11, f.x), f.y));\n"
    "    nrm = nm / max(length(nm), 1e-4);\n"
    "    vec4 s00 = texelFetch(heRvtSurface, ivec3(a.x, a.y, slot), 0), s10 = texelFetch(heRvtSurface, ivec3(b.x, a.y, slot), 0);\n"
    "    vec4 s01 = texelFetch(heRvtSurface, ivec3(a.x, b.y, slot), 0), s11 = texelFetch(heRvtSurface, ivec3(b.x, b.y, slot), 0);\n"
    "    vec2 sf = mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y).rg;\n"
    "    ao = sf.x; rough = sf.y; valid = 1.0;\n"
    "}\n";
}
