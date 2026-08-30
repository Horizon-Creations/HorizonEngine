#include "../../../include/Backends/Software/SoftwareRenderer.h"

#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Diagnostics/Logger.h>
#include <Window/Window.h>

#include <SDL3/SDL.h>

#include <algorithm>

namespace
{
    // The window's clear: the same near-black the other backends start from, so
    // an application does not change colour with its renderer.
    constexpr std::uint8_t kClear[4] = { 18, 18, 22, 255 };

    // Turn a texture asset into bytes the rasterizer can sample. Textures already
    // live on the CPU (TextureAsset keeps its pixels), so there is no upload path
    // to write — only this lookup.
    //
    // RGBA8 only, on purpose. BCn and ASTC are block-compressed formats meant to
    // save GPU memory for 3D; an application's UI textures are small, and the
    // export profile for a software build bakes them uncompressed. Writing a CPU
    // decoder here would be a large piece of work for a case that should not
    // arise — so an unusable format is reported once and the quad draws its tint.
    HE::sw::TextureView resolveTexture(const HE::UUID& id, void* user)
    {
        auto* cm = static_cast<ContentManager*>(user);
        if (!cm) return {};
        const TextureAsset* t = cm->getTexture(id);
        if (!t || t->data.empty() || t->width <= 0 || t->height <= 0) return {};
        if (t->format != TextureFormat::RGBA8)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                HE_LOG_WARN(RHI, "%s",
                    "SoftwareRenderer: compressed textures are not decoded on the CPU — "
                    "export UI textures as RGBA8. Those quads draw their tint instead.");
            }
            return {};
        }
        HE::sw::TextureView v;
        v.rgba   = t->data.data();
        v.width  = static_cast<int>(t->width);
        v.height = static_cast<int>(t->height);
        return v;
    }
}

SoftwareRenderer::SoftwareRenderer() = default;
SoftwareRenderer::~SoftwareRenderer() = default;

void SoftwareRenderer::Initialize(HE::Window* window)
{
    m_window = window;
    HE_LOG_INFO(RHI, "%s",
        "SoftwareRenderer: CPU rasterizer, user interface only (no GPU, no driver)");
}

void SoftwareRenderer::Shutdown()
{
    m_window = nullptr;
    m_frame = {};
}

IRenderer::Capabilities SoftwareRenderer::GetCapabilities() const
{
    // Everything false, and that is the honest answer rather than a limitation:
    // this backend exists for applications, which have no shadows, no post
    // processing and no scene to light. The editor greys the matching settings
    // out from exactly these flags.
    return Capabilities{};
}

IRenderer::FrameGpuStats SoftwareRenderer::GetFrameGpuStats() const
{
    FrameGpuStats s;
    // There is no GPU to time. The quad count is the number that means something
    // here, so it goes where the profiler already looks for draw calls.
    s.gpuFrameMs   = -1.0f;
    s.drawCalls    = m_lastQuads;
    s.totalObjects = m_lastQuads;
    return s;
}

void SoftwareRenderer::Render()
{
    if (!m_window) return;
    SDL_Window* sdl = m_window->GetNativeWindow();
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

    if (m_frame.width != pw || m_frame.height != ph) m_frame.resize(pw, ph);
    m_frame.clear(kClear[0], kClear[1], kClear[2], kClear[3]);

    // The same extraction the GPU backends do, and the same one — a widget tree
    // arrives here already turned into quads.
    if (m_world)
    {
        m_extractor.setContentManager(m_contentManager);
        m_extractor.extractUI(*m_world, static_cast<float>(pw), static_cast<float>(ph),
                              m_renderWorld);
    }
    else
        m_renderWorld.uiObjects.clear();

    HE::sw::draw(m_frame, m_renderWorld.uiObjects, glm::vec4(0.0f),
                 &resolveTexture, m_contentManager);
    m_lastQuads = static_cast<uint32_t>(m_renderWorld.uiObjects.size());

    // Blit into the surface. SDL surfaces are BGRA/ARGB on the common desktop
    // formats, so the channels are placed through the surface's own format
    // rather than memcpy'd — the one place where "it looked blue" would come
    // from.
    if (SDL_LockSurface(surf))
    {
        const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(surf->format);
        auto* rows = static_cast<std::uint8_t*>(surf->pixels);
        for (int y = 0; y < ph; ++y)
        {
            auto* dst = reinterpret_cast<std::uint32_t*>(rows + static_cast<std::size_t>(y) * surf->pitch);
            const std::uint8_t* src = m_frame.rgba.data() + static_cast<std::size_t>(y) * pw * 4;
            for (int x = 0; x < pw; ++x, src += 4)
                dst[x] = SDL_MapRGBA(fmt, nullptr, src[0], src[1], src[2], 255);
        }
        SDL_UnlockSurface(surf);
    }
    SDL_UpdateWindowSurface(sdl);
}

bool SoftwareRenderer::CaptureViewport(std::vector<uint8_t>& rgba,
                                       uint32_t& width, uint32_t& height)
{
    if (!m_frame.valid()) return false;
    rgba   = m_frame.rgba;
    width  = static_cast<uint32_t>(m_frame.width);
    height = static_cast<uint32_t>(m_frame.height);
    return true;
}

bool SoftwareRenderer::RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
                                             uint32_t size, std::vector<uint8_t>& outRgba8)
{
    if (size == 0) return false;
    HE::sw::Image img;
    img.resize(static_cast<int>(size), static_cast<int>(size));
    img.clear(0, 0, 0, 0);
    HE::sw::draw(img, uiObjects, glm::vec4(0.0f), &resolveTexture, m_contentManager);
    outRgba8 = std::move(img.rgba);
    return true;
}
