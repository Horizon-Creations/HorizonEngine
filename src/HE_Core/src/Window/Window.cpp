#include "Window/Window.h"
#include "UIWidget/UIWindowFrame.h"   // UIWindowHit — the hit test speaks it
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <SDL3/SDL.h>
#include "Diagnostics/Log.h"
#include <stdexcept>

namespace HE
{
namespace
{
    // ── The OpenGL versions a window will accept, best first ──────────────────
    // Asking for one fixed version and dying when the driver cannot give it is
    // the difference between "runs everywhere" and "runs on my machine": Mesa's
    // llvmpipe caps below 4.6, older Intel iGPUs on Windows report 4.4/4.5, and
    // VM GL stacks often stop at 3.3–4.1. Descending costs a few lines and
    // nothing at runtime, because everything above 4.1 is already gated where it
    // is used — GI checks GLAD_GL_VERSION_4_3 and turns itself off, and the UI
    // and material shaders are #version 410 core.
    struct GlVersion { int major; int minor; };

#ifdef __APPLE__
    // macOS caps OpenGL at 4.1 Core and requires a forward-compatible context,
    // so there is nothing to descend through. (A game/app on macOS runs on Metal
    // anyway; this path is for the odd GL-forced run.)
    constexpr GlVersion kGlVersions[] = { { 4, 1 } };
#else
    constexpr GlVersion kGlVersions[] = { { 4, 6 }, { 4, 5 }, { 4, 3 }, { 4, 1 } };
#endif

    // Every attribute the GL path needs, for one version. Called once before the
    // window is created (the pixel format is chosen there) and again per attempt,
    // because SDL_GL_CreateContext reads these at call time.
    void setGlAttributes(const GlVersion& v)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, v.major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, v.minor);
#ifdef __APPLE__
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    }
} // namespace

    Window::Window(const WindowProps& props, bool isPrimary) { m_isPrimary = isPrimary; Init(props); }
    Window::~Window()                        { Shutdown(); }

    void Window::Init(const WindowProps& props)
    {
        m_width  = props.width;
        m_height = props.height;
        m_api    = props.api;

        if (m_isPrimary)
        {
            if (!SDL_Init(SDL_INIT_VIDEO))
            {
                // These throws travel up to a message box and a hard exit, so the
                // reason has to reach the log before the process is gone.
                HE_LOG_CRIT(Window, "SDL_Init(VIDEO) failed: %s", SDL_GetError());
                throw std::runtime_error("SDL_Init failed: " + std::string(SDL_GetError()));
            }
            HE_LOG_INFO(Window, "SDL %d.%d.%d initialised, video driver '%s'",
                        SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                        SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                        SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
                        SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
            // Separate subsystem init, warn-only: gamepads are optional, video
            // is not, and a headless/CI environment that can do one may not be
            // able to do the other. Failure here just means no controllers.
            if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
                HE_LOG_WARN(Window, "SDL_InitSubSystem(GAMEPAD) failed: %s — controllers disabled",
                            SDL_GetError());
        }

        // Choose SDL window flags and set GL attributes only for OpenGL
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
        if (props.startHidden) flags |= SDL_WINDOW_HIDDEN;
        switch (props.api)
        {
        case RendererBackend::OpenGL:
            // Best version first; the context loop below descends if the driver
            // refuses it (see kGlVersions).
            setGlAttributes(kGlVersions[0]);
            // High-DPI: render at the display's true pixel density (Retina) so the
            // drawable isn't upscaled by the OS (otherwise the whole UI is blurry).
            // GL/Metal/Vulkan size their drawable from SDL_GetWindowSizeInPixels;
            // D3D uses the logical size (GetWidth/Height) so it is left unchanged.
            flags |= SDL_WINDOW_OPENGL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
            break;
        case RendererBackend::Vulkan:
            flags |= SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
            break;
        case RendererBackend::Metal:
            flags |= SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
            break;
        case RendererBackend::D3D11:
        case RendererBackend::D3D12:
            // Plain window — D3D creates its own swap chain via HWND
            break;
        case RendererBackend::Software:
            // A plain window and nothing else: no context, no drawable, no
            // driver. The software backend blits into the window's own SDL
            // surface, which is exactly what a window without a graphics API
            // already has. Deliberately NOT high-density: the surface a CPU
            // rasterizer fills is the logical one, and asking for four times
            // the pixels would quadruple the work for a blit that is then
            // downscaled again.
            //
            // On Windows and X11 that saving does not exist and must not be
            // faked: the window coordinates ARE pixels there, so the content
            // scale below really does hand the rasterizer four times the work
            // at 200%. The alternative is a window half the size it should be,
            // which is a bug, not a saving.
            break;
        }

        // ── The requested size is in DIPs, SDL wants window coordinates ──────
        // "1280x720" in a project's settings is a size the author saw on their
        // screen, not a count of pixels. SDL takes window coordinates, and what
        // those ARE differs by platform (docs/README-highdpi.md): points on
        // macOS and Wayland, where the system scaling is already baked in and
        // the content scale reads 1.0 — but physical PIXELS on Windows and X11,
        // where the scaling only shows up as a content scale of 1.5 or 2.0. So
        // the same 1280x720 gives a window half as wide on a Windows laptop at
        // 200% as on any Mac, and every canvas scale mode faithfully lays out
        // into that too-small window. Multiplying by the content scale is a
        // no-op on the platforms that need none and the whole fix on the two
        // that do.
        const float contentScale = m_isPrimary
            ? SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()) : 1.0f;
        uint32_t createW = props.width, createH = props.height;
        if (contentScale > 1.0f)
        {
            createW = static_cast<uint32_t>(props.width  * contentScale + 0.5f);
            createH = static_cast<uint32_t>(props.height * contentScale + 0.5f);
            // A size that fit the screen at 100% must still fit it at 200%:
            // without this a 1600x900 default becomes 3200x1800 and hangs off
            // a 2560x1440 panel, which reads as a broken app, not as a scaled
            // one. Usable bounds, so the taskbar keeps its strip.
            SDL_Rect usable{};
            if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
                usable.w > 0 && usable.h > 0)
            {
                createW = std::min(createW, static_cast<uint32_t>(usable.w));
                createH = std::min(createH, static_cast<uint32_t>(usable.h));
            }
            HE_LOG_INFO(Window, "Display content scale %.2f — requested %ux%u scaled to %ux%u",
                        static_cast<double>(contentScale), props.width, props.height,
                        createW, createH);
        }

        m_window = SDL_CreateWindow(
            props.title.c_str(),
            static_cast<int>(createW),
            static_cast<int>(createH),
            flags);
        if (!m_window)
        {
            HE_LOG_CRIT(Window, "SDL_CreateWindow('%s', %ux%u, flags 0x%llx) failed: %s",
                        props.title.c_str(), createW, createH,
                        static_cast<unsigned long long>(flags), SDL_GetError());
            throw std::runtime_error("SDL_CreateWindow failed: " + std::string(SDL_GetError()));
        }

        // m_width/m_height are the logical (points) size — asked for above, but
        // read back rather than assumed: the scaling and the clamp both changed
        // it, and a window manager may have had a further opinion. The drawable
        // size only equals it when HiDPI is off, so seed that from SDL too.
        {
            int lw = 0, lh = 0, pw = 0, ph = 0;
            SDL_GetWindowSize(m_window, &lw, &lh);
            SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
            if (lw > 0) m_width  = static_cast<uint32_t>(lw);
            if (lh > 0) m_height = static_cast<uint32_t>(lh);
            m_pixelWidth  = pw > 0 ? static_cast<uint32_t>(pw) : m_width;
            m_pixelHeight = ph > 0 ? static_cast<uint32_t>(ph) : m_height;
        }
        // The logical-vs-pixel pair is the first thing to check for "everything is
        // blurry" or "the viewport is half the window" reports; the display scale
        // beside it is the first thing to check for "everything is tiny", which is
        // the same report one platform further along.
        HE_LOG_INFO(Window, "%s window created: %ux%u logical, %ux%u pixels, display scale %.2f, "
                            "backend %d, vsync %s, display '%s'",
                    m_isPrimary ? "Primary" : "Secondary", m_width, m_height,
                    m_pixelWidth, m_pixelHeight,
                    static_cast<double>(SDL_GetWindowDisplayScale(m_window)),
                    static_cast<int>(props.api),
                    props.vsync ? "on" : "off",
                    SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window))
                        ? SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window)) : "?");

        if (props.api == RendererBackend::OpenGL)
        {
            // Descend through kGlVersions until one context sticks. Every rung
            // but the last logs a warning rather than dying: a machine that can
            // only do 4.5 should run with GI off, not refuse to start.
            for (const GlVersion& v : kGlVersions)
            {
                setGlAttributes(v);
                m_glContext = SDL_GL_CreateContext(m_window);
                if (m_glContext) break;
                HE_LOG_WARN(Window, "SDL_GL_CreateContext(GL %d.%d core) failed: %s — trying a "
                                    "lower version", v.major, v.minor, SDL_GetError());
            }
            if (!m_glContext)
            {
                // Every rung refused: the driver has no core profile we can use.
                // The list is the essential half of the report.
                HE_LOG_CRIT(Window, "SDL_GL_CreateContext failed for every requested version "
                                    "(GL %d.%d down to %d.%d core): %s",
                            kGlVersions[0].major, kGlVersions[0].minor,
                            kGlVersions[std::size(kGlVersions) - 1].major,
                            kGlVersions[std::size(kGlVersions) - 1].minor,
                            SDL_GetError());
                throw std::runtime_error("SDL_GL_CreateContext failed: " + std::string(SDL_GetError()));
            }
            if (!SDL_GL_SetSwapInterval(props.vsync ? 1 : 0))
                HE_LOG_WARN(Window, "SDL_GL_SetSwapInterval(%d) failed: %s — the driver is "
                                    "overriding vsync", props.vsync ? 1 : 0, SDL_GetError());
            {
                int major = 0, minor = 0;
                SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
                SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
                HE_LOG_INFO(Window, "OpenGL context created: %d.%d core", major, minor);
            }
        }
    }

    void Window::Shutdown()
    {
        if (m_window)
            HE_LOG_INFO(Window, "%s window destroyed", m_isPrimary ? "Primary" : "Secondary");
        if (m_glContext) { SDL_GL_DestroyContext(static_cast<SDL_GLContext>(m_glContext)); m_glContext = nullptr; }
        if (m_window)    { SDL_DestroyWindow(m_window); m_window = nullptr; }
        if (m_isPrimary) SDL_Quit();
    }

    void Window::PollEvents()
    {
        m_eventsLastPoll = 0;
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ++m_eventsLastPoll;
            if (event.type == SDL_EVENT_QUIT)
                m_shouldClose = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(m_window))
                m_shouldClose = true;

            // Keep the cached size current after a user resize — without this the getters
            // kept returning the creation size forever. SDL3 splits the notification in two
            // events that differ on HiDPI, so they must not be mixed up: RESIZED carries the
            // logical size in points, PIXEL_SIZE_CHANGED the drawable size in pixels (2x the
            // points on a Retina display). GetWidth/GetHeight have always meant the points
            // size (what SDL_CreateWindow / SetSize were given), so only RESIZED feeds them;
            // the pixel event feeds GetPixelWidth/GetPixelHeight.
            // Both are only cached when positive: a minimise reports 0x0 on some platforms
            // and consumers divide by these (aspect ratio, swapchain extent).
            if ((event.type == SDL_EVENT_WINDOW_RESIZED ||
                 event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) &&
                event.window.windowID == SDL_GetWindowID(m_window) &&
                event.window.data1 > 0 && event.window.data2 > 0)
            {
                const uint32_t w = static_cast<uint32_t>(event.window.data1);
                const uint32_t h = static_cast<uint32_t>(event.window.data2);
                if (event.type == SDL_EVENT_WINDOW_RESIZED) { m_width      = w; m_height      = h; }
                else                                        { m_pixelWidth = w; m_pixelHeight = h; }
            }

            if (m_eventCallback) m_eventCallback(event);
        }
    }

    void Window::WaitForEvent(int timeoutMs)
    {
        // Null event pointer = wait for something to arrive but leave it queued,
        // so the caller's PollEvents() still dispatches it normally. Returns
        // false on timeout, which is not an error here: the caller wakes up on a
        // heartbeat either way.
        SDL_WaitEventTimeout(nullptr, timeoutMs);
    }

    void Window::SwapBuffers()
    {
        if (m_api == RendererBackend::OpenGL)
            SDL_GL_SwapWindow(m_window);
        // D3D/Vulkan present is called by the backend inside Render()
    }

    void Window::SetTitle(const std::string& title)
    {
        if (m_window) SDL_SetWindowTitle(m_window, title.c_str());
    }

    void Window::SetSize(uint32_t width, uint32_t height)
    {
        if (m_window)
        {
            SDL_SetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
            m_width  = width;
            m_height = height;
            // Pull the matching drawable size right away instead of waiting for the
            // SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED that follows, so the two caches never
            // disagree within the same frame. If the platform applies the resize
            // asynchronously the event still corrects it.
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
            if (pw > 0 && ph > 0)
            {
                m_pixelWidth  = static_cast<uint32_t>(pw);
                m_pixelHeight = static_cast<uint32_t>(ph);
            }
        }
    }

    void Window::SetVSync(bool enabled)
    {
        if (m_api == RendererBackend::OpenGL && m_window && m_glContext)
        {
            SDL_GL_MakeCurrent(m_window, static_cast<SDL_GLContext>(m_glContext));
            SDL_GL_SetSwapInterval(enabled ? 1 : 0);
        }
    }

    void Window::SetFullscreen(bool fullscreen)
    {
        if (m_window) SDL_SetWindowFullscreen(m_window, fullscreen);
    }

    void Window::SetBorderless(bool borderless)
    {
        if (m_window) SDL_SetWindowBordered(m_window, !borderless);
    }

    void Window::Minimize()
    {
        if (m_window) SDL_MinimizeWindow(m_window);
    }

    void Window::Maximize()
    {
        if (m_window) SDL_MaximizeWindow(m_window);
    }

    void Window::Restore()
    {
        if (m_window) SDL_RestoreWindow(m_window);
    }

    bool Window::IsMaximized() const
    {
        // Asked of SDL every time rather than tracked: the person at the keyboard
        // maximises windows too, by double-clicking the bar or hitting the OS's
        // own chord, and none of that comes through here.
        return m_window && (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0;
    }

    // UIWindowHit is SDL_HitTestResult with names of its own — the callback
    // below is a cast, and these are what keeps that true. Written out one by
    // one rather than as "first and last match", because a value swapped in the
    // middle is exactly the mistake that would otherwise survive.
    static_assert((int)UIWindowHit::Normal            == SDL_HITTEST_NORMAL);
    static_assert((int)UIWindowHit::Drag              == SDL_HITTEST_DRAGGABLE);
    static_assert((int)UIWindowHit::ResizeTopLeft     == SDL_HITTEST_RESIZE_TOPLEFT);
    static_assert((int)UIWindowHit::ResizeTop         == SDL_HITTEST_RESIZE_TOP);
    static_assert((int)UIWindowHit::ResizeTopRight    == SDL_HITTEST_RESIZE_TOPRIGHT);
    static_assert((int)UIWindowHit::ResizeRight       == SDL_HITTEST_RESIZE_RIGHT);
    static_assert((int)UIWindowHit::ResizeBottomRight == SDL_HITTEST_RESIZE_BOTTOMRIGHT);
    static_assert((int)UIWindowHit::ResizeBottom      == SDL_HITTEST_RESIZE_BOTTOM);
    static_assert((int)UIWindowHit::ResizeBottomLeft  == SDL_HITTEST_RESIZE_BOTTOMLEFT);
    static_assert((int)UIWindowHit::ResizeLeft        == SDL_HITTEST_RESIZE_LEFT);

    void Window::SetHitTest(HitTestCallback cb)
    {
        m_hitTest = std::move(cb);
        if (!m_window) return;
        if (!m_hitTest) { SDL_SetWindowHitTest(m_window, nullptr, nullptr); return; }
        SDL_SetWindowHitTest(m_window,
            [](SDL_Window*, const SDL_Point* area, void* data) -> SDL_HitTestResult
            {
                auto* self = static_cast<Window*>(data);
                if (!self || !self->m_hitTest || !area) return SDL_HITTEST_NORMAL;
                return (SDL_HitTestResult)(int)self->m_hitTest(area->x, area->y);
            },
            this);
    }

    void Window::Show()
    {
        if (!m_window) return;
        SDL_ShowWindow(m_window);
        // Raise as well as show: the splash it was hidden behind was
        // always-on-top, and on macOS the newly shown window does not
        // necessarily come forward on its own.
        SDL_RaiseWindow(m_window);
    }

    bool        Window::ShouldClose()    const { return m_shouldClose; }
    uint32_t    Window::GetWindowId()    const { return m_window ? SDL_GetWindowID(m_window) : 0; }
    uint32_t    Window::GetWidth()       const { return m_width; }
    uint32_t    Window::GetHeight()      const { return m_height; }
    uint32_t    Window::GetPixelWidth()  const { return m_pixelWidth; }
    uint32_t    Window::GetPixelHeight() const { return m_pixelHeight; }
    SDL_Window* Window::GetNativeWindow()const { return m_window; }
    void*       Window::GetGLContext()   const { return m_glContext; }
}
