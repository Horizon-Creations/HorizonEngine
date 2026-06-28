#pragma once
#include "Types/Defines.h"
#include "Application/GameLoop.h"
#include "Application/GameLogicLoader.h"
#include "Application/Input.h"
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
		HE::GraphicsAPI backend = HE::GraphicsAPI::OpenGL;

		HE::WindowProps windowprops;

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

		// Called once after the loop exits, before the window is destroyed.
		virtual void OnShutdown() {}

		// Override to supply a concrete renderer. Called once before OnInit().
		// Link against HorizonRendering and use RendererFactory::Create() here.
		virtual std::unique_ptr<IRenderer> CreateRenderer() { return nullptr; }

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

	private:
		bool                       m_running  = false;
		bool                       m_vsyncEnabled = true;  // current vsync state
		bool                       m_savedVsync   = true;  // vsync to restore after a capture
		float                      m_maxFps       = 0.0f;  // VSync-off frame cap (0 = unlimited)
		std::unique_ptr<Window>    m_window;
		std::unique_ptr<IRenderer> m_renderer;
		Input                      m_input;
		GameLoop                   m_loop;
		GameLogicLoader            m_logicLoader;
		HorizonWorld*              m_world    = nullptr;
		// Secondary windows keyed by their SDL window ID
		std::unordered_map<uint32_t, std::unique_ptr<Window>> m_secondaryWindows;
		ContentManager			   m_contentManager;
	};
}
