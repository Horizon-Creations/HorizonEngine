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
        WindowState state  = WindowState::Floating;
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
        bool        IsPrimary()   const { return m_isPrimary; }
        uint32_t    GetWindowId() const;
        uint32_t    GetWidth()    const;
        uint32_t    GetHeight()   const;
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
        uint32_t      m_width         = 0;
        uint32_t      m_height        = 0;
        GraphicsAPI   m_api           = GraphicsAPI::OpenGL;
        EventCallback m_eventCallback;
    };
}
