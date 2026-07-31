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

    // Stable shading input for material pipelines — the "material lighting ABI".
    // The engine fills this each frame; the standard-lit preamble's heLit() reads the
    // first four vec4s, heLitP() the rest. std140, and append-only: it started as those
    // four vec4s and has grown by kilobytes since (localShadowVP alone is 16×mat4 = 1 KiB),
    // so the struct below is the only authority on its size — don't quote a byte count
    // here, it rots on the next append. Each backend binds it at its lighting slot (Metal:
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
        // Aerial perspective + the two "is this bound" gates for the shared
        // ambient inputs (append-only, v2.4):
        //   x = fog density, y = fog height falloff (EnvironmentSettings)
        //   z = 1 when heSkyEnv (the procedural sky cubemap) is bound and valid —
        //       heLitP's image-based ambient AND the fog colour both read it;
        //       0 makes both fall back (flat ambient, no fog), which is what the
        //       preview/UI passes want.
        //   w = 1 when heAO (the screen-space AO result) is bound and valid.
        float fog[4] = {};
        // DDGI probe grid (append-only, v2.5) — the SAME values the built-in
        // shaders' GIUniforms carries, so heLitP's indirect diffuse matches them:
        //   giGridOrigin : xyz = probe-grid origin, w = probe spacing
        //   giGridCounts : xyz = probes per axis,   w = probes per atlas row
        //   giProbe      : x = indirect intensity,  y = 1 when the probe atlases
        //                  (heGIIrradiance/heGIVisibility) are bound and valid
        float giGridOrigin[4] = {};
        float giGridCounts[4] = {};
        float giProbe[4]      = {};
        // Weather surface response (append-only, v2.6): x = wetness,
        // y = snow amount — the same EnvironmentComponent values the built-in
        // shaders read, so a graph material greys/glosses with the weather like
        // everything around it.
        float weather[4]      = {};
    };
    static constexpr int kMetalLightingBufferIndex = 1; // fragment [[buffer(1)]]

    struct Compiled
    {
        bool                  ok = false;
        std::string           source; // MSL/HLSL/GLSL text (empty for SpirV)
        std::vector<uint32_t> spirv;  // populated for the SpirV backend
        std::string           log;    // diagnostics on failure
    };

    // True + (hash, glsl) if the material carries a custom shader; false → built-in PBR.
    bool resolveFragment(const ContentManager& cm, const UUID& materialId,
                         uint64_t& hashOut, std::string& glslOut) const;

    // Like resolveFragment, but also returns the material's WPO vertex BODY ("" = use the
    // standard vertex) and folds it into the hash — a WPO material's pipeline is keyed on
    // fragment + vertex together, so permutations never collide.
    bool resolveShaders(const ContentManager& cm, const UUID& materialId,
                        uint64_t& hashOut, std::string& fragOut, std::string& vertBodyOut) const;

    // Deferred: same contract as resolveShaders, but returns the material's G-BUFFER
    // fragment variant (MaterialAsset::customShaderGBufGlsl — MRT emit tail instead of
    // heLitP). False when the material has no G-buffer variant (hand-written escape-hatch
    // GLSL, packaged material without a graph) — the deferred path then draws it through
    // the forward extra pass instead. The hash is of the G-buffer SOURCE (+ vertex fold),
    // so it never collides with the forward pipeline's key space.
    bool resolveGBufferShaders(const ContentManager& cm, const UUID& materialId,
                               uint64_t& hashOut, std::string& fragOut,
                               std::string& vertBodyOut) const;

    // Deferred lighting-resolve fragment: built from the SAME kLightingPreamble as every
    // material fragment and shades by calling heLitP on the G-buffer attributes — there
    // is deliberately no second shading implementation (docs/deferred-renderer-plan.md
    // §4.2). Canonical bindings (set 0): heGB0/1/2 = 19/20/21, heGBDepth = 22, HeResolve
    // UBO (invViewProj + depth/debug params) = 23; Metal pins: GB textures → fragment
    // texture/sampler 0..3, HeResolve → fragment buffer 3, everything else exactly like
    // fragment() (HeLighting → buffer 1, CSM/GI/sky/AO on their scene-pass slots).
    const Compiled& deferredResolve(Backend backend);

    // Attribute-less fullscreen-triangle vertex with NO varyings (the resolve fragment
    // reads gl_FragCoord); paired with deferredResolve for the fullscreen lighting draw.
    const Compiled& fullscreenVertex(Backend backend);

    // Tile-memory variant of deferredResolve (plan P6, Metal/Apple-Silicon only):
    // the G-buffer arrives as subpassInput 0..3 (GB0/GB1/GB2/NDC-depth), emitted
    // as [[color(n)]] framebuffer-fetch reads, and the lit colour goes to output
    // location 4 — the HDR attachment of the shared single pass. Same preamble,
    // same heLitP call; the G-buffer never leaves tile storage.
    const Compiled& deferredResolveTile(Backend backend);

    // std140 layout of the resolve shader's HeResolve UBO. depthParams:
    //   x = clip-space Y sign of the uv→NDC mapping (GL +1, Metal −1: its uv origin
    //       is top-left), y/z = NDC-z scale/bias from the sampled depth (GL 2/−1,
    //       Metal 1/0), w = debug view (0 off, 1 = BaseColor, 2 = Normal,
    //       3 = Rough/Spec/Metal, 4 = Emissive) — HE_DUMP_GBUFFER.
    struct ResolveUniforms
    {
        float invViewProj[16] = {}; // column-major inverse(proj * view)
        float depthParams[4]  = {};
    };

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

    void clear() { m_vertCache.clear(); m_fragCache.clear(); m_cvertCache.clear();
                   m_uiVertCache.clear(); m_resolveCache.clear(); m_resolveTileCache.clear();
                   m_fsVertCache.clear(); }

private:
    std::unordered_map<int, Compiled>      m_vertCache;  // key = (int)backend
    std::unordered_map<uint64_t, Compiled> m_fragCache;  // key = mix(sourceHash, backend)
    std::unordered_map<uint64_t, Compiled> m_cvertCache; // key = mix(bodyHash, backend)
    std::unordered_map<int, Compiled>      m_uiVertCache; // key = (int)backend
    std::unordered_map<int, Compiled>      m_resolveCache; // key = (int)backend
    std::unordered_map<int, Compiled>      m_resolveTileCache; // key = (int)backend
    std::unordered_map<int, Compiled>      m_fsVertCache;  // key = (int)backend
};
} // namespace HE
