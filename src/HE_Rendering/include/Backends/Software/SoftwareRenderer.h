#pragma once
#include <Renderer/IRenderer.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>

#include "SoftwareRaster.h"

// ── The renderer for an application without a GPU ─────────────────────────────
// Block G of docs/he-apps-plan.md: the backend a project with "Advanced Shader
// Effects" switched off ships with, so that it needs no GPU, no driver and no
// shader translation at all.
//
// It is deliberately THIN. Everything that draws lives in SoftwareRaster, which
// knows nothing about windows or IRenderer; this class only asks the extractor
// for the UI, hands it over, and blits the result into the window's own SDL
// surface. That is also why the rasterizer is testable and this file is not.
//
// It answers "no" to every 3D question it is asked. IRenderer has four pure
// virtual methods and thirty-seven with bodies, so a backend that knows nothing
// about SSR, GI, bloom or skeletal previews simply leaves them alone — which is
// what makes this block small enough to exist.
class SoftwareRenderer final : public IRenderer
{
public:
    SoftwareRenderer();
    ~SoftwareRenderer() override;

    void Initialize(HE::Window* window) override;
    void Shutdown() override;
    void Render() override;
    Capabilities GetCapabilities() const override;

    // The frame just drawn, for the headless dump and the thumbnail path.
    bool CaptureViewport(std::vector<uint8_t>& rgba,
                         uint32_t& width, uint32_t& height) override;
    bool RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
                               uint32_t size, std::vector<uint8_t>& outRgba8) override;
    FrameGpuStats GetFrameGpuStats() const override;

    // ── A second window (docs/he-apps-plan.md §13.3) ──────────────────────────
    // The cheapest backend to give one, and the one that needs it: everything
    // here already IS a UI-only path, so a second window is a second surface and
    // a second dirty-rectangle history, not a second scene.
    void AttachWindow(HE::Window* window) override;
    void DetachWindow(HE::Window* window) override;
    void RenderWindow(HE::Window* window) override;

private:
    // Everything a window carries between frames. It used to be nine members of
    // this class; a second window is what makes them a struct, because the
    // dirty-rectangle diff only means anything against the frame THAT window
    // last showed. Sharing one history across two windows would repaint each of
    // them with the other one's changes.
    struct Target
    {
        HE::Window*   window = nullptr;
        HE::sw::Image frame;
        // Last frame's quads, kept to diff against — the whole dirty-rectangle
        // idea in one member. Copied rather than referenced: the extractor
        // rebuilds its list every frame, so a reference would compare the list
        // against itself.
        std::vector<UIRenderObject> prevObjects;
        std::vector<glm::vec4>      dirty;
        // The surface SDL last handed over. If it hands over a DIFFERENT one,
        // its untouched regions hold something else entirely and a partial
        // update would show two frames at once — so a changed surface forces a
        // full repaint.
        void*    lastSurface = nullptr;
        bool     haveFrame   = false;
        uint32_t lastQuads   = 0;
        uint64_t lastPixels  = 0;
    };
    // Draw one window's widgets into its own SDL surface. `windowId` is what
    // WidgetManager knows the window as (0 = the main one).
    void renderTarget(Target& t, uint32_t windowId);

    HE::Window*     m_window = nullptr;
    RenderExtractor m_extractor;
    RenderWorld     m_renderWorld;
    Target          m_primary;
    // Secondary windows, by SDL window id — the same key Application files them
    // under and the same number WidgetManager uses for Instance::windowId.
    std::vector<Target> m_secondaries;
};
