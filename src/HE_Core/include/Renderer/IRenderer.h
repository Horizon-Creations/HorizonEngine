#pragma once
#include "Types/Defines.h"
#include <functional>
#include <memory>

namespace HE { class Window; }
class HorizonWorld;

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

    // Enable or disable vertical synchronisation.
    // Backends recreate swapchains or change swap intervals as needed.
    virtual void SetVSync(bool enabled) { (void)enabled; }

    // ── ImGui texture helpers ──────────────────────────────────────────────
    // Upload raw RGBA8 pixel data and return a backend-specific texture handle
    // that can be cast to ImTextureID at the call site.
    // Returns nullptr on failure or if the backend does not support it.
    virtual void* CreateImGuiTexture(const void* rgba8Pixels, int width, int height);
    // Release a texture previously created with CreateImGuiTexture.
    virtual void  DestroyImGuiTexture(void* handle);

protected:
    OverlayCallback m_overlayCallback;
    HorizonWorld*   m_world = nullptr;
};
