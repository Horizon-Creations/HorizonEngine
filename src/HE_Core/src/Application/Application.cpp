#include "Application/Application.h"
#include <cstdint>
#include "Window/Window.h"
#include "Diagnostics/Logger.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/EngineProfiler.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
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
		case HE::RendererBackend::Software: return "Software";
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
		// parent_path(), not string concatenation: startupPath is argv[0], the
		// EXECUTABLE, so appending "Content" produced ".../HorizonEditor.exeContent"
		// — a directory that cannot exist. It went unnoticed because the editor
		// replaces this root the moment a project loads, so only the window
		// between construction and that load ever saw it; the log line printed
		// the nonsense every run and nobody read it. GameApplication has always
		// built the same path correctly.
		const std::string contentPath =
			(std::filesystem::path(startupPath).parent_path() / "Content").string();
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

	void Application::closeSplash()
	{
		if (!m_splash.isOpen()) return;
		const bool wasGL = m_splash.usedGL();
		m_splash.close();
		// Destroying the splash's 2D renderer destroys whatever context it made
		// current, and on the OpenGL backend that can leave NO context bound —
		// the engine's next GL call would then go nowhere. Re-binding ours is
		// unconditional rather than gated on wasGL: on the platforms without a
		// native framebuffer the "software" renderer still reaches GL through
		// SDL's texture-framebuffer fallback, and it does not say so.
		if (m_window && m_window->GetGLContext())
		{
			SDL_GL_MakeCurrent(m_window->GetNativeWindow(),
			                   static_cast<SDL_GLContext>(m_window->GetGLContext()));
			if (wasGL)
				HE_LOG_INFO(Core, "%s", "Splash closed — GL context restored");
		}
	}

	int Application::Run(int argc, char** argv)
	{
		if (argc > 1 && argv)
		{
			std::string cmd;
			for (int i = 1; i < argc; ++i)
			{
				const std::string a = argv[i] ? argv[i] : "";
				if (!cmd.empty()) cmd += ' ';
				cmd += a;
				// Kept, not just logged: this is where "open with" arrives on
				// Windows and Linux. Options are not documents, so anything
				// starting with a dash stays out of the list.
				if (!a.empty() && a[0] != '-') m_launchArgs.push_back(a);
			}
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

		// The splash goes up before anything else can take a second: on this
		// machine the Metal renderer alone needs ~1.2 s (a Debug build ~80 s),
		// and until now that time was spent staring at an empty window.
		m_splash.open(cfg.splash);
		m_splash.setStatus("Opening window", 0.05f);

		WindowProps wp = cfg.windowprops;
		wp.api = cfg.backend;
		// Hold the primary window back while the splash is up. Nothing draws
		// into it until the first frame anyway, and the renderer initialises
		// against a hidden window just as well as a visible one.
		wp.startHidden = wp.startHidden || m_splash.isOpen();
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
				if (m_secondaryWindows.count(e.window.windowID))
				{
					// Through the same door destroyWindow uses, so the host is
					// told exactly once however the window came to close.
					destroyWindow(WindowHandle{ e.window.windowID });
					return;
				}
			}
			// Mouse MOVEMENT is accumulated unconditionally, ahead of the
			// consume gate below. It is raw device data, and deciding who may
			// act on it is the consumer's job — the game always, the editor only
			// while play mode holds the mouse. Running it through the gate would
			// make ImGui's "I want the mouse" the arbiter of whether a captured,
			// cursor-hidden play session gets to look around, which it is not.
			m_input.ProcessMouseEvent(e);
			// Gamepad hot-plug rides the same ungated path as mouse motion:
			// ImGui never competes for pads (NavEnableGamepad stays off), so
			// there is nothing a consume gate could arbitrate.
			m_input.ProcessGamepadEvent(e);
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

		m_splash.setStatus("Initialising renderer", 0.15f);
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
			// Before the message box, not after: the splash is always-on-top and
			// would sit squarely over the one dialog that explains why there is
			// no editor. RAII would take it down too late.
			closeSplash();
			HE_LOG_CRIT(Core, "%s", e.what());
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Renderer Init Failed", e.what(), nullptr);
			return 1;
		}

		m_splash.setStatus("Starting up", 0.35f);
		OnInit();
		HE_LOG_INFO(Core, "%s", "OnInit complete — entering main loop");

		m_splash.setStatus("Ready", 1.0f);
		closeSplash();
		if (wp.startHidden) m_window->Show();

		m_running = true;
		m_vsyncEnabled = cfg.windowprops.vsync;
		m_savedVsync   = m_vsyncEnabled;
		Uint64 lastTick = SDL_GetTicksNS();
		EngineProfiler& profiler = EngineProfiler::instance();

		// Last frame that was actually drawn — the reference the event-driven
		// heartbeat measures against (see setEventDriven).
		Uint64 lastDrawTick = SDL_GetTicksNS();

		while (m_running && !m_window->ShouldClose())
		{
			// Event-driven idle: sleep inside SDL until something arrives instead
			// of spinning a full frame to discover that nothing did. The event is
			// left queued, so the PollEvents() below dispatches it as usual. A
			// pending redraw request skips the wait — it IS the something.
			// How long this turn may sleep, and how long the heartbeat is worth
			// for it. ONE number, read twice: sleeping 16 ms and then asking
			// whether 100 ms have passed would wake up and go straight back to
			// sleep, and the timer that asked for the short wait would never
			// get its frame.
			const int waitMs = (m_nextWakeMs >= 0 && m_nextWakeMs < m_idleHeartbeatMs)
			                 ? (m_nextWakeMs > 0 ? m_nextWakeMs : 1)
			                 : m_idleHeartbeatMs;
			if (m_eventDriven && !m_redrawRequested)
			{
				HE_PROFILE_SCOPE_N("IdleWait");
				Window::WaitForEvent(waitMs);
			}

			const Uint64 nowTick = SDL_GetTicksNS();
			const Uint64 delta   = nowTick - lastTick;
			// Two numbers, on purpose. `measuredDt` is how long the last frame
			// actually took — that is what the hitch detector and the profiler are
			// asking about, and clamping it would hide exactly the stalls they
			// exist to report. `dt` is how far the world is advanced, capped at
			// kMaxFrameSeconds so a breakpoint or an alt-tab does not teleport
			// everything on the frame after it. See kMaxFrameSeconds.
			const float  measuredDt = delta > 0 ? static_cast<float>(delta) * 1e-9f
			                                    : (1.0f / 60.0f);
			const float  dt         = std::min(measuredDt, kMaxFrameSeconds);
			lastTick = nowTick;

			// Stamp every log record produced this frame with the frame index, so a
			// message can be lined up against a profiler capture or a video.
			HE::Log::setFrameNumber(++m_frameIndex);

			// ── HE_EXIT_AFTER_FRAMES: boot, draw, leave ─────────────────────
			// The one thing no unit test in this repository covers is STARTING:
			// the window, the renderer, the pak, the GameInstance's OnInit. A
			// null pointer there is a crash before anything a test can assert
			// on, and it stayed hidden until somebody launched the app by hand —
			// which is exactly how it was found, once.
			//
			// With this set, the application runs that many frames and then asks
			// to quit, so "does it boot" becomes an exit code a test can read.
			// Read once and cached: getenv per frame is a syscall for a value
			// that cannot change.
			{
				static const unsigned long long kExitAfter = []() -> unsigned long long
				{
					const char* v = std::getenv("HE_EXIT_AFTER_FRAMES");
					return (v && *v) ? std::strtoull(v, nullptr, 10) : 0ull;
				}();
				if (kExitAfter != 0 && m_frameIndex >= kExitAfter)
				{
					HE_LOG_INFO(Core, "HE_EXIT_AFTER_FRAMES=%llu reached — leaving cleanly",
					            kExitAfter);
					m_running = false;
				}
			}

			// ── HE_CAPTURE_FRAME / HE_CAPTURE_PATH: what it actually drew ────
			// The companion to the frame budget above. "Does it start" is an exit
			// code; "does it LOOK right" is not, and a shipped application has no
			// editor to take a screenshot from. One frame, written as a plain
			// PPM (three lines of code, no encoder to link) for a human or a
			// script to look at.
			//
			// The capture happens BEFORE this frame is drawn, so it holds the
			// frame before it — which is why the default is well past the first.
			{
				static const unsigned long long kCaptureAt = []() -> unsigned long long
				{
					const char* v = std::getenv("HE_CAPTURE_FRAME");
					return (v && *v) ? std::strtoull(v, nullptr, 10) : 0ull;
				}();
				// CaptureViewport reads the OFFSCREEN target — the one the editor
				// shows in its viewport pane. A game renders straight to the
				// window and never allocates it, so a capture run has to ask for
				// it, once, before the frame it wants.
				if (kCaptureAt != 0 && m_frameIndex == 1 && m_renderer && m_window)
					m_renderer->SetViewportSize(m_window->GetWidth(), m_window->GetHeight());
				if (kCaptureAt != 0 && m_frameIndex == kCaptureAt && m_renderer)
				{
					std::vector<uint8_t> rgba;
					uint32_t cw = 0, ch = 0;
					const char* path = std::getenv("HE_CAPTURE_PATH");
					const std::string out = (path && *path) ? path : "capture.ppm";
					if (m_renderer->CaptureViewport(rgba, cw, ch) && cw > 0 && ch > 0)
					{
						if (std::FILE* f = std::fopen(out.c_str(), "wb"))
						{
							std::fprintf(f, "P6\n%u %u\n255\n", cw, ch);
							for (std::size_t i = 0; i + 3 < rgba.size(); i += 4)
								std::fwrite(&rgba[i], 1, 3, f);
							std::fclose(f);
							HE_LOG_INFO(Core, "Captured frame %llu (%ux%u) to %s",
							            kCaptureAt, cw, ch, out.c_str());
						}
						else
							HE_LOG_ERROR(Core, "Could not write the capture to %s", out.c_str());
					}
					else
						HE_LOG_ERROR(Core, "%s", "The renderer produced no frame to capture");
				}
			}

			// Hitch detector. A frame this long is always worth knowing about — it is
			// usually a synchronous asset load, a shader compile or a GC-like stall in
			// a script. Throttled so a systematically slow scene logs once a second
			// instead of every frame.
			if (measuredDt > kHitchSeconds && m_frameIndex > kHitchWarmupFrames)
				HE_LOG_THROTTLE(Core, Warning, 1.0,
				                "Frame hitch: %.1f ms (frame %llu)", measuredDt * 1000.0f,
				                static_cast<unsigned long long>(m_frameIndex));

			// Applies any pending start/stop (from F9) on the frame boundary so a
			// frame is always recorded whole or not at all.
			profiler.beginFrame(static_cast<double>(measuredDt) * 1000.0);

			{
				HE_PROFILE_SCOPE_N("PollEvents");
				m_window->PollEvents();
			}
			if (m_window->ShouldClose()) break;

			// Snapshot pad state right after event polling so hot-plug from
			// this batch is reflected and every consumer in the frame sees the
			// same stick/button values.
			m_input.PollGamepads();

			// Does this frame get drawn at all? A game always draws. An
			// event-driven application draws when the OS gave it something, when
			// something asked for a redraw, or when the heartbeat expired — the
			// last one being the safety net under everything that changes the
			// screen without announcing it.
			const bool heartbeatDue =
				(nowTick - lastDrawTick) >= static_cast<Uint64>(waitMs) * 1000000ull;
			// Two decisions, not one. RUNNING a frame is cheap and has to happen
			// on the heartbeat regardless, or the clock stops: a script's Delay,
			// a timer, an animation all live in OnRender and would never fire.
			// PRESENTING it is the expensive half, and is settled after OnRender
			// by WantsPresent() — so a heartbeat that finds nothing changed ticks
			// the app forward without touching the GPU at all.
			const bool inputHappened = m_window->EventsLastPoll() > 0 || m_redrawRequested;
			const bool runThisFrame  = !m_eventDriven || inputHappened || heartbeatDue;
			m_redrawRequested = false;
			if (!runThisFrame)
			{
				// Nothing happened. No OnRender, no Render, no swap — and the
				// input frame still has to end, or the next real frame would
				// read a stale mouse delta.
				m_input.EndFrame();
				HE_PROFILE_FRAME();
				// The short wake is deliberately NOT cleared here. Nothing ran,
				// so nobody had the chance to ask for it again, and clearing it
				// would drop the very frame it was asked for.
				continue;
			}
			// Consumed: OnRender is about to run and says again if it still
			// wants a short wait (see askWakeWithinMs).
			m_nextWakeMs = -1;

			// Settled inside the try (after OnRender) and read again by the swap
			// below, which sits outside it. True by default so a frame that threw
			// on its way through still ends cleanly the way it always did.
			bool present = true;
			try
				{
					// OnRender first: builds ImGui frame and calls ImGui::Render()
					// so GetDrawData() is valid when the renderer's overlay callback fires.
					{
						HE_PROFILE_SCOPE_N("OnRender");
						OnRender(dt);
					}
					// Now that the frame's logic has run, ask whether it is worth
					// showing. Always yes outside event-driven mode; inside it,
					// yes when input arrived and otherwise only when the app says
					// something changed. WantsPresent() is CONSUMING, so it is
					// called exactly once per frame and never inside a short-circuit.
					const bool appChanged = WantsPresent();
					present = !m_eventDriven || inputHappened || appChanged;
					if (present) lastDrawTick = nowTick;
					if (m_renderer && present)
					{
						HE_PROFILE_SCOPE_N("Render");
						m_renderer->Render();
					}

					// Secondary windows, on the SAME decision as the primary.
					// In event-driven mode a frame that is not worth showing is
					// not worth showing in a tool window either, and redrawing
					// them anyway would keep a sleeping application busy — which
					// is the one thing event-driven drawing exists to avoid.
					if (present)
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

			// The mouse movement belonged to the frame just drawn. Cleared here
			// rather than on read, so every reader in a frame sees the same
			// numbers instead of the first one draining it for the rest.
			m_input.EndFrame();

			if (m_world)
			{
				HE_PROFILE_SCOPE_N("GameLogicTick");
				// GAME time, not the raw frame time: C++ game logic obeys the
				// same pause and slow motion as scripts, physics and the ECS
				// systems do. Ticking it on the wall clock is how a paused game
				// kept moving in exactly the one language that could not see it.
				m_loop.tick(*m_world, m_logicLoader.logic(), GameLogicDeltaTime(dt));
			}

			// No swap for a frame that was only ticked: presenting an unchanged
			// image still costs a full buffer flip and, with VSync on, blocks
			// until the next refresh — which is exactly the cost this is here to
			// avoid.
			if (present)
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
				lf.deltaMs    = static_cast<double>(measuredDt) * 1000.0;
				lf.cpuFrameMs = profiler.lastCpuFrameMs();
				if (gsPulled)
				{
					lf.gpuFrameMs = gs.gpuFrameMs;
					lf.draws      = gs.drawCalls;
					lf.triangles  = gs.triangles;
					lf.visible    = gs.visibleObjects;
					lf.total      = gs.totalObjects;
				}
				// Scene-side counters come from the world tick earlier in this
				// frame (SceneSystems::pushProfilerSceneCounters); the frame loop
				// cannot read them itself — HE_Core has no view of the registry.
				const ProfSceneCounters& sc = profiler.sceneCounters();
				lf.entities          = sc.entities;
				lf.lights            = sc.lights;
				lf.particles         = sc.particles;
				lf.streamingInFlight = sc.streamingInFlight;
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
		// Detach and destroy secondary windows first — each through the same
		// door a user's click takes, so the host gets its OnWindowClosing for
		// every one of them and nothing is torn down behind its back. Ids
		// snapshotted: destroyWindow erases from the map it would iterate.
		{
			std::vector<uint32_t> ids;
			ids.reserve(m_secondaryWindows.size());
			for (const auto& [id, win] : m_secondaryWindows) ids.push_back(id);
			for (const uint32_t id : ids) destroyWindow(WindowHandle{ id });
		}
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
        if (m_renderer && !m_renderer->GetCapabilities().supportsSecondaryWindows)
        {
            // Refused in ONE place rather than four backends each inventing an
            // excuse: GL clears the window black, Vulkan would redraw the whole
            // scene into it, D3D has no path at all. A window that opens onto
            // any of those is worse than one that does not open.
            HE_LOG_WARN(Core, "%s",
                "createSecondaryWindow: this renderer has no second-window path "
                "(Software and Metal do) — no window opened");
            return {};
        }
        WindowProps sp = props;
        // The graphics API is NOT the caller's to choose. The SDL flags follow
        // from it and cannot be changed afterwards, and one renderer draws into
        // both windows: a secondary created for the default (OpenGL) under a
        // software renderer has a GL flag and no SDL surface to blit into, which
        // fails inside the backend with nothing pointing back at here.
        sp.api = m_window->GetApi();
        auto win = std::make_unique<Window>(sp, /*isPrimary=*/false);
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
        // The host first, while the window is still there: it destroys the
        // widgets that hang in it and fires OnWindowClosed at the graph. A
        // handler that closes the window AGAIN would recurse, so the entry is
        // taken out of the map before the call and destroyed after it.
        auto win = std::move(it->second);
        m_secondaryWindows.erase(it);
        OnWindowClosing(handle);
        if (m_renderer) m_renderer->DetachWindow(win.get());
        win.reset();
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

    void Application::setWindowMinimized()
    {
        if (m_window) m_window->Minimize();
    }

    void Application::setWindowMaximized(bool maximized)
    {
        if (!m_window) return;
        if (maximized) m_window->Maximize();
        else           m_window->Restore();
    }

    bool Application::windowMaximized() const
    {
        return m_window && m_window->IsMaximized();
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
