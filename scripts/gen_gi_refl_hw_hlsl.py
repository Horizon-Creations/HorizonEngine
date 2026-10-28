#!/usr/bin/env python3
"""Regenerates src/HE_Rendering/shaders/gi_refl_hw.hlsl (the D3D12 DXR twin of the GI reflection kernel) from the
embedded SW kernel in src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h (kGiTraversalHLSL + kGiReflCSHLSL).
Run from the repository root after changing kGiReflCSHLSL; the offline dxc step (CMake: gi_refl_hw -> gi_refl_hw.cso)
picks the file up. tests/test_culling.cpp pins the hash/cone constants of every copy."""
import re
H = 'src/HE_Rendering/src/Backends/D3D_Shared/HlslSources.h'
src = open(H, encoding='utf-8', newline='').read().replace('\r\n', '\n')

def literal(name):
    a = src.index('inline constexpr const char* %s = R"HLSL(' % name)
    a = src.index('R"HLSL(', a) + len('R"HLSL(')
    b = src.index(')HLSL";', a)
    return src[a:b]

trav = literal('kGiTraversalHLSL')
refl = literal('kGiReflCSHLSL')

# 1. declarations of the traversal prefix only (structs, the three SRVs, the count CB) - the SW traversal is not used
decl_end = trav.index('// Möller-Trumbore')
decl = trav[:decl_end]

# 2. the reflection kernel without its SW traversal (giBlasHitTri ... giSceneClosestHitTri)
cut_a = refl.index('bool giBlasHitTri(')
# include the comment block above it if any
cut_b = refl.index('// World-space geometric normal of triangle')
refl_body = refl[:cut_a] + '@@HW_TRAVERSAL@@\n' + refl[cut_b:]

hw = '''RaytracingAccelerationStructure uTlas : register(t13);   // this frame's TLAS (instance ID = giInsts index), a root SRV

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
'''
refl_body = refl_body.replace('@@HW_TRAVERSAL@@\n', hw)

header = '''// GI reflections - HARDWARE ray tracing variant (DXR 1.1 inline RayQuery, SM 6.5, compiled offline by dxc - FXC cannot
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
'''
out = header + decl + refl_body
assert 'void GiReflCS(' in out
out = out.replace('void GiReflCS(', 'void main(')
open('src/HE_Rendering/shaders/gi_refl_hw.hlsl', 'w', encoding='utf-8', newline='').write(out.replace('\n', '\r\n'))
print('ok', len(out.splitlines()), 'lines')
