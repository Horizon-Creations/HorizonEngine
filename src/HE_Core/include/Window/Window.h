#pragma once
#include "Types/Defines.h"
#include "Types/Enums.h"
#include <string>
#include <cstdint>
#include <functional>
#include <SDL3/SDL_events.h>

struct SDL_Window;

namespace HE
{
    struct WindowProps
    {
        std::string title  = "HorizonEngine";
        uint32_t    width  = 1280;
        uint32_t    height = 720;
        bool        vsync  = true;
        WindowMode  mode   = WindowMode::Windowed;
        // Set by Application::Run based on the chosen backend.
        // Window::Init uses this to set the correct SDL window flags
        // and to skip GL context creation for D3D / Vulkan.
        GraphicsAPI api    = GraphicsAPI::OpenGL;
    };

    class HE_API Window
    {
    public:
        // isPrimary=true  → Window calls SDL_Init / SDL_Quit and owns the event queue
        // isPrimary=false → Secondary window; SDL is assumed to already be initialised
        explicit Window(const WindowProps& props = {}, bool isPrimary = true);
        ~Window();

        Window(const Window&)            = delete;
        Window& operator=(const Window&) = delete;

        void PollEvents();
        void SwapBuffers();

        void        SetTitle(const std::string& title);
        void        SetSize(uint32_t width, uint32_t height);
        void        SetVSync(bool enabled);
        void        SetFullscreen(bool fullscreen);
        void        SetBorderless(bool borderless);

        bool        ShouldClose() const;
        // Veto a close request that PollEvents() already registered this frame.
        // Lets the application defer an OS-level quit (X / Cmd+Q / SDL_EVENT_QUIT)
        // until the user resolves a prompt (e.g. unsaved-changes confirmation).
        void        CancelClose()       { m_shouldClose = false; }
        bool        IsPrimary()   const { return m_isPrimary; }
        uint32_t    GetWindowId() const;
        // Logical size in points — what SDL_CreateWindow / SetSize were given.
        // Kept current by PollEvents (SDL_EVENT_WINDOW_RESIZED).
        uint32_t    GetWidth()    const;
        uint32_t    GetHeight()   const;
        // Drawable size in pixels. Equals the points size unless the window was created
        // with SDL_WINDOW_HIGH_PIXEL_DENSITY (GL/Metal/Vulkan) and runs on a HiDPI display,
        // where it is the points size times the display scale.
        // Kept current by PollEvents (SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED).
        uint32_t    GetPixelWidth()  const;
        uint32_t    GetPixelHeight() const;
        SDL_Window* GetNativeWindow() const;
        void*       GetGLContext()    const;

        // Set by Application so every SDL_Event is forwarded for input processing.
        using EventCallback = std::function<void(const SDL_Event&)>;
        void SetEventCallback(EventCallback cb) { m_eventCallback = std::move(cb); }

    private:
        void Init(const WindowProps& props);
        void Shutdown();

        SDL_Window*   m_window        = nullptr;
        void*         m_glContext     = nullptr;
        bool          m_shouldClose   = false;
        bool          m_isPrimary     = true;
        uint32_t      m_width         = 0;   // points
        uint32_t      m_height        = 0;   // points
        uint32_t      m_pixelWidth    = 0;   // pixels (HiDPI-scaled points)
        uint32_t      m_pixelHeight   = 0;   // pixels (HiDPI-scaled points)
        GraphicsAPI   m_api           = GraphicsAPI::OpenGL;
        EventCallback m_eventCallback;
    };
}
