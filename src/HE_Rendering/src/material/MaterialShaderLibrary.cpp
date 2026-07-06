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

namespace
{
// Blocks the WPO body may reference (Time = heLight.sunDir.w, params). Vertex-stage
// bindings 8/9 avoid the fragment slots; Metal pins them to vertex buffers 2/3.
constexpr const char* kWpoUniforms = R"(layout(std140, set = 0, binding = 8) uniform HeLighting {
    vec4 sunDir; vec4 sunColor; vec4 ambient; vec4 camPos;
} heLight;
layout(std140, set = 0, binding = 9) uniform HeParams { vec4 v[16]; } heParams;
)";

// Noise helpers, duplicated for the vertex stage (the fragment injects its own copies).
// glslang dead-strips whatever the body doesn't call, so including them is free.
constexpr const char* kWpoNoise = R"(float heHash21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float heValueNoise(vec2 p) { vec2 i = floor(p); vec2 f = fract(p); vec2 u2 = f * f * (3.0 - 2.0 * f); float a = heHash21(i); float b = heHash21(i + vec2(1.0, 0.0)); float cc = heHash21(i + vec2(0.0, 1.0)); float d = heHash21(i + vec2(1.0, 1.0)); return mix(mix(a, b, u2.x), mix(cc, d, u2.x), u2.y); }
float heFbm(vec2 p) { float v = 0.0; float a = 0.5; for (int i = 0; i < 4; i++) { v += a * heValueNoise(p); p *= 2.0; a *= 0.5; } return v; }
float heHash31(vec3 p) { p = fract(p * 0.1031); p += dot(p, p.zyx + 31.32); return fract((p.x + p.y) * p.z); }
float heValueNoise3(vec3 p) { vec3 i = floor(p); vec3 f = fract(p); vec3 u3 = f * f * (3.0 - 2.0 * f); float n000 = heHash31(i); float n100 = heHash31(i + vec3(1.0, 0.0, 0.0)); float n010 = heHash31(i + vec3(0.0, 1.0, 0.0)); float n110 = heHash31(i + vec3(1.0, 1.0, 0.0)); float n001 = heHash31(i + vec3(0.0, 0.0, 1.0)); float n101 = heHash31(i + vec3(1.0, 0.0, 1.0)); float n011 = heHash31(i + vec3(0.0, 1.0, 1.0)); float n111 = heHash31(i + vec3(1.0, 1.0, 1.0)); float x00 = mix(n000, n100, u3.x); float x10 = mix(n010, n110, u3.x); float x01 = mix(n001, n101, u3.x); float x11 = mix(n011, n111, u3.x); return mix(mix(x00, x10, u3.y), mix(x01, x11, u3.y), u3.z); }
float heFbm3(vec3 p) { float v = 0.0; float a = 0.5; for (int i = 0; i < 4; i++) { v += a * heValueNoise3(p); p *= 2.0; a *= 0.5; } return v; }
)";

// Assemble the full canonical custom vertex around the graph body. The varyings are
// WRITTEN first so the body may read them by their usual names; the world-space offset
// is mapped back to object space with the transpose trick (exact for rigid transforms
// with uniform scale — model^-1 ≈ model^T / |col0|²), so u.mvp keeps working.
std::string buildCustomVertex(const std::string& body, bool ssbo)
{
    std::string src = "#version 450\n";
    if (ssbo)
        src += "layout(std430, set = 0, binding = 0) readonly buffer Verts { float d[]; };\n";
    else
        src += "layout(location = 0) in vec3 aPos;\n"
               "layout(location = 1) in vec3 aNormal;\n"
               "layout(location = 2) in vec2 aUV;\n";
    src += "layout(std140, set = 0, binding = 1) uniform U {\n"
           "    mat4 mvp; mat4 model; vec4 color; vec4 flags; vec4 pbr;\n"
           "} u;\n";
    src += kWpoUniforms;
    src += kWpoNoise;
    src += "layout(location = 0) out vec3 vNormal;\n"
           "layout(location = 1) out vec3 vColor;\n"
           "layout(location = 2) out vec2 vUV;\n"
           "layout(location = 3) out vec3 vWorldPos;\n"
           "void main() {\n";
    if (ssbo)
        src += "    int b = gl_VertexIndex * 8;\n"
               "    vec3 pos = vec3(d[b + 0], d[b + 1], d[b + 2]);\n"
               "    vec3 nrm = vec3(d[b + 3], d[b + 4], d[b + 5]);\n"
               "    vec2 uv  = vec2(d[b + 6], d[b + 7]);\n";
    else
        src += "    vec3 pos = aPos;\n    vec3 nrm = aNormal;\n    vec2 uv = aUV;\n";
    src += "    vec4 wp   = u.model * vec4(pos, 1.0);\n"
           "    vNormal   = mat3(u.model) * nrm;\n"
           "    vColor    = u.color.rgb;\n"
           "    vUV       = uv;\n"
           "    vWorldPos = wp.xyz;\n";
    src += body; // graph statements (read the varyings above) → `vec3 heWpo`
    src += "    vWorldPos += heWpo;\n"
           "    vec3 heObjWpo = (transpose(mat3(u.model)) * heWpo)\n"
           "                  / max(dot(u.model[0].xyz, u.model[0].xyz), 1e-8);\n"
           "    gl_Position = u.mvp * vec4(pos + heObjWpo, 1.0);\n"
           "}\n";
    return src;
}
} // namespace

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::customVertex(
    uint64_t bodyHash, const std::string& body, Backend backend)
{
    const uint64_t key = bodyHash ^ (0xC2B2AE3D27D4EB4FULL * (static_cast<uint64_t>(backend) + 1));
    if (auto it = m_cvertCache.find(key); it != m_cvertCache.end()) return it->second;

    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
    {
        // verts@0, U@1 (the geometry pass's fixed binds) + HeLighting/HeParams pinned to
        // vertex buffers 2/3, which the renderer binds for WPO materials.
        out = toCompiled(compileMslPinned(buildCustomVertex(body, /*ssbo=*/true), Stage::Vertex,
            { { Stage::Vertex, 0, 0, 0 }, { Stage::Vertex, 0, 1, 1 },
              { Stage::Vertex, 0, 8, 2 }, { Stage::Vertex, 0, 9, 3 } }));
    }
    else
    {
        out = toCompiled(compile(buildCustomVertex(body, /*ssbo=*/false), Stage::Vertex,
                                 toTarget(backend)));
    }
    return m_cvertCache.emplace(key, std::move(out)).first->second;
}

bool MaterialShaderLibrary::resolveShaders(const ContentManager& cm, const UUID& materialId,
                                           uint64_t& hashOut, std::string& fragOut,
                                           std::string& vertBodyOut) const
{
    if (!resolveFragment(cm, materialId, hashOut, fragOut)) return false;
    vertBodyOut.clear();
    if (const MaterialAsset* mat = cm.getMaterial(materialId))
        vertBodyOut = mat->customShaderVertGlsl;
    if (!vertBodyOut.empty()) // fold the vertex into the pipeline key
        hashOut ^= std::hash<std::string>{}(vertBodyOut) * 0x9E3779B97F4A7C15ULL;
    return true;
}

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

namespace
{
// Attribute-less screen-space quad vertex for materials on in-game UI quads.
// Same varyings + U bind point as the standard vertex, so any cached material
// fragment links against it unchanged (see the header for the U field layout).
constexpr const char* kUIVertex = R"(#version 450
layout(std140, set = 0, binding = 1) uniform U {
    mat4 mvp; mat4 model; vec4 color; vec4 flags; vec4 pbr;
} u;
layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec3 vWorldPos;
void main() {
    vec2 c[4] = vec2[](vec2(0.0,0.0), vec2(1.0,0.0), vec2(0.0,1.0), vec2(1.0,1.0));
    vec2 corner = c[gl_VertexIndex];
    vec4 rect = u.model[0];
    vec4 uvr  = u.model[1];
    vec2 vp   = max(u.model[2].xy, vec2(1.0));
    vec2 sp   = rect.xy + corner * rect.zw;
    gl_Position = vec4(sp.x / vp.x * 2.0 - 1.0,
                       1.0 - sp.y / vp.y * 2.0, 0.0, 1.0);
    vNormal   = vec3(0.0, 0.0, 1.0);
    vColor    = u.color.rgb;
    vUV       = mix(uvr.xy, uvr.zw, corner);
    vWorldPos = vec3(sp, 0.0);
}
)";
} // namespace

const MaterialShaderLibrary::Compiled& MaterialShaderLibrary::uiVertex(Backend backend)
{
    const int key = static_cast<int>(backend);
    if (auto it = m_uiVertCache.find(key); it != m_uiVertCache.end()) return it->second;

    using namespace he::shaderc;
    Compiled out;
    if (backend == Backend::Metal)
    {
        // Pin U to vertex buffer 1 — the same slot the mesh path uses, so the
        // UI pass binds its repurposed U block at a familiar index.
        out = toCompiled(compileMslPinned(kUIVertex, Stage::Vertex,
            { { Stage::Vertex, 0, 1, 1 } }));
    }
    else
    {
        out = toCompiled(compile(kUIVertex, Stage::Vertex, toTarget(backend)));
    }
    return m_uiVertCache.emplace(key, std::move(out)).first->second;
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
              { Stage::Fragment, 0, 2, 0 },     // legacy/mesh texture → texture/sampler 0
              { Stage::Fragment, 0, 3, 2 },     // HeParams UBO → fragment buffer 2
              // Node-graph project textures heTexP0..3 (GLSL binding 4..7) → MSL
              // texture/sampler 1..4. Pinned unconditionally (harmless when unused).
              { Stage::Fragment, 0, 4, 1 },
              { Stage::Fragment, 0, 5, 2 },
              { Stage::Fragment, 0, 6, 3 },
              { Stage::Fragment, 0, 7, 4 } }));
    }
    else
    {
        out = toCompiled(compile(injected, Stage::Fragment, toTarget(backend)));
    }
    return m_fragCache.emplace(key, std::move(out)).first->second;
}
} // namespace HE
