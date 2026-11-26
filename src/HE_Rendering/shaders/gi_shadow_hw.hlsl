// GI shadow rays — HARDWARE ray tracing variant (DXR 1.1 inline RayQuery,
// SM 6.5, compiled offline by dxc — FXC cannot target SM 6). Identical binding
// interface to the embedded SW kernel (kGiShadowCSHLSL12 in D3D12Renderer.cpp:
// b0, t0/t1, u0 unchanged), plus the TLAS as a root SRV at t7. Ray semantics
// mirror the SW kernel 1:1: one cone-jittered any-hit ray per pixel toward the
// dominant directional light, same origin bias (N * 0.05) and tMin/tMax
// (0.02 / 10000). The host guarantees the TLAS instance order equals the
// giInsts order (irrelevant here — occlusion only — but kept as an invariant).
//
// ─── SYNC: seven hand-kept copies of this kernel ─────────────────────────────
// The GI shadow-ray kernel exists once per API dialect and once per traversal
// path. Nothing generates them from a common source:
//
//   GLSL SW  src/HE_Rendering/shaders/gi_shadow.comp     (Vulkan, CPU BVH)
//   GLSL HW  src/HE_Rendering/shaders/gi_shadow_hw.comp  (Vulkan, ray query)
//   HLSL SW  src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h
//            `kGiTraversalHLSL` + `kGiShadowCSHLSL`      (D3D11 and D3D12)
//   HLSL HW  src/HE_Rendering/shaders/gi_shadow_hw.hlsl  (this file)
//   GLSL SW  src/HE_Rendering/src/Backends/OpenGL/OpenGLRenderer.cpp
//            `kGiTraversalGLSL` + `kGiShadowCS`          (GL 4.3)
//   MSL  SW  src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm `kGISWMSL`
//   MSL  HW  src/HE_Rendering/src/Backends/Metal/MetalRenderer.mm `kGIShadowMSL`
//
// WHAT MUST NOT DRIFT: the ray parameters — surface origin bias (N * 0.05),
// tMin/tMax (0.02 / 10000), the local-light skip radius and the 0.1 slack on the
// local ray's tMax — the cone-jitter hash constants, and the 8x8 thread group
// (the host dispatches ceil(w/8) x ceil(h/8) and relies on it).
//
// WHAT BREAKS ON DRIFT: every copy still compiles and still runs. Only the
// shadow mask differs, on the one backend/path that was edited — a changed bias
// gives acne or peter-panning, a changed tMax makes distant shadows stop, a
// changed hash gives a different noise the temporal filter then resolves
// differently. Nothing here is covered by an image test.
//
// GUARD: tests/test_culling.cpp, "GI kernels: the constants the hand-kept copies
// must share", string-compares those constants across the three copies that live
// in files of their own. The four embedded as C++ string literals are out of its
// reach — change one, change all seven, and run that test.
cbuffer GiShadowCB : register(b0)
{
    float4 uSunDirRadius; // xyz = direction TOWARD the light, w = angular radius (radians)
    float4 uFrame;        // x = jitter seed, y = tex width, z = tex height
    float4 uLocalPosRange[4]; // xyz = local (point/spot) light position, w = range
    float4 uLocalExtra;       // x = local light count, y = sun rays per pixel
};
Texture2D<float4>   uGPos     : register(t0);
Texture2D<float4>   uGNorm    : register(t1);
RWTexture2D<float>  uOut      : register(u0);
RWTexture2D<float4> uOutLocal : register(u1); // per-pixel local-light visibility (1 channel per light, first 4)
RaytracingAccelerationStructure uTlas : register(t7);

// The TILE landscape's height clipmap (Thema 326): its ground is no mesh, so it is in no BLAS, and the TLAS alone would let
// the sun through every mountain. Off (uHfParams.w == 0) it answers false. R32F texture, mips = conservative max pyramid
// (GiHeightfield.h). The march is kGiHeightfieldHLSL's (HlslSources.h), kept in step with it.
cbuffer GiHfCB : register(b2)
{
    float4x4 uHfWorldToLocal;
    float4   uHfParams;   // x = 1/sizeX, y = 1/sizeZ, z = res-1 (texels per axis), w = enabled
    float4   uHfParams2;  // x = top mip level, y = max height, z = min height, w = rays march this many steps at most
};
Texture2D<float> uHf : register(t5);

float hfBilinear(float2 tp)
{
    float2 f  = clamp(tp, 0.0, uHfParams.z);
    int2   i0 = int2(floor(f));
    int2   i1 = min(i0 + 1, int2(uHfParams.zz));
    float2 w  = f - float2(i0);
    float h00 = uHf.Load(int3(i0.x, i0.y, 0));
    float h10 = uHf.Load(int3(i1.x, i0.y, 0));
    float h01 = uHf.Load(int3(i0.x, i1.y, 0));
    float h11 = uHf.Load(int3(i1.x, i1.y, 0));
    return lerp(lerp(h00, h10, w.x), lerp(h01, h11, w.x), w.y);
}

bool hfAnyHit(float3 o, float3 d, float tMin, float tMax)
{
    if (uHfParams.w < 0.5) return false;
    float3 oL = mul(uHfWorldToLocal, float4(o, 1.0)).xyz;
    float3 dL = mul((float3x3)uHfWorldToLocal, d);
    float2 texPerLocal = float2(uHfParams.x, uHfParams.y) * uHfParams.z;      // local XZ -> texels
    float2 tp0 = (oL.xz * float2(uHfParams.x, uHfParams.y) + 0.5) * uHfParams.z;
    float2 tpd = dL.xz * texPerLocal;                                          // texels per unit t
    float  texLen = max(length(tpd), 1e-6);

    // t range inside the field's footprint and below its highest point.
    float t0 = tMin, t1 = tMax;
    float2 inv = 1.0 / (abs(tpd) + 1e-9) * sign(tpd + 1e-12);
    float2 ta  = (0.0 - tp0) * inv, tb = (uHfParams.z - tp0) * inv;
    float2 tlo = min(ta, tb), thi = max(ta, tb);
    t0 = max(t0, max(tlo.x, tlo.y));
    t1 = min(t1, min(thi.x, thi.y));
    float maxH = uHfParams2.y;
    if (abs(dL.y) > 1e-9)
    {
        float tH = (maxH - oL.y) / dL.y;
        if (dL.y < 0.0) t0 = max(t0, tH); else t1 = min(t1, tH);
    }
    else if (oL.y > maxH) return false;
    if (t0 >= t1) return false;

    // Lift a start that lies a little under the field (a LOD mesh), but not one that is metres below the ground.
    float2 tpS = tp0 + tpd * t0;
    float  hS  = hfBilinear(tpS);
    float  yS  = oL.y + dL.y * t0;
    float  lift = (hS - yS) < 8.0 ? max(0.0, hS + 0.15 - yS) : 0.0;

    int    topLvl = int(uHfParams2.x);
    int    lvl    = min(2, topLvl);
    float  t      = t0;
    int    steps  = int(uHfParams2.w);
    for (int it = 0; it < steps; ++it)
    {
        if (t >= t1) return false;
        float2 tp = tp0 + tpd * t;
        float  y  = oL.y + dL.y * t + lift;
        int2   cell = int2(floor(tp)) >> lvl;
        cell = min(cell, max(1, (int(uHfParams.z) + 1) >> lvl) - 1);   // the last texel of this mip
        float  m  = uHf.Load(int3(cell.x, cell.y, lvl));
        float  cellTex = float(1 << lvl);
        float  dt = 0.5 * cellTex / texLen;
        float  yEnd = y + dL.y * dt;
        if (min(y, yEnd) > m + 0.02)
        {
            t += dt;
            lvl = min(lvl + 1, topLvl);
        }
        else if (lvl > 0)
        {
            lvl -= 1;
        }
        else
        {
            if (y < hfBilinear(tp) - 0.02) return true;
            t += 0.5 / texLen;
        }
    }
    return false;
}

// HW analogue of the SW BVH traversal: opaque first-hit query over the TLAS, then the height clipmap.
// FORCE_OPAQUE matches the SW kernel (no any-hit shading); the instance descs
// additionally set TRIANGLE_CULL_DISABLE (the SW Moeller-Trumbore is two-sided).
bool giSceneAnyHit(float3 o, float3 d, float tMin, float tMax)
{
    RayDesc ray;
    ray.Origin    = o;
    ray.Direction = d;
    ray.TMin      = tMin;
    ray.TMax      = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_FORCE_OPAQUE |
             RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(uTlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) { }
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) return true;
    return hfAnyHit(o, d, tMin, tMax);
}

// Cone-jitter random numbers: a per-pixel offset from a PCG3D integer hash
// (Jarzynski & Olano 2020) plus an R2 low-discrepancy step per frame, so each
// pixel walks the light disk evenly over the frames the temporal pass averages.
// The host wraps the frame index to [0, 1024) (GIJitter.h), so seed stays small
// and exact. The old fract(sin(seed * ...)) hash went constant once the never-
// reset float seed passed ~1e5 (Thema 131) - keep every copy on this one.
uint3 giPcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
float2 giHash2(uint2 gid, float seed)
{
    uint3 h = giPcg3d(uint3(gid, 0u));
    return frac(float2(h.xy >> 8u) * (1.0 / 16777216.0) + seed * float2(0.7548776662, 0.5698402910));
}
float3 giConeSample(float3 L, float angleRad, float2 xi)
{
    float3 up = (abs(L.y) < 0.99) ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);
    float r   = sin(angleRad) * sqrt(xi.x);
    float phi = 6.28318530718 * xi.y;
    return normalize(L + T * (r * cos(phi)) + B * (r * sin(phi)));
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    if (float(gid.x) >= uFrame.y || float(gid.y) >= uFrame.z) return;
    float4 pv = uGPos.Load(int3(gid.xy, 0));
    if (pv.a < 0.5) // background -> everything unoccluded
    {
        uOut[gid.xy]      = 1.0;
        uOutLocal[gid.xy] = float4(1.0, 1.0, 1.0, 1.0);
        return;
    }
    float3 N = normalize(uGNorm.Load(int3(gid.xy, 0)).xyz);
    float3 L = uSunDirRadius.xyz;

    // -- Directional light (cone-jittered, temporally accumulated) --
    float sunVis = 0.0;
    // Grazing/back-facing relative to the light: direct lighting's dot(N,L)
    // term already zeroes this out, so skip the trace entirely.
    if (dot(N, L) > 0.0)
    {
        // spp rays, averaged; sample k at seed*spp + k (see gi_shadow.comp).
        uint spp = uint(max(uLocalExtra.y, 1.0));
        for (uint k = 0u; k < spp; ++k)
        {
            float2 xi  = giHash2(gid.xy, uFrame.x * float(spp) + float(k));
            float3 dir = giConeSample(L, max(uSunDirRadius.w, 1e-4), xi);
            // Same self-intersection guards as the SW kernel: normal-offset origin + min t.
            float3 origin = pv.xyz + N * 0.05;
            sunVis += giSceneAnyHit(origin, dir, 0.02, 10000.0) ? 0.0 : 1.0;
        }
        sunVis /= float(spp);
    }
    uOut[gid.xy] = sunVis;

    // -- Local (point/spot) lights: one HARD occlusion ray each toward the
    // first 4 (see the Metal kernels) -- deliberately UNjittered: deterministic,
    // no temporal pass, one visibility channel per light; the scene shader
    // indexes by its local-light counter.
    float4 localVis = float4(1.0, 1.0, 1.0, 1.0);
    int localCount = clamp(int(uLocalExtra.x), 0, 4);
    // Fixed-trip unrolled loop (i is a literal per iteration) so the dynamic
    // vector-component write localVis[i] stays FXC/SM5.0-safe.
    [unroll] for (int i = 0; i < 4; ++i)
    {
        if (i >= localCount) break;
        float3 toL   = uLocalPosRange[i].xyz - pv.xyz;
        float  distL = length(toL);
        if (distL <= 0.05) continue; // on top of the light -> lit
        if (distL >= uLocalPosRange[i].w) continue; // outside the attenuation radius -> contributes nothing, skip the ray
        float3 dirL = toL / distL;
        if (dot(N, dirL) <= 0.0) { localVis[i] = 0.0; continue; }
        if (giSceneAnyHit(pv.xyz + N * 0.05, dirL, 0.02, max(distL - 0.1, 0.02)))
            localVis[i] = 0.0;
    }
    uOutLocal[gid.xy] = localVis;
}
