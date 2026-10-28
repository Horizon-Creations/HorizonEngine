// GI reflections - HARDWARE ray tracing variant (DXR 1.1 inline RayQuery, SM 6.5, compiled offline by dxc - FXC cannot
// target SM 6): kGiReflCSHLSL (HlslSources.h) with its two software traversals - the closest hit over the instance list
// and the any-hit of the sun / local-light shadow rays at the hit - replaced by ray queries over the TLAS the diffuse GI
// builds each frame. Everything else (cbuffer, t0/t1/t5/t6/t7..t12, u0, the hit shading, the landscape colour, the DDGI
// sampling, the cone jitter, the sky cube at t12) is the SW kernel's text; only the TLAS at t13 is new, a root SRV.
//
// The hit triangle: the host builds each HW BLAS from the SW BVH's triangle array in ITS order (non-indexed), so the
// query's primitive index is the triangle's number inside that mesh's slice of giTris[]; adding the instance's triOffset
// gives the very index the SW traversal returns, and giHitNormal() reads the same triangle. InstanceID is the position in
// giInsts[] (the reflection-only copy of the instance list, same order as the TLAS).
//
// SYNC: hand-kept copy of kGiReflCSHLSL, GENERATED once from it by scripts/gen_gi_refl_hw_hlsl.py; change the SW kernel
// and regenerate (the Vulkan twin is shaders/gi_refl_hw.comp).

struct GiNode { float4 d0; float4 d1; }; // d0.xyz bmin, d0.w leftFirst (int bits), d1.xyz bmax, d1.w triCount (int bits)
struct GiTri  { float4 v0; float4 v1; float4 v2; };
struct GiInst { float4x4 invTransform; float4 baseColor; int4 offsets; }; // offsets.x = nodeOffset, .y = triOffset
StructuredBuffer<GiNode> giNodes : register(t2);
StructuredBuffer<GiTri>  giTris  : register(t3);
StructuredBuffer<GiInst> giInsts : register(t4);
cbuffer GiCountCB : register(b1) { int4 uGiCount; }; // x = instance count


cbuffer GiReflCB : register(b0)
{
    float4 uCamPos;      // xyz = camera world position
    float4 uSunDir;      // xyz = direction TOWARD the sun, w = local light count
    float4 uSunColor;    // rgb = sun radiance * intensity
    float4 uAmbient;     // rgb = never-black floor applied at a hit
    float4 uGridOrigin;  // xyz = probe-grid origin, w = spacing
    float4 uGridCounts;  // xyz = probes per axis, w = probes per atlas row
    float4 uReflParams;  // x = max ray distance, y = max roughness, z = indirect intensity, w = probe atlases valid
    float4 uFrame;       // x = jitter seed, y = width, z = height, w = 1 -> glossy cone jitter
    float4 uRays;        // x = rays per glossy pixel (tier), z = 1 -> uSkyRefl holds this frame's sky
    float4 uLightPosRange[8];  // xyz pos, w range
    float4 uLightColorType[8]; // rgb colour*intensity, w type (1 point, 2 spot)
    float4 uLightDirCos[8];    // xyz spot travel dir, w cos(half angle)
};
Texture2D<float4>   uGPos   : register(t0); // prepass world position, a >= 1 valid geometry
Texture2D<float4>   uGNorm  : register(t1); // prepass world normal, a = receiver roughness
Texture2D<float4>   uGIIrr  : register(t5); // DDGI irradiance atlas
Texture2D<float2>   uGIVis  : register(t6); // DDGI visibility atlas
RWTexture2D<float4> uOut    : register(u0); // rgb radiance, a confidence
SamplerState        uLinSamp : register(s1); // linear + clamp

// Painted / auto landscape — HE::GiLandscape; host mirror GILandGpu (192 bytes). cfg: xy = 1/(sizeX,sizeZ),
// z = uvTiling, w = layer count (< 0 = auto landscape: layer = Grass/Dirt/Rock/Snow means, autoWet/autoSlope/
// autoSnow as in GiLandscape.h).
struct GiLand { float4x4 worldToLocal; float4 cfg; float4 layer[4]; float4 autoWet; float4 autoSlope; float4 autoSnow; };
StructuredBuffer<GiLand> giLands : register(t7);
Texture2D<float4> uLandW0 : register(t8);   // one weightmap per landscape, in giLands order
Texture2D<float4> uLandW1 : register(t9);
Texture2D<float4> uLandW2 : register(t10);
Texture2D<float4> uLandW3 : register(t11);
// The real sky (clouds, weather) around the camera for rays that miss, baked each frame (D3D12Renderer::renderSkyReflCube).
// uRays.z = 0: no bake, a miss returns nothing and the composite's cloudless cubemap shows instead.
TextureCube<float4> uSkyRefl : register(t12);

static const int kOctSize = 8; // must match the host's kGIProbeOctSize

// Auto landscape material at a hit: the graph's masks from the hit's slope and world height over the slices' mean
// colours. Mirrors HE::giAutoLandscapeAlbedo (GiLandscape.h) and the GL/Metal/Vulkan kernels line for line.
float3 giAutoLandAlbedo(GiLand L, float3 pos, float3 n)
{
    float slope = saturate(1.0 - n.y);
    float rs = L.autoSlope.x, rb = L.autoSlope.y;
    float dirt  = saturate(L.autoSlope.z + smoothstep(rs - rb, rs, slope));
    float3 ground = lerp(L.layer[0].rgb, L.layer[1].rgb, dirt);
    float rock  = smoothstep(rs, rs + rb, slope);
    float3 s1   = lerp(ground, L.layer[2].rgb, rock);
    float snow  = smoothstep(L.autoSnow.x, L.autoSnow.x + L.autoSnow.y, pos.y)
                * (1.0 - smoothstep(L.autoSnow.z, L.autoSnow.z + 0.1, slope));
    float3 s2   = lerp(s1, L.layer[3].rgb, snow);
    float pms   = L.autoSlope.w;
    float flat_ = (1.0 - smoothstep(0.2 * pms, pms, slope)) * (1.0 - snow);
    return lerp(s2, L.autoWet.rgb, L.autoWet.w * flat_);
}

// Closest hit with the triangle index (the shadow/probe kernels use the cheaper
// pair above and fake the normal with -rayDir; a mirror needs the real one).
RaytracingAccelerationStructure uTlas : register(t13);   // this frame's TLAS (instance ID = giInsts index), a root SRV

// Closest hit over the TLAS. Returns the instance (its giInsts index) or -1; `triOut` is the hit triangle's index into
// giTris[] (the instance's slice offset + the query's primitive index), `tOut` the distance along `d` (normalised by
// every caller, so it is a world distance). FORCE_OPAQUE matches the SW kernel (no any-hit shading); the instance descs
// set TRIANGLE_CULL_DISABLE (the SW Moeller-Trumbore is two-sided).
int giSceneClosestHitTri(float3 o, float3 d, float tMin, float tMax, out float tOut, out int triOut)
{
    RayDesc ray;
    ray.Origin    = o;
    ray.Direction = d;
    ray.TMin      = tMin;
    ray.TMax      = tMax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(uTlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) { }
    tOut   = tMax;
    triOut = -1;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return -1;
    int inst = int(q.CommittedInstanceID());
    tOut   = q.CommittedRayT();
    triOut = giInsts[inst].offsets.y + int(q.CommittedPrimitiveIndex());
    return inst;
}

// Any-hit (sun / local-light occlusion at the hit): the first opaque hit ends the query.
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
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
// World-space geometric normal of triangle `tri` on instance `inst`, facing
// AGAINST the incoming ray. The BVH triangles are object space, so the normal
// moves with the inverse-transpose of object->world, and invTransform IS
// world->object: transpose(M) * n == mul(n, M) in HLSL's row-vector form.
float3 giHitNormal(int inst, int tri, float3 rayDir)
{
    GiTri t = giTris[tri];
    float3 nL = cross(t.v1.xyz - t.v0.xyz, t.v2.xyz - t.v0.xyz);
    float3 nW = mul(nL, (float3x3)giInsts[inst].invTransform);
    float len = length(nW);
    if (len < 1e-12) return -rayDir;
    nW /= len;
    return (dot(nW, rayDir) > 0.0) ? -nW : nW;
}

// The hit's emissive: two halves per int lane (r,g in .z; b in .w's low half).
float3 giHitEmissive(int inst)
{
    uint z = asuint(giInsts[inst].offsets.z);
    uint w = asuint(giInsts[inst].offsets.w);
    return float3(f16tof32(z & 0xFFFFu), f16tof32(z >> 16u), f16tof32(w & 0xFFFFu));
}

// SYNC: the scene shader's giOctEncode / sampleDDGIIrradiance (the sampled
// variant WITH the Chebyshev visibility test) and the GL kernel's copy. Pinned
// by the "DDGI probe sampling on the shading side" subcase of test_culling.cpp.
float2 giOctEncode(float3 n)
{
    float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    float2 signP = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

float3 sampleDDGIIrradiance(float3 P, float3 N)
{
    int gx = int(uGridCounts.x), gy = int(uGridCounts.y), gz = int(uGridCounts.z);
    if (gx <= 0 || gy <= 0 || gz <= 0) return float3(0.0, 0.0, 0.0);
    int probesPerRow = max(1, int(uGridCounts.w));
    int probeRows    = int(ceil(float(gx * gy * gz) / float(probesPerRow)));
    float2 atlasSizeTexels = float2(float(probesPerRow), float(probeRows)) * float(kOctSize);
    float spacing = max(uGridOrigin.w, 1e-4);

    float3 gridSpace = (P - uGridOrigin.xyz) / spacing;
    float3 base      = floor(gridSpace);
    float3 fracP     = gridSpace - base;

    float3 sumColor  = float3(0.0, 0.0, 0.0);
    float  sumWeight = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float3 offs = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
        float3 cell = base + offs;
        if (any(cell < 0.0) || cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
            continue;
        int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;

        float3 trilinear = lerp(1.0 - fracP, fracP, offs);
        float weight = trilinear.x * trilinear.y * trilinear.z;
        if (weight <= 1e-5) continue;

        float3 probePos   = uGridOrigin.xyz + cell * spacing;
        float3 toProbe    = probePos - P;
        float  dist       = max(length(toProbe), 1e-4);
        float3 dirToProbe = toProbe / dist;

        weight *= max(0.05, dot(N, dirToProbe) * 0.5 + 0.5);

        float2 tileOrigin = float2(float(probeIndex % probesPerRow),
                                   float(probeIndex / probesPerRow)) * float(kOctSize);

        float2 visUV = (tileOrigin + (giOctEncode(-dirToProbe) * 0.5 + 0.5) * float(kOctSize)) / atlasSizeTexels;
        float2 visSample = uGIVis.SampleLevel(uLinSamp, visUV, 0).rg;
        float mean = visSample.x, mean2 = visSample.y;
        float variance = abs(mean2 - mean * mean);
        float chebyshev = 1.0;
        if (dist > mean)
        {
            float d = dist - mean;
            chebyshev = variance / (variance + d * d);
            chebyshev = chebyshev * chebyshev * chebyshev;
        }
        weight *= max(chebyshev, 0.05);

        float2 irrUV = (tileOrigin + (giOctEncode(N) * 0.5 + 0.5) * float(kOctSize)) / atlasSizeTexels;
        sumColor  += uGIIrr.SampleLevel(uLinSamp, irrUV, 0).rgb * weight;
        sumWeight += weight;
    }
    return sumColor / max(sumWeight, 1e-4);
}

// SYNC: the same hash/cone pair as kGiShadowCSHLSL (a separate compilation unit).
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
void main(uint3 gid3 : SV_DispatchThreadID)
{
    uint2 gid = gid3.xy;
    if (float(gid.x) >= uFrame.y || float(gid.y) >= uFrame.z) return;
    int2 px = int2(gid);

    // The result has the prepass' size, so the receiver is a plain texel fetch.
    float4 pv = uGPos.Load(int3(px, 0));
    // Background, or a receiver too rough to show a traced reflection: confidence
    // 0 means the composite keeps the sky-cube term, the right answer for both.
    if (pv.a < 0.5) { uOut[px] = float4(0.0, 0.0, 0.0, 0.0); return; }
    float4 nm    = uGNorm.Load(int3(px, 0));
    float  rough = nm.a;
    if (rough > uReflParams.y) { uOut[px] = float4(0.0, 0.0, 0.0, 0.0); return; }

    float3 N = nm.xyz; // already normalized by kGiGBufPS before it writes uGNorm
    float3 V = normalize(uCamPos.xyz - pv.xyz);
    float3 R = reflect(-V, N);
    // Near-mirrors keep ONE deterministic ray (a cone narrower than a pixel would
    // only dither the silhouette); rougher receivers spend the tier's rays.
    float coneW = (uFrame.w > 0.5 && rough > 0.08) ? rough * 0.5 : 0.0;
    int   rays  = (coneW > 1e-3) ? clamp(int(uRays.x), 1, 8) : 1;
    // Normal-offset origin + a min-t, the same self-intersection guards the
    // shadow kernel uses.
    float3 origin = pv.xyz + N * 0.05;

    float3 outRadiance = float3(0.0, 0.0, 0.0);
    float  outConf     = 0.0;
    [loop] for (int sIdx = 0; sIdx < rays; ++sIdx)
    {
        float3 Rs = R;
        if (coneW > 1e-3)
        {
            // The sample index goes into the hashed pixel id, not the seed: every
            // sample gets its own per-pixel offset under the same R2 frame step.
            float2 xi = giHash2(gid + uint2(0u, uint(sIdx) * 65536u), uFrame.x);
            Rs = giConeSample(R, coneW, float2((float(sIdx) + xi.x) / float(rays), xi.y));
            if (dot(Rs, N) <= 0.0) Rs = R;
        }

        float dist; int tri;
        int hitInst = giSceneClosestHitTri(origin, Rs, 0.02, max(uReflParams.x, 1.0), dist, tri);
        if (hitInst < 0)
        {
            // Miss -> the sky. With the bake it is a full sample (the receiver's confidence, like a hit at close range)
            // carrying the REAL sky, so a mirror shows clouds and a part-miss glossy lobe averages hit and sky; without it
            // nothing is added and the composite keeps the cloudless analytic cube. Mirrors the GL, Metal and Vulkan kernels.
            if (uRays.z > 0.5)
            {
                outRadiance += uSkyRefl.SampleLevel(uLinSamp, Rs, 0).rgb;
                outConf     += 1.0;
            }
            continue;
        }

        float3 hitPos = origin + Rs * dist;
        float3 hitN   = giHitNormal(hitInst, tri, Rs);
        float3 albedo = giInsts[hitInst].baseColor.rgb;
        // Landscape hit: the colour at THIS point instead of one flat colour for the whole terrain. A landscape is
        // a heightfield over an axis-aligned local rect whose mesh UVs are linear in it, so the hit POSITION
        // recovers the UV (no per-vertex UV in the BVH). An auto landscape (cfg.w < 0) has no paint: its colour
        // follows the hit's slope and height (hitN faces the ray). Mirrors the GL, Metal and Vulkan kernels.
        {
            int li = int(giInsts[hitInst].baseColor.a + 0.5) - 1;
            uint nLands, strideLands;
            giLands.GetDimensions(nLands, strideLands);
            if (li >= 0 && li < 4 && uint(li) < nLands)
            {
                GiLand L = giLands[li];
                if (L.cfg.w < -0.5)
                    albedo = giAutoLandAlbedo(L, hitPos, hitN);
                else if (int(L.cfg.w) > 0)
                {
                    float3 lp  = mul(L.worldToLocal, float4(hitPos, 1.0)).xyz;
                    float2 luv = (float2(lp.x, lp.z) * L.cfg.xy + 0.5) * L.cfg.z;
                    float4 lw  = (li == 0) ? uLandW0.SampleLevel(uLinSamp, luv, 0)
                               : (li == 1) ? uLandW1.SampleLevel(uLinSamp, luv, 0)
                               : (li == 2) ? uLandW2.SampleLevel(uLinSamp, luv, 0)
                                           : uLandW3.SampleLevel(uLinSamp, luv, 0);
                    float3 lsum = float3(0.0, 0.0, 0.0);
                    float  wsum = 0.0;
                    int layers = int(L.cfg.w);
                    for (int k = 0; k < layers && k < 4; ++k)
                    {
                        if (lw[k] <= 0.0) continue;
                        lsum += L.layer[k].rgb * lw[k];
                        wsum += lw[k];
                    }
                    albedo = (wsum > 1e-4) ? lsum / wsum : L.layer[0].rgb;
                }
            }
        }

        // Hit shading: emissive first (a glowing object shows in a mirror even
        // unlit; the probe field deliberately does not carry it), then the sun,
        // the local lights and the probe field.
        float3 radiance = giHitEmissive(hitInst);
        float ndl = max(dot(hitN, uSunDir.xyz), 0.0);
        if (ndl > 0.0 && dot(uSunColor.rgb, float3(1.0, 1.0, 1.0)) > 1e-5)
        {
            if (giSceneAnyHit(hitPos + hitN * 0.05, uSunDir.xyz, 0.02, 10000.0)) ndl = 0.0;
            radiance += albedo * uSunColor.rgb * ndl;
        }
        int lightCount = int(uSunDir.w);
        for (int i = 0; i < lightCount; ++i)
        {
            float3 toL = uLightPosRange[i].xyz - hitPos;
            float d  = max(length(toL), 1e-4);
            float range = max(uLightPosRange[i].w, 1e-4);
            if (d >= range) continue;
            float3 L = toL / d;
            float ndl2 = max(dot(hitN, L), 0.0);
            if (ndl2 <= 0.0) continue;
            float atten = 1.0 - d / range;
            atten *= atten;
            if (uLightColorType[i].w > 1.5)
            {
                float c       = dot(-L, normalize(uLightDirCos[i].xyz));
                float cosCone = uLightDirCos[i].w;
                atten *= smoothstep(cosCone, lerp(cosCone, 1.0, 0.2), c);
            }
            if (atten <= 0.0) continue;
            if (giSceneAnyHit(hitPos + hitN * 0.05, L, 0.02, max(d - 0.1, 0.02))) continue;
            radiance += albedo * uLightColorType[i].rgb * ndl2 * atten;
        }
        // Indirect: the probe field at the hit, what makes this reflection
        // "GI-based" rather than a second, differently lit scene. Only when the
        // atlases are filled; the flat floor is added either way.
        if (uReflParams.w > 0.5)
            radiance += albedo * sampleDDGIIrradiance(hitPos, hitN) * uReflParams.z;
        radiance += albedo * uAmbient.rgb;

        // Confidence fades toward the ray's far end (probe grid ends, geometry
        // outside the BVH): fading into the sky term is the graceful failure.
        float conf = 1.0 - smoothstep(uReflParams.x * 0.75, uReflParams.x, dist);
        outRadiance += radiance;
        outConf     += conf;
    }
    // Averaged over the rays traced: a missed sample adds nothing to either, so a
    // lobe half off the geometry gets a proportionally lower confidence.
    uOut[px] = float4(outRadiance / float(rays), outConf / float(rays));
}
