#pragma once
#include "Types/Defines.h"
#include <glm/glm.hpp>
#include <functional>
#include <memory>

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

    // ── Offscreen viewport (editor scene view) ────────────────────────────
    // When a non-zero size is set, the scene is rendered into an offscreen
    // target instead of the window and GetViewportTexture() returns it as an
    // ImGui-compatible texture handle (GL: GLuint cast; Metal: id<MTLTexture>).
    // Pass 0×0 to return to direct-to-window rendering.
    virtual void  SetViewportSize(uint32_t /*width*/, uint32_t /*height*/) {}
    virtual void* GetViewportTexture() { return nullptr; }

    // ── ImGui texture helpers ──────────────────────────────────────────────
    // Upload raw RGBA8 pixel data and return a backend-specific texture handle
    // that can be cast to ImTextureID at the call site.
    // Returns nullptr on failure or if the backend does not support it.
    virtual void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height);
    // Release a texture previously created with CreateImGuiTexture.
    virtual void  DestroyImGuiTexture(void* handle);

protected:
    OverlayCallback      m_overlayCallback;
    HorizonWorld*        m_world          = nullptr;
    ContentManager*      m_contentManager = nullptr;
    EditorCameraOverride m_editorCamera;
};
