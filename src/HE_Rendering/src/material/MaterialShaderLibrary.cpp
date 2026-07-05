#include "material/MaterialShaderLibrary.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include "ShaderCompiler.h" // he::shaderc

#include <functional>

namespace HE
{
namespace
{
// The shared standard drop-in vertex (moved out of MetalRenderer). Vertex-pulls the
// interleaved 32-byte VertexIn (pos3, normal3, uv2 = 8 floats) as a flat std430 float
// array indexed by gl_VertexIndex, and reads the Uniforms UBO (std140, matching the
// engine's per-object Uniforms). This is the M2 surface-template's vertex stage in embryo.
// Metal/Vulkan/D3D vertex: pulls the interleaved VertexIn as a flat std430 SSBO indexed
// by gl_VertexIndex (mirrors the engine's `const device VertexIn*` binding). SSBOs are
// GL 4.3+, so this variant is NOT used for the macOS-GL (4.1) path — see kStandardVertexAttrib.
constexpr const char* kStandardVertexSSBO = R"(#version 450
layout(std430, set = 0, binding = 0) readonly buffer Verts { float d[]; };
layout(std140, set = 0, binding = 1) uniform U {
    mat4 mvp; mat4 model; vec4 color; vec4 flags; vec4 pbr;
} u;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vWorldPos;
void main() {
    int b = gl_VertexIndex * 8;
    vec3 pos = vec3(d[b + 0], d[b + 1], d[b + 2]);
    vec3 nrm = vec3(d[b + 3], d[b + 4], d[b + 5]);
    vec4 wp  = u.model * vec4(pos, 1.0);
    gl_Position = u.mvp * vec4(pos, 1.0);
    vNormal   = mat3(u.model) * nrm;
    vColor    = u.color.rgb;
    vUV       = vec2(d[b + 6], d[b + 7]);
    vWorldPos = wp.xyz;
}
)";

// GL-4.1-portable vertex: real vertex attributes (no SSBO), so it compiles on a GLSL 410
// core context. The GL backend feeds pos/normal/uv via a VAO (locations 0/1/2, the same
// interleaved 32-byte layout). Same varyings + Uniforms UBO as the SSBO variant, so the
// shared fragments are identical across backends — only the vertex data path differs.
constexpr const char* kStandardVertexAttrib = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(std140, set = 0, binding = 1) uniform U {
    mat4 mvp; mat4 model; vec4 color; vec4 flags; vec4 pbr;
} u;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vWorldPos;
void main() {
    vec4 wp = u.model * vec4(aPos, 1.0);
    gl_Position = u.mvp * vec4(aPos, 1.0);
    vNormal   = mat3(u.model) * aNormal;
    vColor    = u.color.rgb;
    vUV       = aUV;
    vWorldPos = wp.xyz;
}
)";

he::shaderc::Target toTarget(MaterialShaderLibrary::Backend b)
{
    using B = MaterialShaderLibrary::Backend;
    using T = he::shaderc::Target;
    switch (b)
    {
        case B::Metal:     return T::Msl;
        case B::HLSL:      return T::HlslSm50;
        case B::GLSL410:   return T::Glsl410;
        case B::GLSLES300: return T::GlslEs300;
        case B::SpirV:     return T::SpirvBinary;
    }
    return T::Msl;
}

MaterialShaderLibrary::Compiled toCompiled(he::shaderc::Result&& r)
{
    MaterialShaderLibrary::Compiled c;
    c.ok     = r.ok;
    c.source = std::move(r.source);
    c.spirv  = std::move(r.spirv);
    c.log    = std::move(r.log);
    return c;
}

// Standard-lit shader-library preamble, injected into every material fragment (after its
// #version). Provides the lighting UBO (matches HE::MaterialShaderLibrary::Lighting) and
// heLit() — the M2 "Standard Lit" shading a material calls instead of hand-rolling its own.
// Unused by raw fragments (glslang drops the UBO when heLit isn't called), so it's inert
// for the escape-hatch path. UBO (not SSBO) → GL-4.1 portable. Later: the node graph emits
// calls to these std-library functions.
constexpr const char* kLightingPreamble = R"(
layout(std140, set = 0, binding = 0) uniform HeLighting {
    vec4 sunDir;    // xyz = direction TO the sun (normalized); w = engine time (s)
    vec4 sunColor;  // rgb = sun radiance
    vec4 ambient;   // rgb = ambient / sky fill
    vec4 camPos;    // xyz = camera world position
} heLight;
vec3 heLit(vec3 baseColor, vec3 N, float metallic, float roughness) {
    vec3  L    = normalize(heLight.sunDir.xyz);
    vec3  n    = normalize(N);
    float ndl  = max(dot(n, L), 0.0);
    vec3  diff = baseColor * heLight.sunColor.rgb * ndl;
    vec3  amb  = baseColor * heLight.ambient.rgb;
    // cheap roughness-driven spec toward the sun (view ≈ +Z in this simple model)
    vec3  H    = normalize(L + vec3(0.0, 0.0, 1.0));
    float spec = pow(max(dot(n, H), 0.0), mix(4.0, 64.0, 1.0 - roughness)) * (1.0 - roughness);
    return amb + diff + heLight.sunColor.rgb * spec * mix(0.04, 1.0, metallic);
}
)";

// Insert the preamble right after the material's #version directive (GLSL requires
// #version to be the first token). If the source has none, prepend one.
std::string injectPreamble(const std::string& src)
{
    const size_t vpos = src.find("#version");
    if (vpos == std::string::npos)
        return std::string("#version 450\n") + kLightingPreamble + src;
    size_t eol = src.find('\n', vpos);
    if (eol == std::string::npos) eol = src.size() - 1;
    return src.substr(0, eol + 1) + kLightingPreamble + src.substr(eol + 1);
}
} // namespace

const char* MaterialShaderLibrary::standardVertexGlsl() { return kStandardVertexAttrib; }

bool MaterialShaderLibrary::resolveFragment(const ContentManager& cm, const UUID& materialId,
                                            uint64_t& hashOut, std::string& glslOut) const
{
    if (materialId == UUID{}) return false;
    const MaterialAsset* mat = cm.getMaterial(materialId);
    if (!mat || mat->customShaderFragGlsl.empty()) return false;
    glslOut = mat->customShaderFragGlsl;
    hashOut = std::hash<std::string>{}(glslOut);
    return true;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::standardVertex(Backend backend)
{
    const int key = static_cast<int>(backend);
    if (auto it = m_vertCache.find(key); it != m_vertCache.end()) return it->second;

    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
    {
        // SSBO vertex-pull, pinned so the vertex buffer lands at [[buffer(0)]] and Uniforms
        // at [[buffer(1)]] — the exact bind points the Metal geometry loop issues per draw.
        out = toCompiled(compileMslPinned(kStandardVertexSSBO, Stage::Vertex,
            { { Stage::Vertex, 0, 0, 0 }, { Stage::Vertex, 0, 1, 1 } }));
    }
    else
    {
        // GL/D3D/Vulkan: attribute-based vertex so macOS-GL (4.1, no SSBO) can compile it.
        out = toCompiled(compile(kStandardVertexAttrib, Stage::Vertex, toTarget(backend)));
    }
    return m_vertCache.emplace(key, std::move(out)).first->second;
}

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::fragment(
    uint64_t sourceHash, const std::string& glsl, Backend backend)
{
    // Mix the source hash with the backend so each backend gets its own cache slot
    // without the two ever colliding.
    const uint64_t key = sourceHash ^ (0x9E3779B97F4A7C15ULL * (static_cast<uint64_t>(backend) + 1));
    if (auto it = m_fragCache.find(key); it != m_fragCache.end()) return it->second;

    using namespace he::shaderc;
    const std::string injected = injectPreamble(glsl); // adds the lighting UBO + heLit()
    Compiled out;
    if (backend == Backend::Metal)
    {
        // Pin the lighting UBO to the fragment slot the engine binds it at (buffer 1;
        // SceneUniforms occupies fragment buffer 0 in the scene pass), and the material
        // texture (set 0, binding 2 in canonical GLSL) to texture/sampler 0 — the slot the
        // geometry loop already binds per draw (material/mesh texture + linear sampler).
        out = toCompiled(compileMslPinned(injected, Stage::Fragment,
            { { Stage::Fragment, 0, 0, static_cast<uint32_t>(kMetalLightingBufferIndex) },
              { Stage::Fragment, 0, 2, 0 },     // material texture → texture/sampler 0
              { Stage::Fragment, 0, 3, 2 } })); // HeParams UBO → fragment buffer 2
    }
    else
    {
        out = toCompiled(compile(injected, Stage::Fragment, toTarget(backend)));
    }
    return m_fragCache.emplace(key, std::move(out)).first->second;
}
} // namespace HE
