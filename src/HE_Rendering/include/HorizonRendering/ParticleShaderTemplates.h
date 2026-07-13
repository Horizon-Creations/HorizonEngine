#pragma once
// Fixed vertex/fragment shader TEMPLATES for GPU-instanced ParticleGraph particle
// rendering (see HE::generateParticleShaderSource) — the actual scene draw path,
// distinct from the isolated offscreen RenderParticlePreview shaders (which take
// pre-lerped per-instance color/alpha; these take raw per-instance data and lerp
// on the GPU via the baked/generated heParticleColor/heParticleAlpha functions).
//
// Header-only: RendererOpenGL/RendererMetal only reuse HorizonRendering's PUBLIC
// INCLUDE PATH (they don't link the HorizonRendering shared library — see
// HE_Rendering/CMakeLists.txt), so a .cpp here would need its own new linkage
// plumbing across every consumer (both backends + the editor's export-time
// compiler + he_tests). Pure string-building has no reason to pay that cost.
//
// Instance layout (5 floats/instance, matches RenderWorld::ParticleInstance):
//   position (vec3), size (float), t01 (float) — GL: attributes with divisor=1 at
//   locations 0-2; Metal: a `device Instance*` buffer indexed by [[instance_id]].
// Per-draw-call uniforms: viewProj, camera right/up (billboard basis), whether a
// base texture is bound. No per-particle color/alpha uniform — that's what the
// spliced-in heParticleColor/heParticleAlpha functions replace.
#include <ParticleGraph/ParticleGraph.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace HE
{

inline std::string buildParticleVertexGLSL(const std::string& colorFn, const std::string& alphaFn)
{
    return std::string(R"GLSL(
#version 410 core
layout(location = 0) in vec3  iPosition;
layout(location = 1) in float iSize;
layout(location = 2) in float iT01;

uniform mat4 uViewProj;
uniform vec3 uCamRight;
uniform vec3 uCamUp;

out vec2 vUV;
out vec4 vColorAlpha;

const vec2 kCorners[6] = vec2[](
    vec2(-0.5,-0.5), vec2(0.5,-0.5), vec2(0.5,0.5),
    vec2(-0.5,-0.5), vec2(0.5,0.5),  vec2(-0.5,0.5)
);

)GLSL") + colorFn + "\n" + alphaFn + R"GLSL(

void main()
{
    vec2 corner  = kCorners[gl_VertexID % 6];
    vec3 worldPos = iPosition + (uCamRight * corner.x + uCamUp * corner.y) * iSize;
    gl_Position  = uViewProj * vec4(worldPos, 1.0);
    vUV          = corner + vec2(0.5);
    vColorAlpha  = vec4(heParticleColor(iT01), heParticleAlpha(iT01));
}
)GLSL";
}

inline std::string buildParticleFragmentGLSL()
{
    return R"GLSL(
#version 410 core
in vec2 vUV;
in vec4 vColorAlpha;
out vec4 FragColor;
uniform bool      uHasTex;
uniform sampler2D uTex;
void main()
{
    vec4  texc  = uHasTex ? texture(uTex, vUV) : vec4(1.0);
    // No texture → soft circular sprite, matching RenderParticlePreview's fallback.
    float shape = uHasTex ? texc.a : smoothstep(1.0, 0.0, length(vUV * 2.0 - 1.0));
    vec4  result = vec4(vColorAlpha.rgb * texc.rgb, vColorAlpha.a * shape);
    if (result.a < 0.003) discard;
    FragColor = result;
}
)GLSL";
}

inline std::string buildParticleVertexMSL(const std::string& colorFn, const std::string& alphaFn)
{
    // Metal compiles vertex/fragment as two independent library sources (same
    // convention MaterialShaderLibrary uses for SPIRV-Cross output — see
    // GetOrBuildMaterialPipeline's vLib/fLib) — the [[stage_in]] VOut struct just
    // needs to match byte-for-byte between the two, which the duplicated struct
    // definition below guarantees. heParticleColor/heParticleAlpha are only called
    // from the vertex stage (the fragment stage only reads the interpolated
    // colorAlpha), so the fragment source needs no splice point at all.
    return std::string(R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Instance { packed_float3 position; float size; float t01; };
struct VOut { float4 position [[position]]; float2 uv; float4 colorAlpha; };

constant float2 kCorners[6] = {
    float2(-0.5,-0.5), float2(0.5,-0.5), float2(0.5,0.5),
    float2(-0.5,-0.5), float2(0.5,0.5),  float2(-0.5,0.5)
};

)MSL") + colorFn + "\n" + alphaFn + R"MSL(

vertex VOut heParticleGraphVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                   const device Instance* insts [[buffer(0)]],
                                   constant float4x4& viewProj [[buffer(1)]],
                                   constant float3&   camRight [[buffer(2)]],
                                   constant float3&   camUp    [[buffer(3)]])
{
    Instance inst = insts[iid];
    float2 corner = kCorners[vid % 6];
    float3 worldPos = float3(inst.position) + (camRight * corner.x + camUp * corner.y) * inst.size;
    VOut o;
    o.position   = viewProj * float4(worldPos, 1.0);
    o.uv         = corner + float2(0.5);
    o.colorAlpha = float4(heParticleColor(inst.t01), heParticleAlpha(inst.t01));
    return o;
}
)MSL";
}

inline std::string buildParticleFragmentMSL()
{
    return R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 position [[position]]; float2 uv; float4 colorAlpha; };

fragment float4 heParticleGraphFragment(VOut in [[stage_in]],
                                         constant bool& hasTex [[buffer(0)]],
                                         texture2d<float> tex [[texture(0)]],
                                         sampler samp [[sampler(0)]])
{
    float4 texc  = hasTex ? tex.sample(samp, in.uv) : float4(1.0);
    float  shape = hasTex ? texc.a : smoothstep(1.0, 0.0, length(in.uv * 2.0 - 1.0));
    float4 result = float4(in.colorAlpha.rgb * texc.rgb, in.colorAlpha.a * shape);
    return result;
}
)MSL";
}

// Cache key for the on-demand-compiled shader path (RendererOpenGL::m_particlePrograms /
// RendererMetal::m_particlePipelineCache) — an FNV-1a fold over the 8 floats the
// generated shader actually varies on. Purely a runtime, in-memory, per-backend cache
// key: never serialized, unrelated to the export-time CHUNK_PPSD lookup (which is
// keyed by ParticleBatch::particleAssetId + backend, not this hash). A collision here
// only means two different color/alpha configs share a compiled program — never
// observed by the user as anything worse than one skipped recompile, so a fast,
// simple, non-cryptographic hash is the right tool.
inline uint64_t hashParticleShaderConfig(const ParticleEmitterConfig& c)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    auto mix = [&](float f) {
        uint32_t bits; std::memcpy(&bits, &f, sizeof(bits));
        h = (h ^ bits) * 1099511628211ull; // FNV-1a prime
    };
    mix(c.startColor[0]); mix(c.startColor[1]); mix(c.startColor[2]);
    mix(c.endColor[0]);   mix(c.endColor[1]);   mix(c.endColor[2]);
    mix(c.startAlpha);    mix(c.endAlpha);
    return h;
}

} // namespace HE
