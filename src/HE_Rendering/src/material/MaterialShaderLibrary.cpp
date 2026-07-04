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
constexpr const char* kStandardVertexGlsl = R"(#version 450
layout(std430, set = 0, binding = 0) readonly buffer Verts { float d[]; };
layout(std140, set = 0, binding = 1) uniform U {
    mat4 mvp; mat4 model; vec4 color; vec4 flags; vec4 pbr;
} u;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
void main() {
    int b = gl_VertexIndex * 8;
    vec3 pos = vec3(d[b + 0], d[b + 1], d[b + 2]);
    vec3 nrm = vec3(d[b + 3], d[b + 4], d[b + 5]);
    gl_Position = u.mvp * vec4(pos, 1.0);
    vNormal = mat3(u.model) * nrm;
    vColor  = u.color.rgb;
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
} // namespace

const char* MaterialShaderLibrary::standardVertexGlsl() { return kStandardVertexGlsl; }

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
        // Pin so the vertex buffer lands at [[buffer(0)]] and Uniforms at [[buffer(1)]] —
        // the exact bind points the Metal geometry loop issues per draw.
        out = toCompiled(compileMslPinned(kStandardVertexGlsl, Stage::Vertex,
            { { Stage::Vertex, 0, 0, 0 }, { Stage::Vertex, 0, 1, 1 } }));
    }
    else
    {
        out = toCompiled(compile(kStandardVertexGlsl, Stage::Vertex, toTarget(backend)));
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
    Compiled out = toCompiled(compile(glsl, Stage::Fragment, toTarget(backend)));
    return m_fragCache.emplace(key, std::move(out)).first->second;
}
} // namespace HE
