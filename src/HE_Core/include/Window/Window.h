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
    // Declared, not included: UIWindowFrame.h drags the whole widget model in,
    // and a window only ever passes this value through. Scoped enums with a
    // fixed underlying type are complete enough to be forward declared.
    enum class UIWindowHit : uint8_t;

    struct WindowProps
    {
        std::string     title  = "HorizonEngine";
        uint32_t        width  = 1280;
        uint32_t        height = 720;
        bool            vsync  = true;
        WindowMode      mode   = WindowMode::Windowed;
        // Set by Application::Run based on the chosen backend.
        // Window::Init uses this to set the correct SDL window flags
        // and to skip GL context creation for D3D / Vulkan.
        RendererBackend api    = RendererBackend::OpenGL;
        // Create the window without showing it, so the caller decides when it
        // first appears. Used by the startup splash: the primary window exists
        // (the renderer needs a surface to initialise against) for a full second
        // before there is anything drawn in it, and an empty black rectangle in
        // front of the splash is precisely what the splash is there to avoid.
        // Application::Run shows it once OnInit returns.
        bool            startHidden = false;
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

        // How many SDL events the last PollEvents() actually handled. An
        // event-driven application (docs/he-apps-plan.md A2) redraws when this
        // is non-zero and sleeps when it is not, which is the whole difference
        // between an idle app costing nothing and one costing a core.
        uint32_t    EventsLastPoll() const { return m_eventsLastPoll; }
        // Block until an event arrives or the timeout expires. The event is
        // LEFT in the queue (SDL takes a null event pointer for exactly this),
        // so the PollEvents() that follows still sees and dispatches it.
        static void WaitForEvent(int timeoutMs);

        void        SetTitle(const std::string& title);
        void        SetSize(uint32_t width, uint32_t height);
        void        SetVSync(bool enabled);
        void        SetFullscreen(bool fullscreen);
        void        SetBorderless(bool borderless);

        // ── The other two title-bar buttons (plan F3) ───────────────────────
        // A window without a system frame draws its own three buttons, and two
        // of them had nothing to call. These are that: minimise puts it in the
        // dock or the taskbar, maximise fills the work area, restore puts it
        // back where it was.
        //
        // Maximise and restore are two calls rather than one toggle because SDL
        // has two, and because a title bar is not the only caller: a graph that
        // says "start maximised" means it, whatever the window is doing now.
        // Whether it IS maximised is the question that turns them into a
        // toggle, and it is asked of the platform rather than remembered here —
        // a person can maximise a window by double-clicking its bar, and a
        // cached bool would then be wrong until the next click.
        void        Minimize();
        void        Maximize();
        void        Restore();
        bool        IsMaximized() const;

        // ── The frame a borderless window has to answer for (plan F3) ───────
        // Without a system title bar the OS still asks, on every mouse move,
        // which part of the window a point belongs to: the caption it can be
        // dragged by, an edge it can be resized at, or ordinary content. This
        // installs that answer; a null callback takes it away again.
        //
        // The point handed to the callback is in WINDOW POINTS, which on a
        // Retina display is not what the UI lays out in — the caller converts.
        // Called from inside the event pump on every motion, so it must not
        // allocate, log, or take a lock.
        using HitTestCallback = std::function<UIWindowHit(int x, int y)>;
        void        SetHitTest(HitTestCallback cb);
        // Reveal a window created with WindowProps::startHidden, and put it in
        // front — the splash it was hiding behind was always-on-top.
        void        Show();

        bool        ShouldClose() const;
        // Veto a close request that PollEvents() already registered this frame.
        // Lets the application defer an OS-level quit (X / Cmd+Q / SDL_EVENT_QUIT)
        // until the user resolves a prompt (e.g. unsaved-changes confirmation).
        void        CancelClose()       { m_shouldClose = false; }
        bool        IsPrimary()   const { return m_isPrimary; }
        // The graphics API this window was CREATED for — the SDL flags follow
        // from it and cannot be changed afterwards. A secondary window has to
        // agree with the primary here: the one renderer that draws into both
        // asks the window for a GL context or a Metal layer, and a window
        // created without the matching flag has neither.
        RendererBackend GetApi()  const { return m_api; }
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

        SDL_Window*     m_window        = nullptr;
        void*           m_glContext     = nullptr;
        bool            m_shouldClose   = false;
        bool            m_isPrimary     = true;
        uint32_t        m_width         = 0;   // points
        uint32_t        m_height        = 0;   // points
        uint32_t        m_pixelWidth    = 0;   // pixels (HiDPI-scaled points)
        uint32_t        m_pixelHeight   = 0;   // pixels (HiDPI-scaled points)
        uint32_t        m_eventsLastPoll = 0;  // events handled by the last PollEvents()
        RendererBackend m_api           = RendererBackend::OpenGL;
        EventCallback   m_eventCallback;
        HitTestCallback m_hitTest;
    };
}
