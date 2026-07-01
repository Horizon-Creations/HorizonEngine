#pragma once
#include "Types/Defines.h"
#include "Types/UUID.h"
#include "DebugDraw/DebugDraw.h"
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
        // GPU-simulated weather particles (transform feedback). True only on
        // backends + drivers that can run the GPU precipitation path; the editor
        // greys out the toggle when false. GL reports true (TF is core in 4.1).
        bool supportsGpuParticles   = false;
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

    // Debug: tint each lit fragment by its shadow cascade index (Metal CSM) so the
    // cascade split placement can be verified visually. No-op on other backends.
    virtual void SetShadowDebug(bool /*on*/) {}

    // ── Environment / day-night cycle ───────────────────────────────────────
    // Pushed by the editor. When dayNightCycle is on, the renderer's extractor
    // drives the sun from timeOfDay (0..1: 0.25 sunrise, 0.5 noon, 0.75 sunset,
    // 0/1 midnight) — moving the sky, the image-based ambient and the shadows
    // together. Off = the scene's own directional light is used.
    struct EnvironmentSettings
    {
        bool  dayNightCycle = false;
        float timeOfDay     = 0.5f; // noon
        // Sun & moon directional lights driven by the day-night cycle. Colour and
        // brightness are user-adjustable; each luminary is faded out as it sets.
        glm::vec3 sunColor      = glm::vec3(1.0f, 0.97f, 0.90f); // warm daylight
        float     sunIntensity  = 2.2f;
        glm::vec3 moonColor     = glm::vec3(0.55f, 0.65f, 0.95f); // cool moonlight
        float     moonIntensity = 0.66f;
        float     moonPhase     = 0.5f;  // 0/1 new … 0.5 full
        // Procedural cloud amount (0 = clear sky … 1 = full overcast). At full
        // overcast the sun/moon directional light is switched off (optimisation)
        // and replaced by a soft scattered ambient fill.
        float     cloudCoverage = 0.5f;
        // Atmospheric fog / aerial perspective. Distant scene geometry is blended
        // toward the procedural sky colour in its view direction, so it melts into
        // the horizon (and warms toward the sun at sunset). 0 density = no fog.
        // heightFalloff > 0 makes the fog pool near the ground and thin with
        // altitude (analytic exponential height fog); 0 = uniform distance fog.
        float     fogDensity      = 0.0f;
        float     fogHeightFalloff = 0.1f;
        // Night-sky aurora borealis intensity (0 = off). Drifting light ribbons
        // that sweep across the sky, drawn only at night.
        float     auroraIntensity = 0.0f;
        // Milky Way (dense star band) brightness, space-nebula intensity, and the
        // base colours for the nebula and the aurora ribbons. Stars + nebula
        // rotate with time-of-day to mimic Earth's rotation.
        float     milkyWayIntensity = 0.6f;
        float     nebulaIntensity   = 0.3f;
        glm::vec3 nebulaColor       = glm::vec3(0.42f, 0.45f, 0.92f);
        glm::vec3 nebulaColor2      = glm::vec3(0.85f, 0.40f, 1.00f);
        glm::vec3 nebulaColor3      = glm::vec3(1.00f, 0.52f, 0.72f);
        float     nebulaSeed        = 0.0f;
        int       nebulaQuality      = 1;   // 0 Performance, 1 High, 2 Max (Metal + OpenGL)
        glm::vec3 auroraColor       = glm::vec3(0.25f, 0.95f, 0.50f);
        glm::vec3 auroraColorTop     = glm::vec3(0.62f, 0.26f, 0.95f);
        float     auroraHeight        = 0.18f;
        float     auroraFragmentation = 0.4f;
        // Cloud wind: the compass direction the clouds drift toward (degrees, 0 =
        // toward -Z/north, increasing clockwise) and a speed multiplier. The
        // backend turns these into a horizontal drift vector for the cloud noise.
        float     windDirection = 30.0f;
        float     windSpeed     = 1.0f;
        // Lightning flash brightness (0 = none … 1 = full strike). Driven by the
        // WeatherSystem during storms; added to the sky colour in the backend.
        float     flash         = 0.0f;
        // Ground response to weather (0..1). wetness darkens + glosses lit surfaces;
        // snowAmount lays white snow on up-facing surfaces. Read by the lit shader.
        float     wetness    = 0.0f;
        float     snowAmount = 0.0f;
        float     rainAmount = 0.0f;   // drives the sky rainbow (rain + sun) — Metal/OpenGL sky pass
        // Cloud render mode (OpenGL): 0 = sky-dome (default), 1 = 3D volumetric clouds
        // anchored in the world so they parallax as the camera moves. cloudHeight = the
        // 3D layer's height above the camera in world units. Other backends ignore these.
        int       cloudMode   = 0;
        float     cloudHeight = 200.0f;
        // Cloud raymarch quality (perf knob): 0 Low, 1 Medium, 2 High — scales step counts.
        int       cloudQuality = 1;
        bool      lowResClouds = false; // quarter-res cloud pre-pass + upsample (perf; default off)
        // Cloud appearance (OpenGL 3D path): density scales opacity, fluffiness
        // drives the cauliflower erosion, tint colours the clouds.
        float     cloudDensity    = 1.0f;
        float     cloudFluffiness = 0.6f;
        glm::vec3 cloudTint       = glm::vec3(1.0f);
        // Contrails: scattered vapour-trail lines that fill an empty daytime sky.
        float     contrailAmount  = 0.0f;
        // Thin high cirrus clouds: amount = cover/brightness, seed re-rolls the pattern.
        float     cirrusAmount    = 0.0f;
        float     cirrusSeed      = 0.0f;
        float     godRays         = 0.0f;   // crepuscular sun-shaft strength — Metal/OpenGL sky pass
        float     shootingStars   = 0.0f;   // meteor frequency (0 = none) — Metal/OpenGL night sky
        float     lensFlare       = 0.0f;   // camera sun lens-flare strength (0 = off) — post-process overlay
        // Star field brightness + colour tint, overall size and size variation.
        float     starBrightness    = 1.0f;
        glm::vec3 starColor         = glm::vec3(1.0f);
        float     starSize          = 1.0f;
        float     starSizeVariation = 0.5f;
        float     starGlow          = 1.0f;
        float     starTwinkle       = 0.6f;
        float     starDensity       = 0.5f;
    };
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

    // Drop cached GPU buffers for a mesh so ResolveMesh re-uploads from the
    // ContentManager next frame. Call after replaceStaticMesh so sculpt/edit
    // changes are not masked by the renderer's VBO cache.
    virtual void InvalidateMesh(const HE::UUID& /*meshId*/) {}

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
