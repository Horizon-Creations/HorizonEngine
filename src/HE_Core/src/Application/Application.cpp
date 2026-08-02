#include "Application/Application.h"
#include <cstdint>
#include "Window/Window.h"
#include "Diagnostics/Logger.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/EngineProfiler.h"
#include <SDL3/SDL.h>
#include <stdexcept>
#include <string>

namespace {
const char* rhiName(HE::RendererBackend api)
{
	switch (api)
	{
		case HE::RendererBackend::OpenGL: return "OpenGL";
		case HE::RendererBackend::Metal:  return "Metal";
		case HE::RendererBackend::D3D11:  return "D3D11";
		case HE::RendererBackend::D3D12:  return "D3D12";
		case HE::RendererBackend::Vulkan: return "Vulkan";
		default:                          return "unknown";
	}
}
} // namespace

namespace HE
{
	Application::Application(std::string startupPath)
	{
		m_globalState = &GlobalState::getInstance();
		// Named before the first record so every main-thread line is attributable;
		// worker threads name themselves in JobSystem.
		HE::Log::setThreadName("Main");
		m_globalState->setLogFile(startupPath);
		{
			// The banner is the first thing in the file: without it a log is just a
			// stream of messages with no idea which build, OS or machine produced it.
			char* argv0[1] = { const_cast<char*>(startupPath.c_str()) };
			HE::Log::logStartupBanner("HorizonEngine", 1, argv0);
		}
		m_globalState->readConfig();
		auto contentPath = startupPath + "Content";
		m_contentManager.setContentRoot(contentPath);
		HE_LOG_INFO(Core, "Content root: %s", contentPath.c_str());
	}
	Application::~Application()
	{
		HE_LOG_INFO(Core, "%s", "Application destructor called");
		m_globalState->writeConfig();
		HE_LOG_INFO(Core, "%s", "Application shutdown complete");
		HE::Log::logShutdownSummary();
		HE::Log::closeLogFile();
	}

	int Application::Run(int argc, char** argv)
	{
		if (argc > 1 && argv)
		{
			std::string cmd;
			for (int i = 1; i < argc; ++i) { if (!cmd.empty()) cmd += ' '; cmd += argv[i] ? argv[i] : ""; }
			HE_LOG_INFO(Core, "Command line: %s", cmd.c_str());
		}

		const ApplicationConfig cfg = GetConfig();
		HE_LOG_INFO(Core, "Configuration: backend=%s, window='%s' %ux%u mode=%d, vsync=%s, "
		                  "fixedTimestep=%.4f s, maxFixedSteps=%u",
		            rhiName(cfg.backend), cfg.windowprops.title.c_str(),
		            cfg.windowprops.width, cfg.windowprops.height,
		            static_cast<int>(cfg.windowprops.mode),
		            cfg.windowprops.vsync ? "on" : "off",
		            cfg.fixedTimestep, cfg.maxFixedSteps);

		m_loop = GameLoop({ cfg.fixedTimestep, cfg.maxFixedSteps });

		WindowProps wp = cfg.windowprops;
		wp.api = cfg.backend;
		m_window = std::make_unique<Window>(wp);
		m_window->SetEventCallback([this](const SDL_Event& e)
		{
			// F9 toggles a profiler benchmark capture, engine-wide (editor + game).
			if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F9 && !e.key.repeat)
			{
				toggleProfilerCapture();
				return;
			}
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
		HE_LOG_INFO(Core, "%s", "Window created");

		// HiDPI diagnostic: logical points vs physical pixels. A ratio > 1 means
		// the high-pixel-density drawable is active (no blurry OS upscaling).
		if (SDL_Window* sw = m_window->GetNativeWindow())
		{
			int lw = 0, lh = 0, pw = 0, ph = 0;
			SDL_GetWindowSize(sw, &lw, &lh);
			SDL_GetWindowSizeInPixels(sw, &pw, &ph);
			HE_LOG_INFO(Core, "%s",
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
				HE_LOG_INFO(Core, "%s", "Renderer initialized");
			}
			else
			{
				HE_LOG_WARN(Core, "%s", "No renderer created — running without graphics");
			}
		}
		catch (const std::exception& e)
		{
			HE_LOG_CRIT(Core, "%s", e.what());
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Renderer Init Failed", e.what(), nullptr);
			return 1;
		}

		OnInit();
		HE_LOG_INFO(Core, "%s", "OnInit complete — entering main loop");

		m_running = true;
		m_vsyncEnabled = cfg.windowprops.vsync;
		m_savedVsync   = m_vsyncEnabled;
		Uint64 lastTick = SDL_GetTicksNS();
		EngineProfiler& profiler = EngineProfiler::instance();

		while (m_running && !m_window->ShouldClose())
		{
			const Uint64 nowTick = SDL_GetTicksNS();
			const Uint64 delta   = nowTick - lastTick;
			const float  dt      = delta > 0 ? static_cast<float>(delta) * 1e-9f
											 : (1.0f / 60.0f);
			lastTick = nowTick;

			// Stamp every log record produced this frame with the frame index, so a
			// message can be lined up against a profiler capture or a video.
			HE::Log::setFrameNumber(++m_frameIndex);

			// Hitch detector. A frame this long is always worth knowing about — it is
			// usually a synchronous asset load, a shader compile or a GC-like stall in
			// a script. Throttled so a systematically slow scene logs once a second
			// instead of every frame.
			if (dt > kHitchSeconds && m_frameIndex > kHitchWarmupFrames)
				HE_LOG_THROTTLE(Core, Warning, 1.0,
				                "Frame hitch: %.1f ms (frame %llu)", dt * 1000.0f,
				                static_cast<unsigned long long>(m_frameIndex));

			// Applies any pending start/stop (from F9) on the frame boundary so a
			// frame is always recorded whole or not at all.
			profiler.beginFrame(static_cast<double>(dt) * 1000.0);

			{
				HE_PROFILE_SCOPE_N("PollEvents");
				m_window->PollEvents();
			}
			if (m_window->ShouldClose()) break;

			try
				{
					// OnRender first: builds ImGui frame and calls ImGui::Render()
					// so GetDrawData() is valid when the renderer's overlay callback fires.
					{
						HE_PROFILE_SCOPE_N("OnRender");
						OnRender(dt);
					}
					if (m_renderer)
					{
						HE_PROFILE_SCOPE_N("Render");
						m_renderer->Render();
					}

					// Secondary windows
					for (auto& [id, win] : m_secondaryWindows)
					{
						if (m_renderer) m_renderer->RenderWindow(win.get());
						win->SwapBuffers();
					}
				}
			catch (const std::exception& e)
			{
				HE_LOG_ERROR(Core, "%s", e.what());
				SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Render Error", e.what(), nullptr);
				m_running = false;
				break;
			}

			if (m_world)
			{
				HE_PROFILE_SCOPE_N("GameLogicTick");
				m_loop.tick(*m_world, m_logicLoader.logic(), dt);
			}

			{
				HE_PROFILE_SCOPE_N("SwapBuffers");
				m_window->SwapBuffers();
			}

			// Pull per-frame GPU timing + render counters from the backend while a
			// capture records (full per-pass) OR the editor's live HUD is open (cheap
			// whole-frame + counters). Zero overhead when neither is active.
			const bool profilerLive = profiler.liveEnabled();
			IRenderer::FrameGpuStats gs;
			bool gsPulled = false;
			if (m_renderer && (profiler.isRecording() || profilerLive))
			{
				gs       = m_renderer->GetFrameGpuStats();
				gsPulled = true;
				if (profiler.isRecording())
				{
					ProfRenderStats rs;
					rs.drawCalls      = gs.drawCalls;
					rs.triangles      = gs.triangles;
					rs.visibleObjects = gs.visibleObjects;
					rs.totalObjects   = gs.totalObjects;
					rs.vramUsedMB     = gs.vramUsedMB;
					rs.vramBudgetMB   = gs.vramBudgetMB;
					profiler.setRenderStats(rs);
					std::vector<ProfGpuPass> passes;
					passes.reserve(gs.passes.size());
					for (const auto& p : gs.passes) passes.push_back({ p.name, p.ms, p.approx });
					profiler.setGpuTimes(gs.gpuFrameMs, passes, gs.gpuTimingMode);
				}
			}

			profiler.endFrame();

			// Live overview sample (after endFrame so lastCpuFrameMs is this frame's).
			if (profilerLive)
			{
				ProfLiveFrame lf;
				lf.deltaMs    = static_cast<double>(dt) * 1000.0;
				lf.cpuFrameMs = profiler.lastCpuFrameMs();
				if (gsPulled)
				{
					lf.gpuFrameMs = gs.gpuFrameMs;
					lf.draws      = gs.drawCalls;
					lf.triangles  = gs.triangles;
					lf.visible    = gs.visibleObjects;
					lf.total      = gs.totalObjects;
				}
				profiler.pushLive(lf);
			}

			// If a capture just stopped, a dump was written — log its path.
			std::string dumpPath;
			if (profiler.consumeJustDumped(dumpPath) && !dumpPath.empty())
				HE_LOG_INFO(Core, "%s",
				            ("Profiler dump written: " + dumpPath).c_str());

			HE_PROFILE_FRAME();

			// Optional frame-rate ceiling, applied ONLY when VSync is OFF and the user has
			// set a limit (m_maxFps > 0). Default is 0 = UNLIMITED, so the loop runs fully
			// uncapped (no added latency, full FPS) unless the user opts into a cap — e.g.
			// to smooth the editor's high-FPS mouse-look or cut idle GPU load. Skipped while
			// the profiler benchmarks (it wants a true uncapped capture).
			if (m_maxFps > 0.0f && !m_vsyncEnabled && !profiler.isRecording())
			{
				const Uint64 frameCapNs = static_cast<Uint64>(1.0e9 / m_maxFps);
				const Uint64 elapsed = SDL_GetTicksNS() - nowTick;
				if (elapsed < frameCapNs)
					SDL_DelayNS(frameCapNs - elapsed);
			}
		}

		HE_LOG_INFO(Core, "%s", "Main loop exited — shutting down");
		OnShutdown();
		// Detach and destroy secondary windows first
		for (auto& [id, win] : m_secondaryWindows)
			if (m_renderer) m_renderer->DetachWindow(win.get());
		m_secondaryWindows.clear();
		if (m_renderer)
			m_renderer->Shutdown();
		m_renderer.reset();
		m_window.reset();
		HE_LOG_INFO(Core, "%s", "Application shutdown complete");
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
            HE_LOG_WARN(Core, "%s", "createSecondaryWindow called before Run() — ignoring");
            return {};
        }
        auto win = std::make_unique<Window>(props, /*isPrimary=*/false);
        uint32_t id = win->GetWindowId();
        if (m_renderer) m_renderer->AttachWindow(win.get());
        m_secondaryWindows[id] = std::move(win);
        HE_LOG_INFO(Core, "%s", ("Secondary window created (id=" + std::to_string(id) + ")").c_str());
        return { id };
    }

    void Application::destroyWindow(WindowHandle handle)
    {
        auto it = m_secondaryWindows.find(handle.id);
        if (it == m_secondaryWindows.end()) return;
        if (m_renderer) m_renderer->DetachWindow(it->second.get());
        m_secondaryWindows.erase(it);
        HE_LOG_INFO(Core, "%s", ("Secondary window destroyed (id=" + std::to_string(handle.id) + ")").c_str());
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
        m_vsyncEnabled = enabled;
        if (m_window)   m_window->SetVSync(enabled);
        if (m_renderer) m_renderer->SetVSync(enabled);
    }

    void Application::toggleProfilerCapture()
    {
        EngineProfiler& profiler = EngineProfiler::instance();
        if (profiler.isRecordingOrPending())
        {
            // Stop + dump on the next frame boundary; restore the pre-capture vsync.
            profiler.requestStop();
            setVSync(m_savedVsync);
            HE_LOG_INFO(Core, "%s", "Profiler: stop requested (F9)");
        }
        else
        {
            // Benchmark capture: run uncapped so frame times reflect true cost.
            m_savedVsync = m_vsyncEnabled;
            setVSync(false);

            ProfSessionInfo info;
            info.backend = rhiName(m_globalState->getSelectedRHI());
#ifdef __APPLE__
            info.os = "macOS";
#elif defined(_WIN32)
            info.os = "Windows";
#else
            info.os = "Linux";
#endif
            if (SDL_Window* sw = m_window ? m_window->GetNativeWindow() : nullptr)
            {
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(sw, &pw, &ph);
                info.width  = static_cast<uint32_t>(pw);
                info.height = static_cast<uint32_t>(ph);
            }
            info.vsync = false;
            info.note  = "F9 benchmark capture";
            // Cap the capture so a forgotten F9 can't grow the buffer (and the JSON
            // dump) unbounded at 200+ fps — keep the newest N frames as a ring.
            constexpr size_t kMaxCaptureFrames = 20000; // ~100 s @ 200 fps
            profiler.requestStart(info, kMaxCaptureFrames);
            HE_LOG_INFO(Core, "%s", "Profiler: start requested (F9, vsync off)");
        }
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
