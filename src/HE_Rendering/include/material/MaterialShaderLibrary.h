// Backend-agnostic material shader layer.
//
// Turns a MaterialAsset's canonical-GLSL custom shader into per-backend source
// (MSL / HLSL / GLSL / SPIR-V), cached, so every renderer builds its own pipeline object
// from the SAME authored shader — the material looks identical on all backends
// (docs/material-system-design.md). All the shareable work (resolve → glslang → SPIRV-Cross)
// lives here; only the pipeline-object construction stays in each backend.
#pragma once

#include <Types/UUID.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager; // global namespace (HE_Core's ContentManager is not namespaced)

namespace HE
{
class MaterialShaderLibrary
{
public:
    enum class Backend { Metal, HLSL, GLSL410, GLSLES300, SpirV };

    // Compact, stable shading input for material pipelines — the "material lighting ABI".
    // The engine fills this each frame; the standard-lit preamble's heLit() reads it.
    // std140: three vec4 = 48 bytes. Each backend binds it at its lighting slot (Metal:
    // fragment [[buffer(kMetalLightingBufferIndex)]]). A UBO (GL 3.1+), so unlike the SSBO
    // vertex it is fully GL-4.1 portable.
    struct Lighting
    {
        float sunDir[4]   = { 0.0f, 1.0f, 0.0f, 0.0f }; // xyz = direction TO the sun
        float sunColor[4] = { 1.0f, 1.0f, 1.0f, 0.0f }; // rgb = sun radiance
        float ambient[4]  = { 0.1f, 0.1f, 0.1f, 0.0f }; // rgb = ambient/sky fill
    };
    static constexpr int kMetalLightingBufferIndex = 1; // fragment [[buffer(1)]]

    struct Compiled
    {
        bool                  ok = false;
        std::string           source; // MSL/HLSL/GLSL text (empty for SpirV)
        std::vector<uint32_t> spirv;  // populated for the SpirV backend
        std::string           log;    // diagnostics on failure
    };

    // The shared "drop-in" vertex: vertex-pulls the interleaved pos3/normal3/uv2 vertex and
    // reads the per-object Uniforms UBO. Every material fragment is spliced onto this, so
    // all materials share the geometry pass's per-draw bindings. Fragment interface:
    //   layout(location = 0) in vec3 vNormal;  layout(location = 1) in vec3 vColor;
    //   layout(location = 0) out vec4 oColor;
    static const char* standardVertexGlsl();

    // True + (hash, glsl) if the material carries a custom shader; false → built-in PBR.
    bool resolveFragment(const ContentManager& cm, const UUID& materialId,
                         uint64_t& hashOut, std::string& glslOut) const;

    // Cross-compile, cached. The Metal backend pins the vertex to verts@0 / Uniforms@1 so
    // it drops into the fixed geometry-pass bind points; other backends use their natural
    // binding model. `sourceHash` keys the fragment cache (identical shaders share a slot).
    const Compiled& standardVertex(Backend backend);
    const Compiled& fragment(uint64_t sourceHash, const std::string& glsl, Backend backend);

    void clear() { m_vertCache.clear(); m_fragCache.clear(); }

private:
    std::unordered_map<int, Compiled>      m_vertCache; // key = (int)backend
    std::unordered_map<uint64_t, Compiled> m_fragCache; // key = mix(sourceHash, backend)
};
} // namespace HE
