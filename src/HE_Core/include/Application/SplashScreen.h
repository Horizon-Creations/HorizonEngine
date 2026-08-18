#pragma once
#include "Types/Defines.h"
#include "Renderer/UIFont.h"
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace HE
{
	// What the splash shows. Everything is optional: with no logo and no text it
	// still draws a branded panel, and with no window at all (headless, no video
	// driver) the whole thing quietly does nothing — see SplashScreen::open.
	struct SplashConfig
	{
		bool        enabled = true;
		std::string title;      // big line under the logo, e.g. "Horizon Engine Editor"
		std::string subtitle;   // small line under that, e.g. 0.3.0 "Aurora"
		// Absolute path to a PNG. Empty (or unreadable) → the panel draws without
		// a logo rather than refusing to open.
		std::string logoPath;
		int         width  = 520;   // logical points; the drawing scales to pixels
		int         height = 268;
	};

	// A small always-on-top window that stands in for the editor while it starts.
	//
	// It exists because the interesting part of startup happens BEFORE there is
	// anything to look at: the primary window is created first and the Metal/GL
	// renderer takes over a second to come up behind it, so the first thing a
	// user saw was a black 1600x900 rectangle with no explanation. This window
	// opens ahead of all of that and reports each step by name.
	//
	// It is deliberately independent of the engine's own renderer — its own SDL
	// window and 2D renderer, its own font atlas — because everything it has to
	// survive (a renderer that takes seconds, a renderer that throws) is exactly
	// what it cannot depend on.
	//
	// Not thread-safe, and not meant to be: it is driven from the main thread
	// between the initialisation steps it reports.
	class HE_API SplashScreen
	{
	public:
		SplashScreen() = default;
		~SplashScreen();

		SplashScreen(const SplashScreen&)            = delete;
		SplashScreen& operator=(const SplashScreen&) = delete;

		// Opens the window and paints the first frame. Never throws and never
		// fails hard: a machine with no video driver, no font or no logo gets
		// less of a splash, or none, and startup continues either way. The
		// return value is for logging, not for branching on.
		bool open(const SplashConfig& cfg);

		bool isOpen() const { return m_window != nullptr; }

		// Name the step that is about to run and repaint. `progress` is 0..1 for
		// the bar; a negative value leaves the bar where it is. Costs a fraction
		// of a millisecond — the only reason not to call it is having nothing to
		// say.
		void setStatus(const std::string& text, float progress = -1.0f);

		// Takes the window down. Idempotent, and called by the destructor. Do it
		// explicitly before anything else wants the screen — an always-on-top
		// window will otherwise sit over a message box.
		void close();

		// True if the splash's 2D renderer created an OpenGL context. The caller
		// has to make its own context current again after close(), because
		// destroying that renderer leaves no context bound.
		bool usedGL() const { return m_usedGL; }

		// A font atlas baked at one pixel size, plus its texture. Two of them:
		// baking at the size a line is actually drawn at is the difference
		// between crisp and smeared, and there are only two sizes on the panel.
		struct Face
		{
			BakedUIFont  baked;
			SDL_Texture* tex = nullptr;
		};

	private:
		// One repaint. Pumps the OS event queue (without draining it — the queue
		// belongs to the engine's input system) so the window keeps rendering
		// and the OS does not mark the process unresponsive.
		void draw();

		bool  makeFace(Face& face, float pixelSize);
		// Draws `s` with its BASELINE at y (pixels), left edge at x, and returns
		// the advance width. A face that failed to bake draws nothing but still
		// measures, so centring never divides by a lie.
		float text(const Face& face, const std::string& s, float x, float y,
		           unsigned char r, unsigned char g, unsigned char b, unsigned char a);
		float textWidth(const Face& face, const std::string& s) const;

		SDL_Window*   m_window   = nullptr;
		SDL_Renderer* m_renderer = nullptr;
		SDL_Texture*  m_logo     = nullptr;
		int           m_logoW    = 0;
		int           m_logoH    = 0;
		Face          m_faceBig;              // the title line
		Face          m_faceSmall;            // subtitle + status

		SplashConfig  m_cfg;
		std::string   m_status;
		float         m_progress = 0.0f;
		float         m_scale    = 1.0f;      // pixels per logical point
		int           m_dumpIndex = 0;        // HE_SPLASH_DUMP frame counter
		bool          m_usedGL   = false;
		bool          m_ownsVideo = false;    // we called SDL_InitSubSystem(VIDEO)
	};
}
