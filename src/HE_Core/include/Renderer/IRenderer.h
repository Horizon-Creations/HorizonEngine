#pragma once
#include "Types/Defines.h"
#include "Types/UUID.h"
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
        float     nebulaIntensity   = 0.5f;
        glm::vec3 nebulaColor       = glm::vec3(0.42f, 0.45f, 0.92f);
        glm::vec3 auroraColor       = glm::vec3(0.25f, 0.95f, 0.50f);
    };
    virtual void SetEnvironmentSettings(const EnvironmentSettings& e) { m_environment = e; }
    const EnvironmentSettings& GetEnvironment() const { return m_environment; }

    // ── Offscreen viewport (editor scene view) ────────────────────────────
    // When a non-zero size is set, the scene is rendered into an offscreen
    // target instead of the window and GetViewportTexture() returns it as an
    // ImGui-compatible texture handle (GL: GLuint cast; Metal: id<MTLTexture>).
    // Pass 0×0 to return to direct-to-window rendering.
    virtual void  SetViewportSize(uint32_t /*width*/, uint32_t /*height*/) {}
    virtual void* GetViewportTexture() { return nullptr; }

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

    // ── ImGui texture helpers ──────────────────────────────────────────────
    // Upload raw RGBA8 pixel data and return a backend-specific texture handle
    // that can be cast to ImTextureID at the call site.
    // Returns nullptr on failure or if the backend does not support it.
    virtual void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height);
    // Release a texture previously created with CreateImGuiTexture.
    virtual void  DestroyImGuiTexture(void* handle);

    // ── Night-sky moon texture (optional) ──────────────────────────────────
    // Pushed once by the app. The backend uploads the RGBA8, tightly-packed
    // pixels and samples them on the moon disk in the procedural night sky.
    // Passing nullptr or a zero size leaves the moon as a plain disk.
    virtual void  SetMoonTexture(const void* /*rgba8Pixels*/, int /*width*/, int /*height*/) {}

protected:
    OverlayCallback      m_overlayCallback;
    HorizonWorld*        m_world          = nullptr;
    ContentManager*      m_contentManager = nullptr;
    EditorCameraOverride m_editorCamera;
    EnvironmentSettings  m_environment;
};
