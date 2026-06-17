#include "Application/Application.h"
#include "Window/Window.h"
#include "Diagnostics/Logger.h"
#include "Diagnostics/Profiler.h"
#include <SDL3/SDL.h>
#include <stdexcept>
#include <string>

namespace HE
{
	Application::Application(std::string startupPath)
	{
		m_globalState = &GlobalState::getInstance();
		m_globalState->setLogFile(startupPath);
		Logger::Log(Logger::LogLevel::Info, "Application starting up");
		m_globalState->readConfig();
		auto contentPath = startupPath + "Content";
		m_contentManager = ContentManager(contentPath);
	}
	Application::~Application()
	{
		Logger::Log(Logger::LogLevel::Info, "Application destructor called");
		m_globalState->writeConfig();
		Logger::Log(Logger::LogLevel::Info, "Application shutdown complete");
	}

	int Application::Run(int argc, char** argv)
	{
		(void)argc;
		(void)argv;

		const ApplicationConfig cfg = GetConfig();
		Logger::Log(Logger::LogLevel::Info, "Configuration loaded");

		m_loop = GameLoop({ cfg.fixedTimestep, cfg.maxFixedSteps, cfg.windowprops.vsync });

		// RendererBackend and GraphicsAPI share the same underlying values.
		WindowProps wp = cfg.windowprops;
		wp.api = static_cast<HE::GraphicsAPI>(cfg.backend);
		m_window = std::make_unique<Window>(wp);
		m_window->SetEventCallback([this](const SDL_Event& e)
		{
			// Forward window-close events for secondary windows
			if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
			{
				auto it = m_secondaryWindows.find(e.window.windowID);
				if (it != m_secondaryWindows.end())
				{
					if (m_renderer) m_renderer->DetachWindow(it->second.get());
					m_secondaryWindows.erase(it);
					return;
				}
			}
			// Give the derived application a chance to handle/consume the event first
			// (e.g. EditorApplication forwards it to ImGui).
			if (!OnEvent(e))
				m_input.ProcessEvent(e);
		});
		Logger::Log(Logger::LogLevel::Info, "Window created");

		// HiDPI diagnostic: logical points vs physical pixels. A ratio > 1 means
		// the high-pixel-density drawable is active (no blurry OS upscaling).
		if (SDL_Window* sw = m_window->GetNativeWindow())
		{
			int lw = 0, lh = 0, pw = 0, ph = 0;
			SDL_GetWindowSize(sw, &lw, &lh);
			SDL_GetWindowSizeInPixels(sw, &pw, &ph);
			Logger::Log(Logger::LogLevel::Info,
				("Window size: " + std::to_string(lw) + "x" + std::to_string(lh) +
				 " logical, " + std::to_string(pw) + "x" + std::to_string(ph) +
				 " pixels (HiDPI scale " +
				 std::to_string(lw > 0 ? (float)pw / (float)lw : 1.0f) + ")").c_str());
		}

		switch (cfg.windowprops.mode)
		{
			case WindowMode::Fullscreen: m_window->SetFullscreen(true);  break;
			case WindowMode::Borderless: m_window->SetBorderless(true);  break;
			default: break;
		}

		m_renderer = CreateRenderer();
		try
		{
			if (m_renderer)
			{
				m_renderer->SetContentManager(&m_contentManager);
				m_renderer->Initialize(m_window.get());
				// Window already set the GL swap interval at creation; apply the
				// configured VSync to the renderer too so Metal/Vulkan/D3D start in
				// the right present mode.
				m_renderer->SetVSync(cfg.windowprops.vsync);
				Logger::Log(Logger::LogLevel::Info, "Renderer initialized");
			}
			else
			{
				Logger::Log(Logger::LogLevel::Warning, "No renderer created — running without graphics");
			}
		}
		catch (const std::exception& e)
		{
			Logger::Log(Logger::LogLevel::Critical, e.what());
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Renderer Init Failed", e.what(), nullptr);
			return 1;
		}

		OnInit();
		Logger::Log(Logger::LogLevel::Info, "OnInit complete — entering main loop");

		m_running = true;
		Uint64 lastTick = SDL_GetTicksNS();

		while (m_running && !m_window->ShouldClose())
		{
			const Uint64 nowTick = SDL_GetTicksNS();
			const Uint64 delta   = nowTick - lastTick;
			const float  dt      = delta > 0 ? static_cast<float>(delta) * 1e-9f
											 : (1.0f / 60.0f);
			lastTick = nowTick;

			m_window->PollEvents();
			if (m_window->ShouldClose()) break;

			try
				{
					// OnRender first: builds ImGui frame and calls ImGui::Render()
					// so GetDrawData() is valid when the renderer's overlay callback fires.
					OnRender(dt);
					if (m_renderer)
						m_renderer->Render();

					// Secondary windows
					for (auto& [id, win] : m_secondaryWindows)
					{
						if (m_renderer) m_renderer->RenderWindow(win.get());
						win->SwapBuffers();
					}
				}
			catch (const std::exception& e)
			{
				Logger::Log(Logger::LogLevel::Error, e.what());
				SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Render Error", e.what(), nullptr);
				m_running = false;
				break;
			}

			if (m_world)
				m_loop.tick(*m_world, m_logicLoader.logic(), dt);

			m_window->SwapBuffers();
			HE_PROFILE_FRAME();
		}

		Logger::Log(Logger::LogLevel::Info, "Main loop exited — shutting down");
		OnShutdown();
		// Detach and destroy secondary windows first
		for (auto& [id, win] : m_secondaryWindows)
			if (m_renderer) m_renderer->DetachWindow(win.get());
		m_secondaryWindows.clear();
		if (m_renderer)
			m_renderer->Shutdown();
		m_renderer.reset();
		m_window.reset();
		Logger::Log(Logger::LogLevel::Info, "Application shutdown complete");
		return 0;
	}

    void Application::Quit()
    {
        m_running = false;
        m_loop.requestStop();
    }

    HE::WindowHandle Application::createSecondaryWindow(const WindowProps& props)
    {
        if (!m_window)
        {
            Logger::Log(Logger::LogLevel::Warning, "createSecondaryWindow called before Run() — ignoring");
            return {};
        }
        auto win = std::make_unique<Window>(props, /*isPrimary=*/false);
        uint32_t id = win->GetWindowId();
        if (m_renderer) m_renderer->AttachWindow(win.get());
        m_secondaryWindows[id] = std::move(win);
        Logger::Log(Logger::LogLevel::Info, ("Secondary window created (id=" + std::to_string(id) + ")").c_str());
        return { id };
    }

    void Application::destroyWindow(WindowHandle handle)
    {
        auto it = m_secondaryWindows.find(handle.id);
        if (it == m_secondaryWindows.end()) return;
        if (m_renderer) m_renderer->DetachWindow(it->second.get());
        m_secondaryWindows.erase(it);
        Logger::Log(Logger::LogLevel::Info, ("Secondary window destroyed (id=" + std::to_string(handle.id) + ")").c_str());
    }

    Window* Application::getWindow(WindowHandle handle) const
    {
        auto it = m_secondaryWindows.find(handle.id);
        return it != m_secondaryWindows.end() ? it->second.get() : nullptr;
    }

    void Application::setWindowTitle(const std::string& title)
    {
        if (m_window) m_window->SetTitle(title);
    }

    void Application::setWindowSize(uint32_t width, uint32_t height)
    {
        if (m_window) m_window->SetSize(width, height);
    }

    void Application::setVSync(bool enabled)
    {
        if (m_window) m_window->SetVSync(enabled);
    }

    void Application::setWindowMode(WindowMode mode)
    {
        if (!m_window) return;
        switch (mode)
        {
            case WindowMode::Fullscreen:
                m_window->SetFullscreen(true);
                m_window->SetBorderless(false);
                break;
            case WindowMode::Borderless:
                m_window->SetFullscreen(false);
                m_window->SetBorderless(true);
                break;
            case WindowMode::Windowed:
                m_window->SetFullscreen(false);
                m_window->SetBorderless(false);
                break;
        }
    }
}
