// GI shadow rays — HARDWARE ray tracing variant (DXR 1.1 inline RayQuery,
// SM 6.5, compiled offline by dxc — FXC cannot target SM 6). Identical binding
// interface to the embedded SW kernel (kGiShadowCSHLSL12 in D3D12Renderer.cpp:
// b0, t0/t1, u0 unchanged), plus the TLAS as a root SRV at t7. Ray semantics
// mirror the SW kernel 1:1: one cone-jittered any-hit ray per pixel toward the
// dominant directional light, same origin bias (N * 0.05) and tMin/tMax
// (0.02 / 10000). The host guarantees the TLAS instance order equals the
// giInsts order (irrelevant here — occlusion only — but kept as an invariant).
cbuffer GiShadowCB : register(b0)
{
    float4 uSunDirRadius; // xyz = direction TOWARD the light, w = angular radius (radians)
    float4 uFrame;        // x = jitter seed, y = tex width, z = tex height
};
Texture2D<float4>   uGPos  : register(t0);
Texture2D<float4>   uGNorm : register(t1);
RWTexture2D<float>  uOut   : register(u0);
RaytracingAccelerationStructure uTlas : register(t7);

// HW analogue of the SW BVH traversal: opaque first-hit query over the TLAS.
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
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float2 giHash2(uint2 gid, float seed)
{
    float2 p = float2(gid) + seed * 13.37;
    return float2(frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453),
                  frac(sin(dot(p, float2(39.3468, 11.1352))) * 24634.6345));
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
    if (pv.a < 0.5) { uOut[gid.xy] = 1.0; return; } // background
    float3 N = normalize(uGNorm.Load(int3(gid.xy, 0)).xyz);
    float3 L = uSunDirRadius.xyz;
    if (dot(N, L) <= 0.0) { uOut[gid.xy] = 0.0; return; }

    float2 xi  = giHash2(gid.xy, uFrame.x);
    float3 dir = giConeSample(L, max(uSunDirRadius.w, 1e-4), xi);
    // Same self-intersection guards as the SW kernel: normal-offset origin + min t.
    float3 origin = pv.xyz + N * 0.05;
    uOut[gid.xy] = giSceneAnyHit(origin, dir, 0.02, 10000.0) ? 0.0 : 1.0;
}
