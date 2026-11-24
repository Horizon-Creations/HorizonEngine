#include "../../../include/Backends/Software/SoftwareRenderer.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/TextureBlockDecode.h>
#include <Diagnostics/Logger.h>
#include <Window/Window.h>

#include <SDL3/SDL.h>

#include <algorithm>

namespace
{
    // The window's clear: the same near-black the other backends start from, so
    // an application does not change colour with its renderer.
    constexpr std::uint8_t kClear[4] = { 18, 18, 22, 255 };

}

// Turn a texture asset into bytes the rasterizer can sample. Textures already
// live on the CPU (TextureAsset keeps its pixels), so there is no upload path.
// An export cooks every texture to the target GPU's block format (ASTC for a
// Mac, BC7 elsewhere) whatever renderer the player ends up on, so a non-RGBA8
// base level is expanded once and kept; `src` notices a reloaded asset.
HE::sw::TextureView SoftwareRenderer::resolveTexture(const HE::UUID& id, void* self)
{
    auto* r = static_cast<SoftwareRenderer*>(self);
    ContentManager* cm = r ? r->m_contentManager : nullptr;
    if (!cm) return {};
    const TextureAsset* t = cm->getTexture(id);
    if (!t || t->data.empty() || t->width <= 0 || t->height <= 0) return {};
    Decoded& d = r->m_decoded[id];
    if (d.src != t->data.data())
    {
        d.src = t->data.data();
        d.w = d.h = 0;
        d.base = nullptr;
        d.rgba.clear();
        const std::size_t baseBytes = static_cast<std::size_t>(t->width) * t->height * 4;
        if (t->format == TextureFormat::RGBA8 && t->data.size() >= baseBytes)
            d.base = t->data.data();
        else if (HE::decodeBaseLevelToRGBA8(*t, d.rgba))
            d.base = d.rgba.data();
        else
            HE_LOG_WARN(RHI, "SoftwareRenderer: texture format %d has no CPU decoder here — "
                        "that quad draws its tint. Export with Textures = None.",
                        static_cast<int>(t->format));
        if (d.base)
        {
            d.w = static_cast<int>(t->width);
            d.h = static_cast<int>(t->height);
            // Built here rather than taken from the asset: a cooked chain is in
            // the GPU's block format, and a loose RGBA8 asset often has none.
            HE::sw::buildMipChain(d.base, d.w, d.h, d.mipBytes);
            d.mips.clear();
            int mw = d.w, mh = d.h;
            for (const auto& m : d.mipBytes)
            {
                mw = std::max(1, mw / 2); mh = std::max(1, mh / 2);
                d.mips.push_back({ m.data(), mw, mh });
            }
        }
    }
    if (d.w == 0) return {};
    HE::sw::TextureView v;
    v.rgba     = d.base;
    v.width    = d.w;
    v.height   = d.h;
    v.mips     = d.mips.data();
    v.mipCount = static_cast<int>(d.mips.size());
    v.srgb     = t->srgb;
    return v;
}

// A material for the rasterizer: its graph compiled for the CPU, its parameters
// bound, its textures resolved. Compiled once per graph (an instance compiles
// its PARENT's graph with its own switch overrides, as ContentManager's
// syncMaterialInstance does for the GPU) and rebound every call, so a
// parameter a script sets shows up the next frame without a recompile.
//
// Everything read out of the asset is copied first: the next loadAsset (the
// parent, a function, a texture) moves every getter pointer.
HE::sw::MaterialView SoftwareRenderer::resolveMaterial(const HE::UUID& id, void* self)
{
    auto* r = static_cast<SoftwareRenderer*>(self);
    ContentManager* cm = r ? r->m_contentManager : nullptr;
    if (!cm) return {};
    const MaterialAsset* ma = cm->getMaterial(id);
    if (!ma) return {};
    std::string json = ma->nodeGraphJson;
    const std::string parent = ma->parentMaterialPath;
    const std::vector<std::string> paramNames = ma->graphParamNames;
    const std::vector<float> paramData = ma->shaderParamData;
    const std::vector<std::string> swNames = ma->instanceSwitchNames;
    const std::vector<uint8_t> swValues = ma->instanceSwitchValues;
    const std::string name = ma->path;
    if (!parent.empty())
    {
        const HE::UUID pid = cm->loadAsset(parent);
        const MaterialAsset* pa = pid == HE::UUID{} ? nullptr : cm->getMaterial(pid);
        json = pa ? pa->nodeGraphJson : std::string();
    }
    if (json.empty()) return {};

    // A function's JSON as it is NOW (path → text), loaded on first sight.
    auto functionJson = [cm](const std::string& path) -> std::string
    {
        const HE::UUID fid = cm->loadAsset(path);
        const MaterialFunctionAsset* fa = fid == HE::UUID{} ? nullptr : cm->getMaterialFunction(fid);
        return fa ? fa->nodeGraphJson : std::string();
    };

    CpuMaterial& e = r->m_materials[id];
    bool stale = !e.compiled || e.graphJson != json || e.switchNames != swNames || e.switchValues != swValues;
    for (const auto& [path, text] : e.functionJson)
        if (!stale && functionJson(path) != text) stale = true;
    if (stale)
    {
        e = CpuMaterial{};
        e.graphJson = json; e.switchNames = swNames; e.switchValues = swValues; e.compiled = true;
        HE::MaterialGraph graph;
        std::string error;
        bool ok = HE::materialGraphFromJson(json, graph);
        if (ok)
        {
            const HE::MatFunctionLoader loader = [r, &e, &functionJson](const std::string& path) -> const HE::MaterialGraph*
            {
                const std::string text = functionJson(path);
                if (text.empty()) return nullptr;
                e.functionJson[path] = text;
                HE::MaterialGraph g;
                if (!HE::materialGraphFromJson(text, g)) return nullptr;
                return &(r->m_functions[path] = std::move(g));
            };
            std::map<std::string, bool> overrides;
            for (size_t i = 0; i < swNames.size() && i < swValues.size(); ++i) overrides[swNames[i]] = swValues[i] != 0;
            ok = e.program.compile(graph, loader, overrides.empty() ? nullptr : &overrides, &error);
        }
        else
            error = "its graph could not be read";
        if (!ok)
        {
            HE_LOG_WARN(RHI, "SoftwareRenderer: material '%s' draws its plain surface — %s %s",
                        name.c_str(), error.empty() ? "" : "the CPU cannot run", error.c_str());
            return {};
        }
        for (const std::string& path : e.program.textures())
            e.textureIds.push_back(cm->loadAsset(path));
    }
    if (!e.program.valid()) return {};
    e.program.bindParams(paramNames, paramData);
    e.views.clear();
    for (const HE::UUID& tid : e.textureIds)
        e.views.push_back(tid == HE::UUID{} ? HE::sw::TextureView{} : resolveTexture(tid, r));
    HE::sw::MaterialView v;
    v.program = &e.program;
    v.textures = e.views.data();
    v.textureCount = static_cast<int>(e.views.size());
    return v;
}

void SoftwareRenderer::InvalidateMaterial(const HE::UUID& materialId)
{
    m_materials.erase(materialId);
}

HE::sw::DrawOptions SoftwareRenderer::drawOptions(const std::vector<UIRenderObject>& quads, bool* animated)
{
    HE::sw::DrawOptions o;
    o.texture = &resolveTexture;   o.textureUser = this;
    o.material = &resolveMaterial; o.materialUser = this;
    // The GPU UI pass's clock (EncodeUIPass / RenderUIPass: SDL ticks).
    o.time = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    if (animated)
    {
        *animated = false;
        for (const UIRenderObject& q : quads)
            if (q.type == 0 && q.materialAssetId != HE::UUID{})
            {
                const HE::sw::MaterialView mv = resolveMaterial(q.materialAssetId, this);
                if (mv.program && (mv.program->usesTime() || mv.program->usesBackdrop())) { *animated = true; break; }
            }
    }
    return o;
}

SoftwareRenderer::SoftwareRenderer() = default;
SoftwareRenderer::~SoftwareRenderer() = default;

void SoftwareRenderer::Initialize(HE::Window* window)
{
    m_window = window;
    m_primary = Target{};
    m_primary.window = window;
    HE_LOG_INFO(RHI, "%s",
        "SoftwareRenderer: CPU rasterizer, user interface only (no GPU, no driver)");
}

void SoftwareRenderer::Shutdown()
{
    m_window = nullptr;
    m_primary = Target{};
    m_secondaries.clear();
    m_decoded.clear();
    m_materials.clear();
    m_functions.clear();
}

void SoftwareRenderer::AttachWindow(HE::Window* window)
{
    if (!window) return;
    for (const Target& t : m_secondaries)
        if (t.window == window) return;   // already attached
    Target t;
    t.window = window;
    m_secondaries.push_back(std::move(t));
    HE_LOG_INFO(RHI, "SoftwareRenderer: window %u attached (%zu secondary window(s))",
                window->GetWindowId(), m_secondaries.size());
}

void SoftwareRenderer::DetachWindow(HE::Window* window)
{
    // The surface is SDL's and goes with the window; what is dropped here is the
    // frame buffer and the diff history, which are the only things this backend
    // ever owned per window.
    for (std::size_t i = 0; i < m_secondaries.size(); ++i)
        if (m_secondaries[i].window == window)
        {
            m_secondaries.erase(m_secondaries.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

void SoftwareRenderer::RenderWindow(HE::Window* window)
{
    if (!window) return;
    for (Target& t : m_secondaries)
        if (t.window == window) { renderTarget(t, window->GetWindowId()); return; }
}

IRenderer::Capabilities SoftwareRenderer::GetCapabilities() const
{
    // Everything false, and that is the honest answer rather than a limitation:
    // this backend exists for applications, which have no shadows, no post
    // processing and no scene to light. The editor greys the matching settings
    // out from exactly these flags.
    Capabilities c{};
    // The one thing it CAN do that the GPU backends mostly cannot: a second
    // window is a second SDL surface, and drawing a widget tree into a surface
    // is all this backend does anyway.
    c.supportsSecondaryWindows = true;
    return c;
}

IRenderer::FrameGpuStats SoftwareRenderer::GetFrameGpuStats() const
{
    FrameGpuStats s;
    // There is no GPU to time. The two numbers that mean something for a CPU
    // rasterizer are how many quads it was given and how many pixels it actually
    // wrote — the second is the dirty-rectangle payoff, and it goes where the
    // profiler already looks for triangles.
    // The MAIN window's numbers. A tool window's repaint is not what a profiler
    // capture of the application is about, and adding the two would make a
    // second window look like a main window that got slower.
    s.gpuFrameMs   = -1.0f;
    s.drawCalls    = m_primary.lastQuads;
    s.totalObjects = m_primary.lastQuads;
    s.triangles    = static_cast<uint32_t>(std::min<uint64_t>(m_primary.lastPixels, 0xFFFFFFFFull));
    return s;
}

void SoftwareRenderer::Render()
{
    renderTarget(m_primary, 0);
}

// One window, start to finish. The only two things it is told apart from the
// buffers are which SDL window to blit into and which window's widgets to ask
// the extractor for — everything else was already per-window arithmetic that
// happened to have exactly one window to do it for.
void SoftwareRenderer::renderTarget(Target& t, uint32_t windowId)
{
    if (!t.window) return;
    SDL_Window* sdl = t.window->GetNativeWindow();
    if (!sdl) return;

    // The window's own surface — the one a window without a graphics API already
    // has. No context, no swapchain, no drawable.
    SDL_Surface* surf = SDL_GetWindowSurface(sdl);
    if (!surf)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            HE_LOG_ERROR(RHI, "SoftwareRenderer: no window surface (%s)", SDL_GetError());
        }
        return;
    }
    const int pw = surf->w, ph = surf->h;
    if (pw <= 0 || ph <= 0) return;

    // The same extraction the GPU backends do — a widget tree arrives here
    // already turned into quads.
    if (m_world)
    {
        m_extractor.setContentManager(m_contentManager);
        m_extractor.extractUI(*m_world, static_cast<float>(pw), static_cast<float>(ph),
                              m_renderWorld, windowId);
    }
    else
        m_renderWorld.uiObjects.clear();
    t.lastQuads = static_cast<uint32_t>(m_renderWorld.uiObjects.size());

    // ── Full frame, or only what changed? ────────────────────────────────────
    // The reasons for a full one are all "we cannot trust what is already
    // there": a resize, the first frame, a surface SDL swapped under us, or a
    // diff so big that tracking it costs more than repainting.
    const bool sizeChanged = (t.frame.width != pw || t.frame.height != ph);
    if (sizeChanged) t.frame.resize(pw, ph);
    const bool surfaceChanged = (surf != t.lastSurface);
    t.lastSurface = surf;

    // A material that reads the clock or what is behind it can change while
    // its quad does not, and a backdrop read outside a repainted rectangle
    // would see last frame's finished picture — so a frame with either is
    // painted whole, as every GPU frame is.
    bool animated = false;
    HE::sw::DrawOptions opts = drawOptions(m_renderWorld.uiObjects, &animated);
    bool partial = !animated && !sizeChanged && !surfaceChanged && t.haveFrame &&
                   HE::sw::dirtyRects(t.prevObjects, m_renderWorld.uiObjects, pw, ph, t.dirty);
    if (partial && t.dirty.empty())
    {
        // Nothing at all changed. Not one pixel is written and the surface is
        // not even touched — this is the case event-driven drawing (A2) is meant
        // to reach, and the one where a CPU rasterizer costs nothing.
        t.lastPixels = 0;
        t.prevObjects = m_renderWorld.uiObjects;
        return;
    }
    if (!partial)
    {
        t.dirty.assign(1, glm::vec4(0.0f, 0.0f, static_cast<float>(pw),
                                    static_cast<float>(ph)));
    }

    t.lastPixels = 0;
    for (const glm::vec4& r : t.dirty)
    {
        t.frame.clearRect(r, kClear[0], kClear[1], kClear[2], kClear[3]);
        opts.clip = r;
        HE::sw::draw(t.frame, m_renderWorld.uiObjects, opts);
        t.lastPixels += static_cast<uint64_t>(std::max(0.0f, r.z) * std::max(0.0f, r.w));
    }
    t.prevObjects = m_renderWorld.uiObjects;
    t.haveFrame = true;

    // Blit the same regions into the surface. SDL surfaces are BGRA/ARGB on the
    // common desktop formats, so the channels are placed through the surface's
    // own format rather than memcpy'd — the one place where "it looked blue"
    // would come from.
    std::vector<SDL_Rect> present;
    present.reserve(t.dirty.size());
    if (SDL_LockSurface(surf))
    {
        const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(surf->format);
        auto* rows = static_cast<std::uint8_t*>(surf->pixels);
        for (const glm::vec4& r : t.dirty)
        {
            const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
            const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
            const int x1 = std::min(pw, static_cast<int>(std::ceil(r.x + r.z)));
            const int y1 = std::min(ph, static_cast<int>(std::ceil(r.y + r.w)));
            if (x1 <= x0 || y1 <= y0) continue;
            for (int y = y0; y < y1; ++y)
            {
                auto* dst = reinterpret_cast<std::uint32_t*>(
                    rows + static_cast<std::size_t>(y) * surf->pitch);
                const std::uint8_t* src =
                    t.frame.rgba.data() + (static_cast<std::size_t>(y) * pw + x0) * 4;
                for (int x = x0; x < x1; ++x, src += 4)
                    dst[x] = SDL_MapRGBA(fmt, nullptr, src[0], src[1], src[2], 255);
            }
            present.push_back(SDL_Rect{ x0, y0, x1 - x0, y1 - y0 });
        }
        SDL_UnlockSurface(surf);
    }
    if (!present.empty())
        SDL_UpdateWindowSurfaceRects(sdl, present.data(), static_cast<int>(present.size()));
}

bool SoftwareRenderer::CaptureViewport(std::vector<uint8_t>& rgba,
                                       uint32_t& width, uint32_t& height)
{
    // The main window, always: the headless dump and the thumbnail path both
    // mean "the application", and the application is the window it started in.
    if (!m_primary.frame.valid()) return false;
    rgba   = m_primary.frame.rgba;
    width  = static_cast<uint32_t>(m_primary.frame.width);
    height = static_cast<uint32_t>(m_primary.frame.height);
    return true;
}

bool SoftwareRenderer::RasterizeUI(const std::vector<UIRenderObject>& uiObjects,
                                   uint32_t width, uint32_t height,
                                   const glm::vec4& clear, std::vector<uint8_t>& outRgba8,
                                   bool* animated)
{
    if (width == 0 || height == 0) return false;
    HE::sw::Image img;
    img.resize(static_cast<int>(width), static_cast<int>(height));
    auto b = [](float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    img.clear(b(clear.r), b(clear.g), b(clear.b), 255);
    HE::sw::draw(img, uiObjects, drawOptions(uiObjects, animated));
    outRgba8 = std::move(img.rgba);
    return true;
}

bool SoftwareRenderer::RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
                                             uint32_t size, std::vector<uint8_t>& outRgba8)
{
    if (size == 0) return false;
    HE::sw::Image img;
    img.resize(static_cast<int>(size), static_cast<int>(size));
    img.clear(0, 0, 0, 0);
    HE::sw::draw(img, uiObjects, drawOptions(uiObjects, nullptr));
    outRgba8 = std::move(img.rgba);
    return true;
}
