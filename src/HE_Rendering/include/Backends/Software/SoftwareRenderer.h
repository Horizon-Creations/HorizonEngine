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

private:
    HE::Window*     m_window = nullptr;
    RenderExtractor m_extractor;
    RenderWorld     m_renderWorld;
    HE::sw::Image   m_frame;
    // What the last frame cost, in quads and in pixels actually written — the
    // only two numbers that mean anything for a CPU rasterizer, and the ones the
    // dirty-rectangle work will be measured against.
    uint32_t        m_lastQuads = 0;
};
