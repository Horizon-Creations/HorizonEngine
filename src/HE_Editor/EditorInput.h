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
}
