#pragma once
#include <cstdint>
#include "Types/Defines.h"
#include "Application/GameLoop.h"
#include "Application/GameLogicLoader.h"
#include "Application/Input.h"
#include "Application/SplashScreen.h"
#include "Window/Window.h"
#include "Renderer/IRenderer.h"
#include "Renderer/RendererFactory.h"
#include "../Diagnostics/GlobalState.h"
#include "Diagnostics/DiagnosticsStructs.h"
#include "ContentManager/ContentManager.h"
#include <memory>
#include <string>
#include <unordered_map>

class HorizonWorld;

namespace HE
{

	// Fill this in GetConfig() to configure the window before Run() opens it.
	// All fields have sensible defaults so you only need to set what differs.
	struct ApplicationConfig
	{
		HE::RendererBackend backend = HE::RendererBackend::OpenGL;

		HE::WindowProps windowprops;

		// The little branded window that stands in for the app while it starts.
		// Leave `enabled` false (the default is on, but with no title and no
		// logo path it draws an empty panel) to start without one. Paths are
		// supplied by the application, not discovered here — HorizonCore has no
		// business knowing where an editor keeps its artwork.
		HE::SplashConfig splash;

		// GameLoop tuning
		float    fixedTimestep = 1.0f / 60.0f;
		uint32_t maxFixedSteps = 5;


	};

	// Opaque handle returned by createSecondaryWindow().
	// Pass it back to destroyWindow() / getWindow() / getWindowRenderer().
	struct WindowHandle
	{
		uint32_t id = 0;          // SDL window ID
		bool     isValid() const { return id != 0; }
	};

	class HE_API Application
	{
	public:
		Application(std::string startupPath);
		virtual ~Application();

		int Run(int argc = 0, char** argv = nullptr);
		void Quit();

	protected:
		// ── Subclass hooks ─────────────────────────────────────────────────
		// Override to provide startup configuration. Called once before the
		// window is created.
		virtual ApplicationConfig GetConfig() const { return {}; }

		// Called once after the window is open, before the main loop starts.
		virtual void OnInit()     {}

		// Called for every SDL_Event before it reaches the Input system.
		// Return true to consume the event (stops Input from processing it).
		virtual bool OnEvent(const SDL_Event& event) { (void)event; return false; }

		// Called every frame between PollEvents() and SwapBuffers().
		virtual void OnRender(float deltaTime) { (void)deltaTime; }

		// How much GAME time the frame just rendered was worth, for the
		// fixed-step tick that drives the C++ game-logic module (IGameLogic).
		//
		// It is a hook rather than a straight read of the engine clock because
		// that clock lives in HorizonScene, which sits ABOVE this layer — and
		// because the two applications answer differently: the packaged game
		// hands back the scaled delta, the editor hands back zero unless play
		// mode is running, so C++ game logic no longer ticks in edit mode. The
		// raw dt stays the loop's own pacing; only what the accumulator is fed
		// with moves. Called after OnRender(), so the clock is already advanced.
		virtual float GameLogicDeltaTime(float rawDeltaTime) { return rawDeltaTime; }

		// Called once after the loop exits, before the window is destroyed.
		virtual void OnShutdown() {}

		// Override to supply a concrete renderer. Called once before OnInit().
		// Link against HorizonRendering and use RendererFactory::Create() here.
		virtual std::unique_ptr<IRenderer> CreateRenderer() { return nullptr; }

		// ── Startup splash ─────────────────────────────────────────────────
		// Name the step OnInit is about to run, for the splash window. Free
		// after the splash closes (and when there never was one), so callers do
		// not have to guard it. `progress` is 0..1; negative leaves the bar.
		//
		// The steps Run() owns — window, renderer, "starting" — are reported
		// here; a subclass only has to describe its own OnInit. Keep the text
		// short and in the present participle ("Loading project"): it is a
		// caption, not a log line.
		void splashStatus(const std::string& text, float progress = -1.0f)
		{
			m_splash.setStatus(text, progress);
		}

		// ── Engine systems exposed to subclasses ───────────────────────────
		Window*          window()       const { return m_window.get(); }
		GameLoop&        gameLoop()           { return m_loop; }
		GameLogicLoader& logicLoader()        { return m_logicLoader; }
		IRenderer*       renderer()     const { return m_renderer.get(); }
		Input&           input()              { return m_input; }
		ContentManager&  contentManager()     { return m_contentManager; }
		GlobalState* m_globalState;

		HorizonWorld* world() const                 { return m_world; }
		void          setWorld(HorizonWorld* world)
		{
			m_world = world;
			if (m_renderer) m_renderer->SetWorld(world);
		}

		bool isRunning() const { return m_running; }

		// ── Runtime window changes ─────────────────────────────────────────
		void setWindowTitle(const std::string& title);
		void setWindowSize(uint32_t width, uint32_t height);
		void setVSync(bool enabled);
		void setWindowMode(WindowMode mode);
		// Optional frame-rate ceiling applied only when VSync is OFF (0 = unlimited).
		// Lets the loop be paced without VSync (e.g. to smooth the editor's mouse-look or
		// cut needless GPU load) while defaulting to fully uncapped.
		void  setMaxFps(float fps) { m_maxFps = fps > 0.0f ? fps : 0.0f; }
		float maxFps() const       { return m_maxFps; }

		// ── Multi-window API ──────────────────────────────────────────────
		// Open a new secondary window.  The renderer's AttachWindow() is called
		// automatically.  Returns an invalid handle if Run() has not been called.
		WindowHandle createSecondaryWindow(const WindowProps& props);

		// Close and destroy a secondary window (and detach from renderer).
		void destroyWindow(WindowHandle handle);

		// Look up a secondary window by its handle (returns nullptr if not found).
		Window* getWindow(WindowHandle handle) const;

		// Toggle a profiler benchmark capture (bound to F9). On start it disables
		// vsync (so frame times reflect true cost, not the refresh rate) and on
		// stop it restores the previous vsync state and writes a dump.
		void toggleProfilerCapture();

	protected:
		// Frames completed by the main loop. Stamped onto every log record (see
		// HE::Log::setFrameNumber) so a message can be tied to a specific frame.
		uint64_t                   m_frameIndex = 0;
		// A frame slower than this is reported as a hitch. The first frames are
		// exempt: startup legitimately blocks on shader/asset warmup.
		static constexpr float     kHitchSeconds      = 0.25f;
		static constexpr uint64_t  kHitchWarmupFrames = 10;

	private:
		// Takes the splash down and puts the engine's own GL context back —
		// destroying the splash's 2D renderer can leave no context current.
		void closeSplash();

		bool                       m_running  = false;
		bool                       m_vsyncEnabled = true;  // current vsync state
		bool                       m_savedVsync   = true;  // vsync to restore after a capture
		float                      m_maxFps       = 0.0f;  // VSync-off frame cap (0 = unlimited)
		std::unique_ptr<Window>    m_window;
		std::unique_ptr<IRenderer> m_renderer;
		// Opened at the very top of Run(), closed before the main loop. A
		// closed splash swallows setStatus, so splashStatus() stays callable.
		SplashScreen               m_splash;
		Input                      m_input;
		GameLoop                   m_loop;
		GameLogicLoader            m_logicLoader;
		HorizonWorld*              m_world    = nullptr;
		// Secondary windows keyed by their SDL window ID
		std::unordered_map<uint32_t, std::unique_ptr<Window>> m_secondaryWindows;
		ContentManager			   m_contentManager;
	};
}
