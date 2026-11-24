#pragma once
#include <Renderer/IRenderer.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>

#include "SoftwareRaster.h"

#include <MaterialGraph/MatCpuEval.h>

#include <map>
#include <string>
#include <unordered_map>

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
    bool RasterizeUI(const std::vector<UIRenderObject>& uiObjects,
                     uint32_t width, uint32_t height,
                     const glm::vec4& clear, std::vector<uint8_t>& outRgba8,
                     bool* animated = nullptr) override;
    void InvalidateMaterial(const HE::UUID& materialId) override;
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

    // A texture as the rasterizer samples it: the base level (a block-compressed
    // one, ASTC on macOS or BCn elsewhere, expanded once to RGBA8 into `rgba`; an
    // RGBA8 one read in place through `base`) plus a box-filtered mip chain.
    // `src` is the asset's payload it came from, so a reloaded asset is redone.
    struct Decoded
    {
        const void* src = nullptr;
        std::vector<std::uint8_t> rgba;
        const std::uint8_t* base = nullptr;
        int w = 0, h = 0;
        std::vector<std::vector<std::uint8_t>> mipBytes;
        std::vector<HE::sw::TextureLevel>      mips;
    };
    std::unordered_map<HE::UUID, Decoded> m_decoded;
    static HE::sw::TextureView resolveTexture(const HE::UUID& id, void* self);

    // A material compiled for the CPU (HE::MatCpuProgram). Keyed by what it was
    // compiled FROM — the graph JSON (an instance's parent's), the instance's
    // switch overrides and the JSON of every function it called — so an edit
    // anywhere in that chain recompiles it, and nothing else does.
    struct CpuMaterial
    {
        std::string graphJson;
        std::vector<std::string> switchNames;
        std::vector<uint8_t>     switchValues;
        std::map<std::string, std::string> functionJson;   // path → JSON it was compiled with
        HE::MatCpuProgram program;
        std::vector<HE::UUID> textureIds;                  // program.textures() resolved
        std::vector<HE::sw::TextureView> views;            // this frame's views of them
        bool compiled = false, warned = false;
        // What it was last drawn with, and whether that changed since the last
        // frame asked: a parameter a script set changes the picture while every
        // quad stays the same, so the dirty-rectangle diff alone would miss it.
        std::vector<float> drawnParams;
        bool changed = false;
    };
    std::unordered_map<HE::UUID, CpuMaterial> m_materials;
    std::unordered_map<std::string, HE::MaterialGraph> m_functions;   // parsed, by path
    static HE::sw::MaterialView resolveMaterial(const HE::UUID& id, void* self);
    // What a frame of these quads draws with: the resolvers, this renderer, the
    // clock the GPU's UI pass reads. `animated` (optional) says whether a
    // visible material reads Time or Backdrop, i.e. whether the same quads can
    // come out differently next frame.
    HE::sw::DrawOptions drawOptions(const std::vector<UIRenderObject>& quads, bool* animated);

    HE::Window*     m_window = nullptr;
    RenderExtractor m_extractor;
    RenderWorld     m_renderWorld;
    Target          m_primary;
    // Secondary windows, by SDL window id — the same key Application files them
    // under and the same number WidgetManager uses for Instance::windowId.
    std::vector<Target> m_secondaries;
};
