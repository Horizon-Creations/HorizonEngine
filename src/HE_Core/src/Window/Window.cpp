#include "Window/Window.h"
#include <SDL3/SDL.h>
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
                throw std::runtime_error("SDL_Init failed: " + std::string(SDL_GetError()));
        }

        // Choose SDL window flags and set GL attributes only for OpenGL
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
        switch (props.api)
        {
        case GraphicsAPI::OpenGL:
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
        case GraphicsAPI::Vulkan:
            flags |= SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
            break;
        case GraphicsAPI::Metal:
            flags |= SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY;
            break;
        case GraphicsAPI::D3D11:
        case GraphicsAPI::D3D12:
            // Plain window — D3D creates its own swap chain via HWND
            break;
        }

        m_window = SDL_CreateWindow(
            props.title.c_str(),
            static_cast<int>(props.width),
            static_cast<int>(props.height),
            flags);
        if (!m_window)
            throw std::runtime_error("SDL_CreateWindow failed: " + std::string(SDL_GetError()));

        if (props.api == GraphicsAPI::OpenGL)
        {
            m_glContext = SDL_GL_CreateContext(m_window);
            if (!m_glContext)
                throw std::runtime_error("SDL_GL_CreateContext failed: " + std::string(SDL_GetError()));
            SDL_GL_SetSwapInterval(props.vsync ? 1 : 0);
        }
    }

    void Window::Shutdown()
    {
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

            if (m_eventCallback) m_eventCallback(event);
        }
    }

    void Window::SwapBuffers()
    {
        if (m_api == GraphicsAPI::OpenGL)
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
        }
    }

    void Window::SetVSync(bool enabled)
    {
        if (m_api == GraphicsAPI::OpenGL && m_window && m_glContext)
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
    SDL_Window* Window::GetNativeWindow()const { return m_window; }
    void*       Window::GetGLContext()   const { return m_glContext; }
}
