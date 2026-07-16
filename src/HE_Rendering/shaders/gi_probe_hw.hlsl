// DDGI probe update — HARDWARE ray tracing variant (DXR 1.1 inline RayQuery,
// SM 6.5, compiled offline by dxc — FXC cannot target SM 6). Identical binding
// interface to the embedded SW kernel (kGiProbeCSHLSL12 in D3D12Renderer.cpp:
// b0/b1, t4 giInsts, t5/t6 prev atlases, u0/u1 unchanged; giNodes/giTris at
// t2/t3 are simply not declared — they are never traversed here), plus the
// TLAS as a root SRV at t7. Ray semantics mirror the SW kernel 1:1: gather
// formulation, one thread per octahedral texel, same bias/tMin/tMax constants,
// secondary occlusion rays for sun + local-light bounce. The host guarantees
// the TLAS instance order equals the giInsts order AND writes InstanceID = i
// in every D3D12_RAYTRACING_INSTANCE_DESC, so CommittedInstanceID() indexes
// giInsts directly.
cbuffer GiProbeCB : register(b0)
{
    float4 uGridOrigin;   // xyz = grid origin, w = spacing
    float4 uGridCounts;   // xyz = probe counts, w = probesPerRow
    float4 uRayParams;    // x = max dist, y = hysteresis, z = cursor start, w = probes this batch
    float4 uSunDirRadius; // xyz = direction TOWARD the light, w = local light count
    float4 uSunColor;     // rgb = colour * intensity
    float4 uSkyAmbient;   // rgb = miss colour
    float4 uLightPosRange[8];  // xyz pos, w range
    float4 uLightColorType[8]; // rgb colour*intensity, w type (1 point, 2 spot)
    float4 uLightDirCos[8];    // xyz spot travel dir, w cos(half angle)
};
struct GiInst { float4x4 invTransform; float4 baseColor; int4 offsets; };
StructuredBuffer<GiInst> giInsts : register(t4); // hit instance's baseColor
Texture2D<float4>    uIrrPrev : register(t5);
Texture2D<float2>    uVisPrev : register(t6);
RWTexture2D<float4>  uIrr     : register(u0);
RWTexture2D<float2>  uVis     : register(u1);
RaytracingAccelerationStructure uTlas : register(t7);

static const int kOctSize = 8; // must match the host's kGiProbeOctSize

// Any-hit over the TLAS (occlusion only) — for the secondary shadow rays.
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

// Committed closest hit over the TLAS; the returned instance index equals the
// giInsts index (the host writes InstanceID = giInsts index per instance).
int giSceneClosestHit(float3 o, float3 d, float tMin, float tMax, out float tOut)
{
    RayDesc ray;
    ray.Origin    = o;
    ray.Direction = d;
    ray.TMin      = tMin;
    ray.TMax      = tMax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(uTlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) { }
    tOut = tMax;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        tOut = q.CommittedRayT();
        return int(q.CommittedInstanceID());
    }
    return -1;
}

float3 octDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        float2 signN = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * signN;
    }
    return normalize(n);
}

[numthreads(8, 8, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    int2 texel    = int2(gtid.xy);
    int  batchIdx = int(groupId.x);
    int gx = int(uGridCounts.x), gy = int(uGridCounts.y), gz = int(uGridCounts.z);
    int probeCount = gx * gy * gz;
    if (probeCount <= 0 || batchIdx >= int(uRayParams.w)) return;
    int probeIndex = (int(uRayParams.z) + batchIdx) % probeCount;

    int pz = probeIndex / (gx * gy);
    int py = (probeIndex / gx) % gy;
    int px = probeIndex % gx;
    float3 probePos = uGridOrigin.xyz + float3(px, py, pz) * uGridOrigin.w;

    float2 uv  = (float2(texel) + 0.5) / float(kOctSize) * 2.0 - 1.0;
    float3 dir = octDecode(uv);

    float dist;
    int hitInst = giSceneClosestHit(probePos, dir, 0.01, max(uRayParams.x, 1.0), dist);

    float3 radiance;
    if (hitInst < 0)
    {
        radiance = uSkyAmbient.rgb;
        dist     = uRayParams.x;
    }
    else
    {
        float3 albedo    = giInsts[hitInst].baseColor.rgb;
        float3 hitNormal = -dir;
        float3 hitPos    = probePos + dir * dist;
        float ndl = max(dot(hitNormal, uSunDirRadius.xyz), 0.0);
        // Secondary shadow ray — hit surfaces are NOT assumed fully sun-lit
        // (otherwise probes flood shadowed regions with bright sun bounce).
        if (ndl > 0.0 && giSceneAnyHit(hitPos + hitNormal * 0.05, uSunDirRadius.xyz, 0.02, 10000.0))
            ndl = 0.0;
        radiance = albedo * uSunColor.rgb * ndl;
        int lightCount = int(uSunDirRadius.w);
        for (int i = 0; i < lightCount; ++i)
        {
            float3 toL = uLightPosRange[i].xyz - hitPos;
            float d    = max(length(toL), 1e-4);
            float range = max(uLightPosRange[i].w, 1e-4);
            if (d >= range) continue;
            float3 L = toL / d;
            float ndl2 = max(dot(hitNormal, L), 0.0);
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
            if (giSceneAnyHit(hitPos + hitNormal * 0.05, L, 0.02, max(d - 0.1, 0.02)))
                continue;
            radiance += albedo * uLightColorType[i].rgb * ndl2 * atten;
        }
    }

    int probesPerRow = max(1, int(uGridCounts.w));
    int2 outCoord = int2((probeIndex % probesPerRow) * kOctSize + texel.x,
                         (probeIndex / probesPerRow) * kOctSize + texel.y);

    // Adaptive hysteresis: deterministic gather rays -> deltas are real scene
    // changes; converge fast on change, stay smooth otherwise. Previous values
    // arrive as SRV copies (same convention as the SW kernel — typed UAV loads
    // of RGBA16F/RG16F stay optional even on DXR-class hardware).
    float baseH = clamp(uRayParams.y, 0.0, 0.98);
    float4 oldIrr = uIrrPrev.Load(int3(outCoord, 0));
    float hIrr = lerp(baseH, 0.3, saturate(length(radiance - oldIrr.rgb) * 4.0));
    uIrr[outCoord] = float4(lerp(radiance, oldIrr.rgb, hIrr), 1.0);
    float2 oldVis = uVisPrev.Load(int3(outCoord, 0));
    float2 newVisSample = float2(dist, dist * dist);
    float hVis = lerp(baseH, 0.3, saturate(abs(dist - oldVis.x) / max(uGridOrigin.w, 1.0)));
    uVis[outCoord] = lerp(newVisSample, oldVis.xy, hVis);
}
