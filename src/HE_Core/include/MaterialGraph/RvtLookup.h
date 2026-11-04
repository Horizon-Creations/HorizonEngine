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

// The four declarations (included only by a shader that calls heRvtSample, so a material without the node declares nothing).
inline constexpr char kRvtDeclarationsGLSL[] =
    "layout(set = 0, binding = 40) uniform usampler2D heRvtTable;\n"
    "layout(set = 0, binding = 41) uniform sampler2DArray heRvtAlbedo;\n"
    "layout(set = 0, binding = 42) uniform sampler2DArray heRvtNormal;\n"
    "layout(set = 0, binding = 43) uniform sampler2DArray heRvtSurface;\n";

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
    "    int slot = int(e & 0xFFFFFFu);\n"
    "    int eff  = int(e >> 24);\n"
    "    // The page that actually holds the picture may be a coarser one (the table's fallback): redo the page cell at ITS mip.\n"
    "    int ppsE = max(pps0 >> eff, 1);\n"
    "    vec2 g = uv * float(ppsE);\n"
    "    vec2 local = g - vec2(clamp(ivec2(g), ivec2(0), ivec2(ppsE - 1)));\n"
    "    float ps = B.x, bd = B.y;\n"
    "    vec2 p = vec2(bd) + local * ps - 0.5;       // texel space of the physical page, texel centres at whole numbers\n"
    "    ivec2 i0 = ivec2(floor(p));\n"
    "    vec2 f = p - vec2(i0);\n"
    "    int hi = int(ps + 2.0 * bd + 0.5) - 1;\n"
    "    ivec2 a = clamp(i0, ivec2(0), ivec2(hi)), b = clamp(i0 + ivec2(1), ivec2(0), ivec2(hi));\n"
    "    vec4 c00 = texelFetch(heRvtAlbedo, ivec3(a.x, a.y, slot), 0), c10 = texelFetch(heRvtAlbedo, ivec3(b.x, a.y, slot), 0);\n"
    "    vec4 c01 = texelFetch(heRvtAlbedo, ivec3(a.x, b.y, slot), 0), c11 = texelFetch(heRvtAlbedo, ivec3(b.x, b.y, slot), 0);\n"
    "    base = mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y).rgb;\n"
    "    vec4 n00 = texelFetch(heRvtNormal, ivec3(a.x, a.y, slot), 0), n10 = texelFetch(heRvtNormal, ivec3(b.x, a.y, slot), 0);\n"
    "    vec4 n01 = texelFetch(heRvtNormal, ivec3(a.x, b.y, slot), 0), n11 = texelFetch(heRvtNormal, ivec3(b.x, b.y, slot), 0);\n"
    "    vec3 nm = mix(mix(n00, n10, f.x), mix(n01, n11, f.x), f.y).xyz * 2.0 - 1.0;\n"
    "    nrm = nm / max(length(nm), 1e-4);\n"
    "    vec4 s00 = texelFetch(heRvtSurface, ivec3(a.x, a.y, slot), 0), s10 = texelFetch(heRvtSurface, ivec3(b.x, a.y, slot), 0);\n"
    "    vec4 s01 = texelFetch(heRvtSurface, ivec3(a.x, b.y, slot), 0), s11 = texelFetch(heRvtSurface, ivec3(b.x, b.y, slot), 0);\n"
    "    vec2 sf = mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y).rg;\n"
    "    ao = sf.x; rough = sf.y; valid = 1.0;\n"
    "}\n";
}
