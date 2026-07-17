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
    // std140: four vec4 = 64 bytes. Each backend binds it at its lighting slot (Metal:
    // fragment [[buffer(kMetalLightingBufferIndex)]]). A UBO (GL 3.1+), so unlike the SSBO
    // vertex it is fully GL-4.1 portable.
    struct Lighting
    {
        float sunDir[4]   = { 0.0f, 1.0f, 0.0f, 0.0f }; // xyz = direction TO the sun; w = time (s)
        float sunColor[4] = { 1.0f, 1.0f, 1.0f, 0.0f }; // rgb = sun radiance
        float ambient[4]  = { 0.1f, 0.1f, 0.1f, 0.0f }; // rgb = ambient/sky fill
        float camPos[4]   = { 0.0f, 0.0f, 0.0f, 0.0f }; // xyz = camera world pos (ViewDir/Fresnel)
        // Full scene-light window (matches the built-in PBR shaders' 8-light
        // layout) — consumed by heLitP(); appended AFTER the legacy fields so
        // PRECOMPILED material blobs (old sun-only preamble) keep binding this
        // buffer with unchanged offsets.
        float lightPos[8][4]    = {}; // xyz = position,        w = type (0 dir / 1 point / 2 spot)
        float lightDir[8][4]    = {}; // xyz = travel direction, w = cos(spot half angle)
        float lightColor[8][4]  = {}; // rgb = colour,           w = intensity
        float lightParams[8][4] = {}; // x = range
        float counts[4]         = {}; // x = light count
        // Screen-space GI shadow inputs for heLitP(): xy = viewport size,
        // z = 1 when the GI masks are bound and valid this frame (0 → heLitP
        // skips the mask samples entirely; UI/preview passes leave this 0).
        float giParams[4]       = {};
        // CSM fallback (v2.2, append-only): when the GI masks are NOT valid,
        // heLitP shadows directional lights against the engine's cascaded shadow
        // map instead (binding 12 in the preamble). The fill site pre-bakes its
        // clip-space conventions into the matrices (Metal: depth remap + UV
        // y-flip), so the shared GLSL uses plain uv = p.xy*0.5+0.5 and z in
        // [0,1]. csmSplits.w = cascade count; 0 disables the path entirely
        // (backends without a cascade array simply leave these zeroed).
        float csmVP[3][16]      = {}; // per-cascade light view-proj, column-major
        float csmSplits[4]      = {}; // xyz = planar view-space far distances; w = count
        float camFwd[4]         = {}; // xyz = camera forward (planar cascade selection)
        // Local (point/spot) shadow atlas (append-only, v2.3): per-layer light
        // view-proj with the backend's clip conventions PRE-BAKED (like csmVP).
        // A light's first layer index rides in lightParams[i].y as layer+1 —
        // 0 = casts no shadow, so zero-initialised fills (previews, UI, D3D/
        // Vulkan) never sample the atlas. Spot = 1 layer, point = 6 cube-face
        // layers (+X −X +Y −Y +Z −Z, major-axis pick in the preamble).
        float localShadowVP[16][16] = {};
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

    // Like resolveFragment, but also returns the material's WPO vertex BODY ("" = use the
    // standard vertex) and folds it into the hash — a WPO material's pipeline is keyed on
    // fragment + vertex together, so permutations never collide.
    bool resolveShaders(const ContentManager& cm, const UUID& materialId,
                        uint64_t& hashOut, std::string& fragOut, std::string& vertBodyOut) const;

    // Cross-compile, cached. The Metal backend pins the vertex to verts@0 / Uniforms@1 so
    // it drops into the fixed geometry-pass bind points; other backends use their natural
    // binding model. `sourceHash` keys the fragment cache (identical shaders share a slot).
    const Compiled& standardVertex(Backend backend);
    const Compiled& fragment(uint64_t sourceHash, const std::string& glsl, Backend backend);

    // Custom vertex for World-Position-Offset materials: wraps the graph-generated BODY
    // (canonical statements ending in `vec3 heWpo`) into the per-backend vertex template
    // (SSBO vertex-pull on Metal, attributes elsewhere — same split as standardVertex).
    // The body reads the varying names (vNormal/vUV/vWorldPos/vColor, written first) plus
    // the HeLighting/HeParams UBOs, and the offset is applied in world space.
    const Compiled& customVertex(uint64_t bodyHash, const std::string& body, Backend backend);

    // Screen-space quad vertex for materials on IN-GAME UI elements: emits the
    // same varyings as standardVertex (so any material fragment drops in) from
    // an attribute-less 4-vertex strip. Repurposes the standard U block (same
    // bind point as the mesh path — Metal: vertex buffer 1):
    //   u.model[0] = rect  (x, y, w, h — pixels, top-left origin)
    //   u.model[1] = uvRect(u0, v0, u1, v1)
    //   u.model[2].xy = viewport (w, h in pixels)
    //   u.color   = tint (→ vColor)
    // vNormal = +Z, vWorldPos = (screen px, 0) — sane defaults for UI shading.
    const Compiled& uiVertex(Backend backend);

    void clear() { m_vertCache.clear(); m_fragCache.clear(); m_cvertCache.clear(); m_uiVertCache.clear(); }

private:
    std::unordered_map<int, Compiled>      m_vertCache;  // key = (int)backend
    std::unordered_map<uint64_t, Compiled> m_fragCache;  // key = mix(sourceHash, backend)
    std::unordered_map<uint64_t, Compiled> m_cvertCache; // key = mix(bodyHash, backend)
    std::unordered_map<int, Compiled>      m_uiVertCache; // key = (int)backend
};
} // namespace HE
