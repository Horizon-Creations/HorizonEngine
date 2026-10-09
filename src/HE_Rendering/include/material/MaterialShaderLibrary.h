// Backend-agnostic material shader layer.
//
// Turns a MaterialAsset's canonical-GLSL custom shader into per-backend source
// (MSL / HLSL / GLSL / SPIR-V), cached, so every renderer builds its own pipeline object
// from the SAME authored shader — the material looks identical on all backends
// (docs/material-system-design.md). All the shareable work (resolve → glslang → SPIRV-Cross)
// lives here; only the pipeline-object construction stays in each backend.
#pragma once

#include <Types/Enums.h> // HE::RendererBackend (precompiledFor)
#include <Types/UUID.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager; // global namespace (HE_Core's ContentManager is not namespaced)
struct MaterialAsset;
struct MaterialShaderVariant;

namespace HE
{
class MaterialShaderLibrary
{
public:
    // GLSL430 = desktop GL 4.3+ (SSBOs): only the GL backend's clustered graph-material
    // programs use it (fragmentClustered + the matching vertex); everything else on GL
    // stays GLSL410, which macOS (GL 4.1) can still compile. Append-only — the caches
    // salt their keys with the enum value.
    enum class Backend { Metal, HLSL, GLSL410, GLSLES300, SpirV, GLSL430 };

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
        // The engine wind rides in the three spare w channels of this legacy prefix
        // (HE::FillMaterialWind): a new field would shift every offset after it,
        // and WPO vertex blobs precompiled before the vertex stage declared the
        // whole block (wpoLightingBlock) only see these four vec4s.
        // Zero = no wind, which is what previews and UI passes leave in place.
        float sunDir[4]   = { 0.0f, 1.0f, 0.0f, 0.0f }; // xyz = direction TO the sun; w = time (s)
        float sunColor[4] = { 1.0f, 1.0f, 1.0f, 0.0f }; // rgb = sun radiance; w = wind direction X
        float ambient[4]  = { 0.1f, 0.1f, 0.1f, 0.0f }; // rgb = ambient/sky fill; w = wind direction Z
        float camPos[4]   = { 0.0f, 0.0f, 0.0f, 0.0f }; // xyz = camera world pos (ViewDir/Fresnel); w = wind strength
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
        // SSR (append-only, v2.7, docs/ssr-plan.md §5): y = intensity (mix
        // against the sky cubemap), z = maxRoughness, w = 1 → heLitP SKIPS its
        // specular-IBL term (ambSpec) because a later reflection pass supplies
        // it (deferred path only — forward never sets this). x reserved for the
        // forward heSSR sampler gate (not wired in v1).
        float ssr[4]          = {};
        // Ray-traced GI reflections (append-only, v2.8, docs/gi-reflections-
        // plan.md): x = intensity (mix of the traced result against the sky
        // cubemap, UNDER the SSR mix — SSR wins where it has a hit), y = max
        // roughness. Only the reflection composite pass reads these; every
        // other fill site leaves them 0 (the heGIRefl sample then contributes
        // nothing regardless of what texture is bound).
        float giRefl[4]       = {};
        // Cloud shadows (append-only, v2.9): the sky's procedural cloud layer
        // projected along the sun onto the scene. heCloudShadow (binding 33,
        // Metal texture 16 with an inline constexpr sampler — the 16-sampler
        // cap stays untouched) holds the transmittance of the cloud slab over a
        // world-space XZ region around the camera; heLitP projects the fragment
        // along the directional light's L onto the slab's mid-plane and darkens
        // only the directional term.
        //   cloudShadowA: x/y = region origin (world XZ), z = 1 / region size,
        //                 w = slab mid-plane world Y
        //   cloudShadowB: x = strength (0 = off — every legacy fill site leaves
        //                 this 0, so the sample folds dead there)
        float cloudShadowA[4] = {};
        float cloudShadowB[4] = {};
        // Specular anti-aliasing (append-only, v3.0, docs/anti-aliasing-plan.md
        // A6): widen the roughness by how much the normal varies INSIDE the
        // pixel, so a glancing highlight on a curved or normal-mapped surface
        // stops crawling. No edge filter can fix this — the aliasing is in the
        // shading, not on a silhouette.
        //   x = strength (0 = off; every legacy fill site leaves this 0, so the
        //       term folds dead and the image is byte-identical to before)
        //   y = 1 only where the fragment's OWN normal and its derivatives are
        //       real: the forward shading pass and the G-buffer pass. The
        //       deferred resolve must leave this 0 — there the "normal" comes
        //       from a G-buffer texel, and its derivative jumps at every object
        //       edge and quantisation step, which would halo instead of smooth.
        float specAA[4]       = {};
        // CSM receiver depth bias (append-only, v3.1, the project's
        // ProjectShadowSettings): x = slope-scaled factor, y = minimum —
        //   bias = clamp(x * tan(acos(N·L)), y, 0.02) * (cascade + 1)
        // Only meaningful where csmSplits.w > 0, and every fill site that sets
        // csmVP sets this alongside it; the zero every other site leaves is
        // never read because heCsmShadow is not reached there.
        float shadowBias[4]   = {};
        // Viewport view mode (append-only, v3.2, IRenderer::SetViewMode):
        //   x = 1 → heLitP returns the base colour untouched (Unlit and
        //       Wireframe views) — no lights, no ambient, no weather, no fog
        //       (heApplyFog checks it too). Only the scene-pass fill sites set
        //       it (Metal FillMaterialLighting, GL fillMatLight); the zero every
        //       preview/thumbnail/UI site leaves keeps them shaded.
        float viewMode[4]     = {};
        // Clustered forward lighting (append-only, v3.3, Thema 117): the gate and
        // grid of the CLUSTERED fragment variant (fragmentClustered). Exactly
        // HE::ClusterLightBuild::params / camFwd — HE::FillMaterialClusterParams
        // copies them over:
        //   clusterParams: x/y/z = grid dims, w = gridZ / log(far / near).
        //                  x == 0 → OFF: the variant shades the 8-light window
        //                  exactly like fragment() does. Every fill site that
        //                  does not bind the cluster buffers leaves it 0.
        //   clusterCamFwd: xyz = camera forward (the slice depth axis), w = near.
        // The light WINDOW stays fully filled when this is on: the clustered
        // variant skips the window's point/spot lights itself, so a plain
        // (or pak-precompiled) material shader reading the same buffer in the
        // same frame still sees its eight lights.
        float clusterParams[4] = {};
        float clusterCamFwd[4] = {};
    };
    static constexpr int kMetalLightingBufferIndex = 1; // fragment [[buffer(1)]]
    // Clustered variant's light lists (fragmentClustered): GLSL bindings 24/25/26
    // (light array, grid, index list — the same three the clustered deferred
    // resolve reads) → Metal fragment buffers 4/5/6, HLSL t24/t25/t26.
    static constexpr int kMetalClusterLightsBufferIndex = 4;
    static constexpr int kMetalClusterGridBufferIndex   = 5;
    static constexpr int kMetalClusterIndexBufferIndex  = 6;
    // Metal fragment SAMPLER slots the lighting preamble pins outside the
    // material-texture window 0..kMatMaxGraphTextures (fragment() /
    // fragmentClustered(), see the pin table there). Every encoder that draws a
    // material PSO must bind a sampler on ALL of them, used or not: API
    // validation rejects the draw on any declared-but-unbound sampler. The scene
    // passes bind them per frame; the material preview / thumbnail encoder has no
    // frame state and binds them with inert defaults (Thema 124 A7 — it bound
    // none, and MTL_DEBUG_LAYER flagged 5-12/14 on every preview draw).
    static constexpr int kMetalPreambleSamplerSlots[] = { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    // GL 4.3 (GLSL430): the same three lists as shader-storage bindings 4/5/6
    // (glBindBufferBase(GL_SHADER_STORAGE_BUFFER, …)). Not 24..26: GL 4.3 only
    // guarantees 8 SSBO bindings, and the GI compute passes own 0..3.
    static constexpr int kGlClusterLightsSsboBinding = 4;
    static constexpr int kGlClusterGridSsboBinding   = 5;
    static constexpr int kGlClusterIndexSsboBinding  = 6;

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

    // The variant the exporter baked for `backend` (MaterialAsset::precompiledShaders,
    // CHUNK_PSHD), or null → cross-compile at runtime. D3D11 and D3D12 share one HLSL
    // (same SM 5.0 source, same pinned registers), so either tag serves both: a pak
    // exported for D3D12 alone still carries its materials onto D3D11 and vice versa.
    // Every other backend matches exactly. Null `ma` → null.
    static const MaterialShaderVariant* precompiledFor(const MaterialAsset* ma,
                                                       RendererBackend backend);

    // A Vulkan variant stores its SPIR-V as the raw word bytes in the variant's string
    // fields (the exporter memcpy's the words; the runtime reinterprets). Unpack them;
    // false when the byte count is not a whole number of words or the module is empty —
    // the caller then treats the variant as absent and cross-compiles.
    static bool spirvFromBytes(const std::string& bytes, std::vector<uint32_t>& out);

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
    // clusterParams/clusterCamFwd drive the CLUSTERED variant only (plan P7,
    // Metal): grid dims + log-slice scale, camera forward + near plane. Zero on
    // backends without clustering (the non-clustered sources never read them).
    struct ResolveUniforms
    {
        float invViewProj[16]   = {}; // column-major inverse(proj * view)
        float depthParams[4]    = {};
        float clusterParams[4]  = {}; // x/y/z = grid dims, w = gridZ / log(far/near)
        float clusterCamFwd[4]  = {}; // xyz = camera forward (view-z), w = cluster near
    };

    // ── SSR (docs/ssr-plan.md §4.5 — deferred path, Metal v1) ────────────────
    // ssrTrace: fullscreen world-space ray march against the G-buffer depth,
    // sampling the CURRENT frame's resolved HDR colour (no lag, no history).
    // Output RGBA16F: rgb = reflected radiance, a = confidence.
    // ssrComposite: additive fullscreen pass that re-adds the specular-IBL term
    // heLitP skipped (heLight.ssr.w) — sky cubemap mixed against the SSR hit,
    // with heLitP's exact weather/AO/fog factors (SYNC-commented).
    // ssrBlur: separable 5-tap Gaussian over the half-res trace result (plan
    // §4.3 / P4) — smooths the jittered march's dithering before the composite.
    // Confidence-weighted: miss pixels (a = 0) contribute no colour, so hit
    // edges do not darken; the blurred confidence itself feathers them.
    const Compiled& ssrTrace(Backend backend);
    const Compiled& ssrComposite(Backend backend);
    const Compiled& ssrBlur(Backend backend);
    // Forward-path glossy mix: bakes the deferred composite's narrow/wide
    // roughness lerp into the reflection texture (the forward scene shader
    // samples only one). See kSSRRoughMixFS.
    const Compiled& ssrRoughMix(Backend backend);

    // ── Reflection pre-pass (forward path) ───────────────────────────────────
    // The backends without a G-buffer cannot hand the SSR trace a GB1/GBDepth
    // pair, so the forward path rasterises the scene once more and writes the
    // two channels the trace reads, in exactly the G-buffer's encoding:
    //   attachment 0  vec4 view-space position (w = 1 → valid geometry)
    //   attachment 1  rg = oct world normal *0.5+0.5, b = roughness (always 0 —
    //                 the pre-pass has no material data, the scene shader
    //                 applies the roughness fade), a = unused
    //   attachment 2  NDC depth, the same value the G-buffer pass stores
    // Uniform block (binding 1) is three mat4 — mvp, modelView, model — 192
    // bytes std140, the layout the Metal/GL encoders already send per draw.
    // Vertex data follows standardVertex's split: SSBO pull on Metal (mesh
    // buffer at index 0), attributes everywhere else (macOS-GL 4.1 has no SSBO).
    // Was hand-written MSL in MetalRenderer.mm until the cross-backend port
    // (docs/ssr-cross-backend-plan.md §3.2 way (a)).
    const Compiled& reflPrepassVertex(Backend backend);
    const Compiled& reflPrepassFragment(Backend backend);
    // The canonical fragment text, for tests that compare it against the
    // preamble's oct encoder rather than against a compiled result.
    static const std::string& reflPrepassFragmentGlsl();

    // std140 layout of the pre-pass's uniform block (binding 1). Byte-identical
    // to the SSAOPosUniforms the Metal backend has always sent.
    struct ReflPrepassUniforms
    {
        float mvp[16]       = {};
        float modelView[16] = {};
        float model[16]     = {};
    };

    // Instanced twin of reflPrepassVertex for a GeometryPass batch: the model
    // matrix comes per instance (Metal: SSBO of mat4 at binding 2, pinned to
    // vertex buffer 5, indexed by gl_InstanceIndex; GL: four vec4 attributes at
    // locations 4–7, the scene pass's divisor-1 binding), and the uniform block
    // (binding 1) shrinks to the batch-constant camera pair below. Same
    // varyings, so reflPrepassFragment is shared with the plain variant.
    const Compiled& reflPrepassVertexInstanced(Backend backend);

    // std140 layout of the instanced variant's uniform block (binding 1).
    struct ReflPrepassInstUniforms
    {
        float viewProj[16] = {};
        float view[16]     = {};
    };

    // std140 layout of the blur shader's HeSSRBlur UBO (binding 23).
    struct SSRBlurUniforms
    {
        float dir[4] = {}; // xy = one-texel UV step along the blur axis, zw = 1 / target size
    };

    // std140 layout of the trace shader's HeSSRTrace UBO (binding 23).
    struct SSRTraceUniforms
    {
        float viewProj[16]     = {};
        float invViewProj[16]  = {};
        float prevViewProj[16] = {}; // LAST frame's view-proj (temporal reprojection)
        float camPos[4]        = {}; // xyz camera world position
        float camFwd[4]        = {}; // xyz camera forward (view-z axis)
        float cfg[4]           = {}; // x maxDistance, y thickness, z maxRoughness, w stepCount
        float conv[4]          = {}; // x ndc-y sign, y depth scale, z depth bias, w edge-fade width
        float vp[4]            = {}; // xy = trace-target size in pixels
        float cfg2[4]          = {}; // x = frame seed, y = history blend (0 = temporal off), z = forward path, w = glossy cone jitter
    };

    // ── Deferred decals (P7 follow-up) ───────────────────────────────────────
    // A unit-cube projector rasterized into the G-buffer pass: the fragment
    // reads the NDC depth, reconstructs the world position, clips against the
    // decal box and alpha-blends its colour into GB0's rgb (metallic in .a
    // stays — writeMask RGB). No depth test: the box volume decides, so the
    // camera may sit inside the projector.
    //
    // The first two fragment variants differ ONLY in where the depth comes from
    // (docs/decals-cross-backend-plan.md §3):
    //   decalFragment        — subpassInput / framebuffer fetch out of G-buffer
    //                          attachment 3. Metal single-pass tile mode only.
    //   decalFragmentSampled — sampler2D heGBDepth on binding 22, the stored
    //                          depth texture. GL and Metal's two-pass fallback.
    //   decalFragmentForward — same sampled depth and the same box clip, but it
    //                          SHADES the pixel (one directional + ambient, from
    //                          a geometric normal out of ddx/ddy of the recon-
    //                          structed position) and blends into the already-lit
    //                          colour target. For the backends with no G-buffer
    //                          at all: Vulkan, D3D11, D3D12. No shadows, no
    //                          point/spot lights, no GI on the decal — the
    //                          deliberate optical deviation of those backends.
    //
    // Backend::HLSL is emitted with PINNED registers, because SPIRV-Cross turns
    // layout(binding = N) into register(bN/tN/sN) and the canonical decal
    // bindings would land past D3D11's hard limits (14 constant-buffer and 16
    // sampler slots per stage). The contract both D3D backends bind against:
    //   b13 HeDecal (VS+PS) | t14/s14 heDecalTex | t15/s15 heGBDepth
    const Compiled& decalVertex(Backend backend);
    const Compiled& decalFragment(Backend backend);
    const Compiled& decalFragmentSampled(Backend backend);
    const Compiled& decalFragmentForward(Backend backend);

    // std140 layout of the decal shader's HeDecal UBO (binding 23, both stages).
    struct DecalUniforms
    {
        float viewProj[16]    = {}; // scene view-proj (raster convention)
        float model[16]       = {}; // unit cube [-0.5, 0.5]³ → world
        float invModel[16]    = {};
        float invViewProj[16] = {};
        float color[4]        = {}; // rgba tint (a = opacity)
        float params[4]       = {}; // x hasTexture, y ndc-y sign, z depth scale, w depth bias
        float vp[4]           = {}; // xy viewport in pixels
        // Read by decalFragmentForward ONLY. The G-buffer backends leave them at
        // zero and their shaders never look — the block is shared so that vertex
        // and fragment keep declaring the identical HeDecal (a member mismatch is
        // a link error on GL).
        float sunDir[4]       = {}; // xyz direction TO the sun
        float sunColor[4]     = {}; // rgb sun radiance
        float ambient[4]      = {}; // rgb ambient
        float camPos[4]       = {}; // xyz camera position (normal faces the viewer)
    };

    // Clustered-lighting variants of the two resolves (plan P7; the sampled one
    // also for HLSL and SPIR-V since Thema 150, the tile one Metal only):
    // heLitP shades ambient/GI/directional from a DIRECTIONAL-ONLY light window,
    // and all point/spot lights come from per-cluster light lists in SSBOs
    // (bindings 24/25/26 → Metal fragment buffers 4/5/6) — the 8-light limit
    // falls. Per-light math and the atlas-shadow lookup are kept line-for-line
    // with heLitP's loop (the clustered-off A/B is the guard). The ray-traced
    // GI local masks are not applied to cluster lights (v1 limitation).
    const Compiled& deferredResolveClustered(Backend backend);
    const Compiled& deferredResolveTileClustered(Backend backend);

    // D3D (HLSL SM 5.0) register contract of deferredResolve[Clustered](HLSL),
    // pinned in compileResolveVariant (Thema 150, docs/deferred-renderer-plan.md
    // §10.4). Everything the preamble declares keeps the material pin table's
    // registers (b0, t10..t18, t31..t33 with their moved samplers), the cluster
    // lists keep t24..t26. Only the resolve's own inputs need numbers:
    //   HeResolve (binding 23)        → b4   (b0..b13 is all SM 5.0 has)
    //   heGB0/1/2, heGBDepth (19..22) → t27..t30, samplers s2/s4/s5/s6
    // t27..t30 rather than the binding numbers: D3D11's built-in scene shader
    // keeps its structured cluster lists on t18..t20, and a G-buffer view left on
    // t19/t20 would be read as a cluster grid by the next built-in draw. The
    // samplers are the ones the resolve leaves free (it declares neither heTex0
    // nor heTexP0..3); s7 is the one register still unused.
    static constexpr int kHlslResolveUboReg      = 4;
    static constexpr int kHlslResolveGB0Reg      = 27;
    static constexpr int kHlslResolveGB1Reg      = 28;
    static constexpr int kHlslResolveGB2Reg      = 29;
    static constexpr int kHlslResolveDepthReg    = 30;
    static constexpr int kHlslResolveGB0Sampler   = 2;
    static constexpr int kHlslResolveGB1Sampler   = 4;
    static constexpr int kHlslResolveGB2Sampler   = 5;
    static constexpr int kHlslResolveDepthSampler = 6;

    // Cross-compile, cached. The Metal backend pins the vertex to verts@0 / Uniforms@1 so
    // it drops into the fixed geometry-pass bind points; other backends use their natural
    // binding model. `sourceHash` keys the fragment cache (identical shaders share a slot).
    const Compiled& standardVertex(Backend backend);
    const Compiled& fragment(uint64_t sourceHash, const std::string& glsl, Backend backend);

    // Clustered-lighting twin of fragment() (Thema 117, forward path): the same
    // material, but heLitP additionally declares the cluster light lists (SSBO
    // bindings 24/25/26 — see kMetalCluster*BufferIndex; HLSL t24..t26, which
    // SPIRV-Cross emits as ByteAddressBuffer, so D3D11 needs RAW views there)
    // and, while heLight.clusterParams.x > 0, shades point/spot lights from the
    // fragment's cluster list instead of the 8-light window. With the gate at 0
    // the output equals fragment()'s. Opt-in per backend: the pipeline must
    // bind all three buffers (D3D12: root signature, Vulkan: set layout), so a
    // backend switches to this only together with its binding code.
    // GLSL410 / GLSLES300 have no SSBOs → ok = false with a log line, never a
    // silent fallback to the window variant. GLSL430 pins the lists to SSBO
    // bindings 4/5/6 (kGlCluster*SsboBinding) and drops every other binding;
    // pair it with standardVertex/customVertex(GLSL430) in one program.
    const Compiled& fragmentClustered(uint64_t sourceHash, const std::string& glsl, Backend backend);

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
                   m_ssrCache.clear(); m_decalCache.clear(); m_fsVertCache.clear();
                   m_reflPrepassCache.clear(); }

private:
    const Compiled& fragmentVariant(uint64_t sourceHash, const std::string& glsl, Backend backend,
                                    bool clustered);

    std::unordered_map<int, Compiled>      m_vertCache;  // key = (int)backend
    std::unordered_map<uint64_t, Compiled> m_fragCache;  // key = mix(sourceHash, backend, clustered)
    std::unordered_map<uint64_t, Compiled> m_cvertCache; // key = mix(bodyHash, backend)
    std::unordered_map<int, Compiled>      m_uiVertCache; // key = (int)backend
    std::unordered_map<int, Compiled>      m_resolveCache; // key = (int)backend (+64 clustered)
    std::unordered_map<int, Compiled>      m_resolveTileCache; // key = (int)backend (+64 clustered)
    std::unordered_map<int, Compiled>      m_ssrCache;     // key = (int)backend*4 + (0 trace / 1 composite / 2 blur / 3 roughMix)
    std::unordered_map<int, Compiled>      m_decalCache;   // key = (int)backend*4 + (0 vertex / 1 fetch / 2 sampled / 3 forward)
    std::unordered_map<int, Compiled>      m_fsVertCache;  // key = (int)backend
    // Own map, not a fifth slot in m_ssrCache: that key space is backend*4 and
    // full, and a collision there hands a caller someone else's shader while
    // still reporting ok.
    std::unordered_map<int, Compiled>      m_reflPrepassCache; // key = (int)backend*2 + (0 vertex / 1 fragment); 0x100 + backend = instanced vertex
};
} // namespace HE
