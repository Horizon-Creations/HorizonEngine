#include "Window/Window.h"
#include <cstdint>
#include <SDL3/SDL.h>
#include "Diagnostics/Log.h"
#include <stdexcept>

namespace HE
{
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
        switch (props.api)
        {
        case RendererBackend::OpenGL:
#ifdef __APPLE__
            // macOS caps OpenGL at 4.1 Core and requires a forward-compatible context
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
#endif
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
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
        }

        m_window = SDL_CreateWindow(
            props.title.c_str(),
            static_cast<int>(props.width),
            static_cast<int>(props.height),
            flags);
        if (!m_window)
        {
            HE_LOG_CRIT(Window, "SDL_CreateWindow('%s', %ux%u, flags 0x%llx) failed: %s",
                        props.title.c_str(), props.width, props.height,
                        static_cast<unsigned long long>(flags), SDL_GetError());
            throw std::runtime_error("SDL_CreateWindow failed: " + std::string(SDL_GetError()));
        }

        // m_width/m_height are the logical (points) size by construction — that is what
        // SDL_CreateWindow was given. The drawable size only equals it when HiDPI is off,
        // so seed it from SDL instead of assuming.
        {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
            m_pixelWidth  = pw > 0 ? static_cast<uint32_t>(pw) : m_width;
            m_pixelHeight = ph > 0 ? static_cast<uint32_t>(ph) : m_height;
        }
        // The logical-vs-pixel pair is the first thing to check for "everything is
        // blurry" or "the viewport is half the window" reports.
        HE_LOG_INFO(Window, "%s window created: %ux%u logical, %ux%u pixels, backend %d, "
                            "vsync %s, display '%s'",
                    m_isPrimary ? "Primary" : "Secondary", m_width, m_height,
                    m_pixelWidth, m_pixelHeight, static_cast<int>(props.api),
                    props.vsync ? "on" : "off",
                    SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window))
                        ? SDL_GetDisplayName(SDL_GetDisplayForWindow(m_window)) : "?");

        if (props.api == RendererBackend::OpenGL)
        {
            m_glContext = SDL_GL_CreateContext(m_window);
            if (!m_glContext)
            {
                // Usually a driver that cannot give the requested core profile —
                // the requested version is the essential half of the report.
                int major = 0, minor = 0;
                SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
                SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
                HE_LOG_CRIT(Window, "SDL_GL_CreateContext failed (requested GL %d.%d core): %s",
                            major, minor, SDL_GetError());
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
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
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

    bool        Window::ShouldClose()    const { return m_shouldClose; }
    uint32_t    Window::GetWindowId()    const { return m_window ? SDL_GetWindowID(m_window) : 0; }
    uint32_t    Window::GetWidth()       const { return m_width; }
    uint32_t    Window::GetHeight()      const { return m_height; }
    uint32_t    Window::GetPixelWidth()  const { return m_pixelWidth; }
    uint32_t    Window::GetPixelHeight() const { return m_pixelHeight; }
    SDL_Window* Window::GetNativeWindow()const { return m_window; }
    void*       Window::GetGLContext()   const { return m_glContext; }
}
