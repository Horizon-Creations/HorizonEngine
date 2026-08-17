#pragma once
#include "Types/Defines.h"
#include "Types/Enums.h" // HE::RenderPath
#include "Types/UUID.h"
#include "DebugDraw/DebugDraw.h"
#include "Renderer/EnvironmentSettings.h" // IRenderer::EnvironmentSettings (aliased below)
#include "Renderer/UIRenderObject.h"     // RenderWidgetThumbnail takes UI draw quads
#include <glm/glm.hpp>
#include <functional>
#include <memory>
#include <vector>
#include <cstdint>

namespace HE { class Window; }
class HorizonWorld;
class ContentManager;

// ─── EditorCameraOverride ───────────────────────────────────────────────────
// When active, the RenderExtractor uses this camera instead of scanning the
// scene for a CameraComponent. The editor owns the orbit/fly state and pushes
// the resulting view matrix here every frame; the projection is rebuilt by the
// extractor with the backend's current aspect ratio so it always matches the
// viewport size exactly.
struct EditorCameraOverride
{
    bool      active       = false;
    glm::mat4 view         = glm::mat4(1.0f);
    glm::vec3 position     = glm::vec3(0.0f);
    float     fovDegrees   = 60.0f;
    float     nearPlane    = 0.1f;
    float     farPlane     = 5000.0f;
    bool      orthographic = false;
};

// One fully-resolved particle for RenderParticlePreview — position/size/color/
// alpha already interpolated over the particle's lifetime by the caller (the
// Particle Graph Editor, which owns the live scratch simulation via
// ParticleSystem::stepPool). The renderer just draws camera-facing billboards.
struct ParticlePreviewInstance
{
    glm::vec3 position;
    float     size  = 0.0f;
    glm::vec3 color = glm::vec3(1.0f);
    float     alpha = 1.0f;
};

// Which asset an IRenderer::RenderAssetThumbnail call is being asked to draw.
// Deliberately a small closed set rather than "any AssetType": a thumbnail needs
// a geometric/shaded representation, which only these three have.
enum class ThumbnailKind
{
    Material     = 0, // the material on a unit sphere
    StaticMesh   = 1, // the mesh itself, camera auto-framed on its bounds
    SkeletalMesh = 2, // the mesh in bind pose (no clip evaluated)
};

// ─── IRenderer ────────────────────────────────────────────────────────────────
// Pure interface — lives in HorizonCore so Application can hold a renderer
// without creating a circular dependency with HorizonRendering.
// Backends are implemented in HorizonRendering and chosen via RendererFactory.

class HE_API IRenderer
{
public:
    struct Capabilities
    {
        bool supportsShadows        = false;
        bool supportsPostProcessing = false;
        bool supportsHDR            = false;
        // GPU-simulated weather particles. True only on backends that implement
        // the GPU precipitation path; the editor greys out the toggle when false.
        // OpenGL (transform feedback, core in 4.1) and Metal (compute kernel)
        // report true; D3D11/D3D12/Vulkan report false and stay on the CPU pool.
        bool supportsGpuParticles   = false;
        // Ray-traced DDGI (dynamic diffuse global illumination). Metal-only, and
        // only when the device + OS actually support GPU ray tracing (checked once
        // at Initialize() — see MetalRenderer::QueryRaytracingSupport). False on
        // every other backend; the editor greys out the GI toggle when false and
        // the backend keeps rendering CSM shadows + AO/ambient as today.
        bool supportsGlobalIllumination = false;
        // Deferred render path (G-buffer + fullscreen lighting resolve, see
        // docs/deferred-renderer-plan.md). Metal + OpenGL when built with the
        // shader cross-compiler (the resolve shader is generated from the shared
        // lighting preamble at runtime); the editor greys out the Render Path
        // combo when false and the backend stays forward regardless of
        // SetRenderPath.
        bool supportsDeferredRendering = false;
        // Screen-space reflections (docs/ssr-plan.md). v1: Metal only, and only
        // in the DEFERRED render path's tile mode (the reflection pass reads the
        // stored G-buffer + the resolved HDR colour — lag-free, no history).
        // The editor greys out the SSR toggle when false.
        bool supportsScreenSpaceReflections = false;
        // Ray-traced GI reflections (docs/gi-reflections-plan.md): specular rays
        // against the GI acceleration structure, hit-shaded from the DDGI probe
        // field. Metal composites them UNDER SSR (filling its off-screen gaps)
        // in the deferred tile path and needs hardware ray tracing; OpenGL
        // (§10, GL 4.3+ = Windows/Linux) composites in the shading pass itself,
        // so BOTH render paths work there and there is no SSR to sit under.
        bool supportsGIReflections = false;
    };

    // Overlay callback: called by the backend at the correct point inside the
    // active render pass / command list so an overlay (e.g. ImGui) can inject
    // its draw calls without the renderer knowing about the overlay library.
    // context is a backend-specific pointer (cmdList for D3D12, VkCommandBuffer*
    // for Vulkan, nullptr for OpenGL/D3D11 which read state from globals).
    using OverlayCallback = std::function<void(void* context)>;

    virtual ~IRenderer() = default;

    // Called once after the primary window is open.
    virtual void Initialize(HE::Window* window) = 0;
    virtual void Shutdown()                     = 0;
    // Called every frame for the primary window.
    virtual void Render()                        = 0;
    virtual Capabilities GetCapabilities() const = 0;

    // ── Profiler GPU stats ─────────────────────────────────────────────────
    // Per-frame GPU timing + counters, pulled by the EngineProfiler when a
    // capture is recording (never on the hot path otherwise). GPU times are
    // measured with backend timer queries and are typically 1–N frames behind
    // the CPU; the profiler attributes them to the frame it reads them on.
    // `name` pointers are static string literals owned by the backend.
    //   gpuFrameMs < 0  → GPU timing unavailable on this backend/driver.
    //   passes          → per-pass breakdown (Shadow / SSAO / Scene=Sky+Clouds /
    //                     Bloom / Tonemap / …) — the cost breakdown that matters.
    //   approx = true → a draw-boundary interval inside one render encoder (an
    //   intra-"Scene" element split). Tile-deferred fragment work on TBDR GPUs
    //   makes such sub-encoder deltas approximate, not exact — the profiler marks
    //   them so they are never read as authoritative pass costs.
    struct GpuPassTime { const char* name = ""; double ms = 0.0; bool approx = false; };
    struct FrameGpuStats
    {
        double                   gpuFrameMs = -1.0;
        std::vector<GpuPassTime> passes;
        uint32_t drawCalls = 0, triangles = 0, visibleObjects = 0, totalObjects = 0;
        double   vramUsedMB = 0.0, vramBudgetMB = 0.0;
        // Which GPU-timing path actually produced `passes` this frame (a static
        // literal): "detailed" (one cmdbuf/pass, serialized, exclusive+additive),
        // "counter" (stage-boundary spans — overlap on TBDR), "whole-frame" (no
        // per-pass), or "" (none). Recorded so a dump says what RAN, not what was
        // requested — the request flag can't catch an engage bug.
        const char* gpuTimingMode = "";
    };
    virtual FrameGpuStats GetFrameGpuStats() const { return {}; }

    // ── Multi-window support (optional – backends may override) ────────────
    // Attach a secondary window so the renderer can create an additional
    // swap-chain / framebuffer for it.  Called once after the window is open.
    virtual void AttachWindow(HE::Window* /*window*/)  {}
    // Detach and destroy the swap-chain for the given secondary window.
    virtual void DetachWindow(HE::Window* /*window*/)  {}
    // Present / render a single secondary window.  Called once per frame
    // after Render() has run for the primary window.
    virtual void RenderWindow(HE::Window* /*window*/)  {}

    // Optional overlay injection — set once after Initialize(), before first Render().
    void SetOverlayCallback(OverlayCallback cb) { m_overlayCallback = std::move(cb); }

    // Scene to render. Set by Application whenever the active world changes.
    // Opaque to HorizonCore — only HorizonRendering's RenderExtractor reads it.
    virtual void SetWorld(HorizonWorld* world) { m_world = world; }

    // Asset source for resolving mesh/texture UUIDs to CPU data. Set once by
    // Application before the first Render(). Backends upload on first sight.
    virtual void SetContentManager(ContentManager* cm) { m_contentManager = cm; }

    // Enable or disable vertical synchronisation.
    // Backends recreate swapchains or change swap intervals as needed.
    virtual void SetVSync(bool enabled) { (void)enabled; }

    // ── Editor camera override ─────────────────────────────────────────────
    // Set by the editor each frame so the scene view is driven by the orbit/
    // fly camera rather than a scene CameraComponent. Cleared (active=false)
    // returns control to the scene camera. Read by the backends when they
    // call the RenderExtractor.
    virtual void SetEditorCamera(const EditorCameraOverride& cam) { m_editorCamera = cam; }
    const EditorCameraOverride& GetEditorCamera() const { return m_editorCamera; }

    // ── Bloom / post-process settings ──────────────────────────────────────
    // Pushed by the editor from its preferences. Backends that implement bloom
    // (GL, Metal) honour it; others ignore it. Defaults match the built-in
    // always-on behaviour.
    struct BloomSettings
    {
        bool  enabled   = true;
        float threshold = 1.0f;   // luminance above which pixels bloom
        float intensity = 0.6f;   // how strongly the blurred bloom is added back
    };
    virtual void SetBloomSettings(const BloomSettings& /*settings*/) {}

    // ── SSAO (screen-space ambient occlusion) ───────────────────────────────
    // Pushed by the editor from its preferences. Backends that implement SSAO
    // (GL, Metal) honour it; others ignore it. When enabled, a view-space depth
    // pre-pass feeds a hemisphere-kernel occlusion estimate that darkens only the
    // image-based ambient term (contact shadows in crevices), leaving the direct
    // lighting untouched. Disabled = zero cost (the pre-pass is skipped) and the
    // image is identical to before.
    struct SSAOSettings
    {
        bool  enabled   = true;
        float radius    = 0.5f;  // hemisphere sampling radius in view-space units
        float intensity = 1.0f;  // 0 = no darkening … 1 = full occlusion
        int   method    = 0;     // AO method: 0 = SSAO, 1 = HBAO, 2 = GTAO (planned)
    };
    virtual void SetSSAOSettings(const SSAOSettings& /*settings*/) {}

    // ── Global Illumination (ray-traced DDGI) ───────────────────────────────
    // Pushed every frame by the editor's preferences and by the packaged game
    // (see GameApplication's GlobalState config read, mirroring GpuParticles).
    // Only the Metal backend implements this (Capabilities::supportsGlobalIllumination
    // gates it); other backends silently ignore it and keep CSM shadows + AO. When
    // enabled on a supported device, GI COMPLETELY REPLACES both CSM shadows and
    // AO/ambient with one ray-traced pipeline: 1 shadow ray/pixel toward the
    // dominant directional light (soft, temporally accumulated) plus DDGI probes
    // sampled for indirect diffuse. Disabled = zero cost and the image is
    // byte-identical to GI never having existed.
    struct GISettings
    {
        bool  enabled             = false;
        float indirectIntensity  = 1.0f;   // multiplier on probe-sampled indirect diffuse
        float lightRadius        = 0.5f;   // degrees — sun angular radius, drives shadow penumbra softness
        int   raysPerProbe       = 128;    // rays traced per probe on the frame it's updated
        int   probeBudgetPerFrame = 256;   // probes relit per frame (round-robin over the grid)
    };
    virtual void SetGISettings(const GISettings& /*settings*/) {}

    // ── Screen-space reflections (docs/ssr-plan.md) ────────────────────────
    // Pushed by the editor's preferences / the packaged game's GlobalState read.
    // v1 effective only on Metal in the deferred path (Capabilities::
    // supportsScreenSpaceReflections gates the UI); other backends ignore it and
    // metallic surfaces keep reflecting the sky cubemap only. Disabled = the
    // image is byte-identical to SSR never having existed.
    struct SSRSettings
    {
        bool  enabled      = false;
        float intensity    = 1.0f;   // 0…1 mix against the sky cubemap
        float maxRoughness = 0.6f;   // above this no SSR (smooth fade toward it)
        float maxDistance  = 30.0f;  // world-space ray length
        float thickness    = 0.5f;   // depth-buffer thickness assumption
        int   quality      = 1;      // 0 = 16 steps, 1 = 32, 2 = 64
    };
    virtual void SetSSRSettings(const SSRSettings& /*settings*/) {}

    // ── Ray-traced GI reflections (docs/gi-reflections-plan.md) ─────────────
    // Pushed like SSR/GI. One specular ray per (half-res) pixel against the GI
    // acceleration structure; hits are shaded from the sun + the DDGI probe
    // field, so reflections stay consistent with the diffuse GI. Where SSR also
    // runs they sit UNDER it: the screen-space trace wins where it has a
    // confident hit, the traced result fills its off-screen gaps.
    // Metal: deferred tile mode + hardware RT. OpenGL 4.3+ (Windows/Linux):
    // the software BVH the diffuse GI already builds, composited in the shading
    // pass, so BOTH render paths work and there is no SSR to sit under.
    // Capabilities::supportsGIReflections gates the UI. Disabled = image
    // byte-identical to the feature never having existed.
    struct GIReflectionSettings
    {
        bool  enabled      = false;
        float intensity    = 1.0f;   // 0…1 mix against the sky cubemap
        float maxRoughness = 0.6f;   // above this no traced reflections (smooth fade)
        float maxDistance  = 200.0f; // world-space ray length
        // Ray bounces (1–4): mirror-like surfaces seen IN a reflection reflect
        // onward instead of flattening to their base colour. Each extra bounce
        // costs one more trace + sun-occlusion ray on the affected pixels only.
        // Metal only — the OpenGL kernel traces a single segment and ignores
        // this rather than pretending to honour it.
        int   bounces      = 1;
        // The tier sets RAYS per pixel and the trace RESOLUTION:
        //   0 Low    1 ray  @ 1/4 screen
        //   1 Medium 2 rays @ 1/2
        //   2 High   4 rays @ full
        // (OpenGL varies rays only — its reflection targets live at the shared
        // GI prepass resolution; see the gap note in OpenGLRenderer's DrawScene.)
        int   quality      = 1;
        // Post-trace blur. It exists to stand in for what the trace did NOT
        // sample — the pixels a lower resolution skipped and the part of the
        // glossy lobe the rays missed — so its width shrinks as the tier rises.
        // Switchable because "is the blur what I am looking at?" turned out to
        // be the question worth answering directly: with this off, what remains
        // is the raw trace at the tier's resolution and ray count, and any
        // remaining softness has a different cause.
        bool  blur         = true;
    };
    virtual void SetGIReflectionSettings(const GIReflectionSettings& /*settings*/) {}

    // ── Render path (Forward | Deferred) ────────────────────────────────────
    // Pushed by the editor's preferences / the packaged game's GlobalState read,
    // like SetGISettings. Backends without deferred support (Capabilities::
    // supportsDeferredRendering == false) ignore it and stay forward. Takes
    // effect at the start of the next frame — never mid-frame.
    virtual void SetRenderPath(HE::RenderPath path) { m_renderPath = path; }
    HE::RenderPath GetRenderPath() const { return m_renderPath; }

    // Debug: tint each lit fragment by its shadow cascade index (Metal CSM) so the
    // cascade split placement can be verified visually. No-op on other backends.
    virtual void SetShadowDebug(bool /*on*/) {}

    // ── Environment / day-night cycle ───────────────────────────────────────
    // The ~60-field sky / day-night / weather-appearance block. It lives in its
    // own header (Renderer/EnvironmentSettings.h) because of its size; the alias
    // keeps every existing `IRenderer::EnvironmentSettings` spelling valid.
    using EnvironmentSettings = ::EnvironmentSettings;
    virtual void SetEnvironmentSettings(const EnvironmentSettings& e) { m_environment = e; }
    const EnvironmentSettings& GetEnvironment() const { return m_environment; }

    // ── GPU weather particles (transform-feedback precipitation) ────────────
    // Pushed every frame by the scene tick. When `enabled`, the backend owns the
    // rain/snow simulation entirely: a fixed camera-following pool lives in GPU
    // buffers, is integrated + recycled by a transform-feedback pass, and drawn as
    // vertex-pulled billboards — the CPU precipitation pool is skipped. `enabled`
    // is false (idle/clear) whenever the toggle is off or the backend can't do it.
    struct GpuParticleParams
    {
        bool      enabled     = false;
        bool      isSnow      = false;
        int       count       = 0;       // pool size (cap); buffers resize on change
        float     dt          = 0.0f;    // sim step this frame
        float     time        = 0.0f;    // monotonically rising clock (sway / respawn hash)
        glm::vec3 cameraPos   = glm::vec3(0.0f);
        glm::vec3 windVec     = glm::vec3(0.0f);
        float     coverage    = 0.0f;    // 0..1 fraction of the pool kept alive (curPrecip)
        float     fallSpeed   = 18.0f;
        float     lifeSpan    = 5.0f;
        float     groundLevel = 0.0f;    // flat collision plane for the GPU path
        float     boxHalf     = 16.0f;   // horizontal half-extent of the spawn volume
        float     boxTop      = 24.0f;   // spawn height above the camera
    };
    virtual void SetGpuParticleParams(const GpuParticleParams& /*p*/) {}

    // ── Offscreen viewport (editor scene view) ────────────────────────────
    // When a non-zero size is set, the scene is rendered into an offscreen
    // target instead of the window and GetViewportTexture() returns it as an
    // ImGui-compatible texture handle (GL: GLuint cast; Metal: id<MTLTexture>).
    // Pass 0×0 to return to direct-to-window rendering.
    virtual void  SetViewportSize(uint32_t /*width*/, uint32_t /*height*/) {}
    virtual void* GetViewportTexture() { return m_viewportImGuiHandle; }

    // Called by the platform layer (editor) after it has registered the viewport
    // texture with the ImGui GPU backend (D3D12: D3D12_GPU_DESCRIPTOR_HANDLE.ptr
    // packed into void*; Vulkan: VkDescriptorSet; others: unused).
    void SetViewportImGuiHandle(void* handle) { m_viewportImGuiHandle = handle; }

    // ── Offscreen capture (headless screenshot / validation / thumbnails) ───
    // Read the most recently rendered offscreen viewport color target back into
    // CPU memory as tightly-packed RGBA8, top row first (y-down). Returns false
    // if there is no offscreen target or the backend cannot read it back.
    virtual bool  CaptureViewport(std::vector<uint8_t>& /*rgba*/,
                                  uint32_t& /*width*/, uint32_t& /*height*/) { return false; }

    // ── Material hot-reload ────────────────────────────────────────────────
    // Drop any GPU state the backend cached for this material (e.g. uploaded
    // base-color textures) so the next frame re-resolves it from the
    // ContentManager. Called by the editor after a material asset is edited or
    // re-assigned. No-op on backends that do not honour MaterialComponent yet.
    virtual void InvalidateMaterial(const HE::UUID& /*materialId*/) {}

    // ── Material pipeline warm-up ──────────────────────────────────────────
    // Build (cross-compile + link/PSO) the node-graph material pipelines for
    // these materials AHEAD of their first draw, so the first frame that shows a
    // custom material doesn't hitch on a synchronous glslang/SPIRV-Cross + pipeline
    // build inside the encoder loop. Call after a scene loads (editor + packaged
    // game). Materials without a custom shader are skipped; already-built
    // pipelines are a cheap cache hit. No-op on backends that build eagerly.
    virtual void WarmupMaterials(const std::vector<HE::UUID>& /*materialIds*/) {}

    // ── Material preview ───────────────────────────────────────────────────
    // Render a single primitive shaded with material `materialId` into a small
    // dedicated offscreen target and return an ImGui-compatible texture handle
    // for ImGui::Image (GL: GLuint cast; Metal: id<MTLTexture>). `size` is the
    // square edge in pixels. An orbit camera frames the mesh: `yaw`/`pitch` are
    // radians, `dist` the camera distance. `shape` picks the preview primitive:
    // 0 = sphere, 1 = cube, 2 = plane (double-sided quad). The background is left
    // transparent (alpha 0) so the editor can composite it over its own backdrop.
    // Returns nullptr on backends without a preview path or on failure — the
    // editor then shows a placeholder. Independent of the main viewport target.
    //
    // `meshId` overrides `shape` with a STATIC MESH asset (any project or engine
    // mesh — the Material Editor lets the author pick one), auto-framed on its
    // bounds so `dist` means the same thing for a teapot as for the unit sphere.
    // A mesh that is not loaded/uploadable falls back to `shape`, so a caller
    // never has to pre-check: the preview shows the primitive instead. The CALLER
    // owns getting the asset into the ContentManager (the editor streams it
    // asynchronously and shows progress) — this does no blocking disk I/O.
    virtual void* RenderMaterialPreview(class ContentManager& /*cm*/, const HE::UUID& /*materialId*/,
                                        uint32_t /*size*/, float /*yaw*/, float /*pitch*/, float /*dist*/,
                                        int /*shape*/ = 0, const HE::UUID& /*meshId*/ = HE::UUID{})
    { return nullptr; }

    // ── Skeletal mesh preview ──────────────────────────────────────────────
    // Render a skeletal mesh (skinned with `boneMatrices`, or the bind pose if
    // empty) into a small dedicated offscreen target — same conventions as
    // RenderMaterialPreview (ImGui-compatible handle, transparent background,
    // orbit camera via yaw/pitch/dist, independent of the main viewport).
    // `boneMatrices` is one mat4 per joint (see AnimationPreview::evaluateClipPose
    // in HE_Scene, which the Skeletal Mesh Editor uses to turn a clip + scrub
    // time into this array — the renderer never evaluates animation itself).
    // `showSkeleton` overlays joint markers + parent-child bone lines drawn
    // directly into this preview target (independent of the main viewport's
    // DebugDrawBuffer). Returns nullptr on backends without a preview path.
    // width/height in pixels — the target matches the panel's pane, so the
    // preview fills it edge to edge instead of sitting as a square in a strip
    // of dead space (the projection takes its aspect from the same numbers).
    //
    // `outViewProj` (optional) reports the view-projection this preview was
    // drawn with. It exists so a caller can put its OWN overlay on top — a
    // collider outline, a camera boom — in the same space, without rebuilding
    // the framing. That framing depends on the mesh's GPU-side bounds, which
    // only the renderer has; a caller recomputing it would drift the moment the
    // rule here changed, and the drift would look like a wrong collider rather
    // than a wrong camera. Left untouched on backends without a preview path.
    virtual void* RenderSkeletalPreview(class ContentManager& /*cm*/, const HE::UUID& /*meshId*/,
                                        const std::vector<glm::mat4>& /*boneMatrices*/,
                                        uint32_t /*width*/, uint32_t /*height*/,
                                        float /*yaw*/, float /*pitch*/, float /*dist*/,
                                        bool /*showSkeleton*/ = true,
                                        glm::mat4* /*outViewProj*/ = nullptr)
    { return nullptr; }

    // ── World preview ──────────────────────────────────────────────────────
    // Render an ARBITRARY `HorizonWorld` into a dedicated offscreen target —
    // the only preview path here that takes a world instead of a single asset.
    // The Class Editor's viewport needs it: a character is a mesh AND colliders
    // AND a camera boom AND whatever else hangs under its root, and no per-asset
    // path can show that assembly. The caller owns the world (the editor builds
    // a scratch one from the class's component blob); this only reads it.
    //
    // Deliberate differences from the per-asset previews above:
    //  • the background is an opaque GRAY with a ground plane and a grid, not
    //    transparent — this is a scene view in the sense of Unreal's character
    //    viewport, and a floating mesh with no ground reads as scale-less.
    //  • the camera orbits a fixed `pivot` (normally the world origin = the
    //    character's own origin) at `dist` in PLAIN WORLD UNITS. It does NOT
    //    auto-frame on the content's bounds: the extractor deliberately leaves
    //    bounds invalid for meshes that are not resident yet, so an auto-fit
    //    would jump around while assets stream in.
    //  • lighting is the previews' fixed headlight, not the world's lights — a
    //    class blob normally contains no light at all, and "your character is
    //    black" would be the wrong lesson to teach.
    //
    // ONE shared target per backend, so exactly one world preview is live at a
    // time. That matches the call site: asset tabs are exclusive, and ImGui
    // never executes an inactive tab's content, so only the active tab calls
    // in a given frame.
    //
    // `outViewProj` reports the view-projection used, so the caller can put its
    // own overlay (origin marker, collider outlines, camera boom) on top in the
    // same space — same contract as RenderSkeletalPreview. Returns nullptr on
    // backends without a world-preview path (currently D3D11/D3D12/Vulkan).
    virtual void* RenderWorldPreview(class ContentManager& /*cm*/, HorizonWorld& /*world*/,
                                     uint32_t /*width*/, uint32_t /*height*/,
                                     float /*yaw*/, float /*pitch*/, float /*dist*/,
                                     const glm::vec3& /*pivot*/ = glm::vec3(0.0f),
                                     glm::mat4* /*outViewProj*/ = nullptr)
    { return nullptr; }

    // ── Particle system preview ────────────────────────────────────────────
    // Render a live-simulated particle pool into a small dedicated offscreen
    // target — same conventions as RenderMaterialPreview/RenderSkeletalPreview.
    // The Particle Graph Editor owns the actual simulation (ParticleSystem::
    // stepPool, HE_Scene, has no business living in the renderer) and hands
    // over one fully-resolved instance per live particle each frame; the mesh
    // is typically the default billboard quad unless the graph's Emitter
    // Output references one. Camera auto-frames around the particles' bounds
    // (yaw/pitch orbit, dist scales that bound — same as RenderSkeletalPreview,
    // since a cloud's extent varies wildly with velocity/gravity, unlike
    // Material's fixed-size primitives).
    virtual void* RenderParticlePreview(class ContentManager& /*cm*/, const HE::UUID& /*meshId*/,
                                        const HE::UUID& /*materialId*/,
                                        const std::vector<ParticlePreviewInstance>& /*particles*/,
                                        uint32_t /*size*/, float /*yaw*/, float /*pitch*/, float /*dist*/)
    { return nullptr; }

    // ── Asset thumbnails ───────────────────────────────────────────────────
    // Render `assetId` into a PRIVATE offscreen target and read it back as
    // tightly packed, top-down RGBA8 (`size`×`size`×4 bytes, transparent where
    // nothing was drawn). Unlike the Render*Preview calls above this returns
    // pixels rather than a GPU handle, so the caller can cache the result on
    // disk and re-upload it as a plain texture — the Content Browser draws one
    // thumbnail per asset and must not re-render them every frame. It also uses
    // a target of its own: sharing the interactive preview's would overwrite
    // whatever the Material Editor is currently showing.
    //
    // Synchronous (renders and waits), so the caller has to budget how many it
    // asks for per frame. Returns false on backends without a thumbnail path,
    // or when the asset cannot be resolved.
    virtual bool RenderAssetThumbnail(class ContentManager& /*cm*/, ThumbnailKind /*kind*/,
                                      const HE::UUID& /*assetId*/, uint32_t /*size*/,
                                      std::vector<uint8_t>& /*outRgba8*/)
    { return false; }

    // Same, for a particle system: it has no single asset to point at — a tile is
    // a snapshot of a SIMULATED pool, which the caller steps (ParticleSystem::
    // stepPool lives in HE_Scene; the renderer never simulates) and hands over
    // already resolved, exactly as for RenderParticlePreview. Reads back to
    // top-down RGBA8 like RenderAssetThumbnail; the target is private to the
    // thumbnail path, not the interactive particle preview's.
    virtual bool RenderParticleThumbnail(class ContentManager& /*cm*/,
                                         const HE::UUID& /*materialId*/,
                                         const std::vector<ParticlePreviewInstance>& /*particles*/,
                                         uint32_t /*size*/, std::vector<uint8_t>& /*outRgba8*/)
    { return false; }

    // Same, for a UI widget: the caller instantiates the widget and extracts its
    // draw quads (WidgetManager, HE_Scene), then hands them over — the renderer
    // knows how to draw UIRenderObjects but nothing about widget assets. `size`
    // is the tile edge; the quads must already be laid out for a square viewport
    // of that many pixels, since UI layout is resolution-dependent.
    virtual bool RenderWidgetThumbnail(const std::vector<UIRenderObject>& /*uiObjects*/,
                                       uint32_t /*size*/, std::vector<uint8_t>& /*outRgba8*/)
    { return false; }

    // Drop cached GPU buffers for a mesh so ResolveMesh re-uploads from the
    // ContentManager next frame. Call after replaceStaticMesh so sculpt/edit
    // changes are not masked by the renderer's VBO cache.
    virtual void InvalidateMesh(const HE::UUID& /*meshId*/) {}

    // Same for a texture: drop the cached GPU texture so it re-uploads from the
    // ContentManager. Call after replaceTexture — e.g. the landscape weightmap,
    // which is rewritten in place on every paint stroke and would otherwise stay
    // frozen at whatever the first upload captured.
    virtual void InvalidateTexture(const HE::UUID& /*textureId*/) {}

    // ── ImGui texture helpers ──────────────────────────────────────────────
    // Upload raw RGBA8 pixel data and return a backend-specific texture handle
    // that can be cast to ImTextureID at the call site.
    // Returns nullptr on failure or if the backend does not support it.
    virtual void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height);
    // Release a texture previously created with CreateImGuiTexture.
    virtual void  DestroyImGuiTexture(void* handle);

    // ── ImGui texture registrar (D3D12 / Vulkan) ───────────────────────────
    // The renderer DLL does not link ImGui, so backends whose ImTextureID is an
    // ImGui-owned object (D3D12: a GPU SRV descriptor in ImGui's heap; Vulkan: a
    // VkDescriptorSet from ImGui_ImplVulkan_AddTexture) cannot build the handle
    // themselves. The editor installs this callback after ImGui is initialized;
    // the backend creates+uploads the GPU texture and then calls the registrar to
    // turn its native handle into an ImGui ImTextureID.
    //   D3D12:  a = ID3D12Resource*,  b = nullptr.
    //   Vulkan: a = VkImageView,      b = VkSampler.
    void SetImGuiTextureRegistrar(std::function<void*(void*, void*)> fn) { m_imguiTexRegistrar = std::move(fn); }

    // ── Night-sky moon texture (optional) ──────────────────────────────────
    // Pushed once by the app. The backend uploads the RGBA8, tightly-packed
    // pixels and samples them on the moon disk in the procedural night sky.
    // Passing nullptr or a zero size leaves the moon as a plain disk.
    virtual void  SetMoonTexture(const void* /*rgba8Pixels*/, int /*width*/, int /*height*/) {}

    // ── Debug line overlay (editor gizmos / visualisations) ────────────────
    // Uploaded every frame from the editor's DebugDrawBuffer. The backend
    // draws them as world-space line segments on top of the opaque scene but
    // before post-process so they participate in tonemap. Passing an empty
    // vector is a no-op / clears any previously submitted lines.
    virtual void SetDebugLines(const std::vector<DebugLine>& /*lines*/) {}

protected:
    OverlayCallback      m_overlayCallback;
    HE::RenderPath       m_renderPath           = HE::RenderPath::Forward;
    HorizonWorld*        m_world                = nullptr;
    ContentManager*      m_contentManager       = nullptr;
    EditorCameraOverride m_editorCamera;
    EnvironmentSettings  m_environment;
    // Viewport texture handle registered by the editor with the ImGui GPU backend.
    // OpenGL/D3D11 override GetViewportTexture() and ignore this field; D3D12 and
    // Vulkan return it so the editor can control descriptor lifetime.
    void*                m_viewportImGuiHandle  = nullptr;
    // Editor-installed callback that converts a backend native texture handle into
    // an ImGui ImTextureID. See SetImGuiTextureRegistrar above. Null on backends
    // (OpenGL/D3D11) that build the handle directly inside CreateImGuiTexture.
    std::function<void*(void*, void*)> m_imguiTexRegistrar;
};
