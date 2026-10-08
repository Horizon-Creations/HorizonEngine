#pragma once

struct AppContext;

// Pointer-device grammar — the answer to "is the user on a trackpad?".
//
// Why it matters: the preview panes steer their cameras with held mouse drags
// (LMB/RMB orbit), and on a trackpad a press-and-drag means physically pressing
// the pad the whole time — tiring, and for RMB a two-finger press. On a trackpad
// the comfortable gesture is the two-finger SWIPE, which arrives as wheel input
// (MouseWheel/MouseWheelH). So panes ask trackpadPointer() and, when true, let
// bare scroll steer (orbit/pan) and move zoom behind Cmd/Ctrl+scroll; with a
// mouse the wheel keeps meaning zoom, exactly as before.
//
// The user chooses in Preferences ▸ Viewport: Auto (detected), Mouse, Trackpad.
namespace EditorInput
{
	// EditorConfig::PointerInput values.
	enum : int { kPointerAuto = 0, kPointerMouse = 1, kPointerTrackpad = 2 };

	// Pure resolve, separated from the hardware probe so it is trivially
	// testable: the effective answer for a config choice + detection result.
	inline bool resolveTrackpad(int configPointerInput, bool detectedTrackpad)
	{
		return configPointerInput == kPointerTrackpad ||
		       (configPointerInput == kPointerAuto && detectedTrackpad);
	}

	// Hardware probe (cached): a battery (= laptop, which has a built-in
	// trackpad) or an indirect SDL touch device (= external trackpad on a
	// desktop). Note macOS registers the touch device LAZILY on the first
	// touch event, so the second signal can flip to true mid-session — the
	// probe keeps checking until it has seen one, then sticks.
	bool detectTrackpad();

	// The one call panels make: is the trackpad grammar active?
	bool trackpadPointer(const AppContext& ctx);

	// The same answer for call sites that have no AppContext (the shared
	// GraphEditor canvas). Returns whatever the last trackpadPointer() call
	// resolved — EditorUI refreshes it once per frame, so it is never stale by
	// more than a frame. Header-inline on purpose: GraphEditor.cpp is also
	// compiled into he_tests, which must not inherit EditorInput.cpp's SDL
	// dependency just to read a cached bool (it stays false there).
	namespace detail { inline bool g_trackpadActive = false; }
	inline bool trackpadActive() { return detail::g_trackpadActive; }

	// ── The pinch gesture ────────────────────────────────────────────────────
	// SDL 3.2 delivers no pinch event, and macOS does not turn the trackpad's
	// pinch into Ctrl+scroll the way Windows' precision touchpads do — so on a
	// MacBook the zoom gesture reached nobody. The Mac build listens for the
	// native magnify event itself (EditorInputMac.mm) and parks the sum here;
	// beginFrame() publishes it once per frame so every canvas under the pointer
	// can ask the same question. Zero everywhere else, which keeps the
	// Ctrl/Cmd+scroll route as the zoom gesture there.
	//
	// The value is NSEvent.magnification summed over the frame: positive spreads
	// the fingers (zoom in), a full pinch adds up to roughly ±1.
	namespace detail
	{
		inline float g_pinchPending = 0.0f;   // written by the platform hook
		inline float g_pinchFrame   = 0.0f;   // what this frame's panels read
	}
	inline float pinchDelta() { return detail::g_pinchFrame; }

	// Once per frame, before any panel draws (EditorUI::render). Installs the
	// platform hook on first use and publishes the pending pinch.
	void beginFrame();
	// Platform half: macOS installs the magnify monitor, everyone else nothing.
	void pinchPlatformInstall();
}
