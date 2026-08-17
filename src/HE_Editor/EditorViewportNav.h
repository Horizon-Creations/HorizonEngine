#pragma once
#include "EditorCamera.h"

struct AppContext;
struct SDL_Window;

// ── Scene-navigation input for a 3D viewport ────────────────────────────────
// One frame of "how does the user want to move the camera", read from live
// ImGui + SDL state: Alt+LMB orbit, MMB pan, RMB fly-look, WASDQE/Shift while
// flying, wheel dolly — plus the trackpad flavour, where a two-finger tap
// TOGGLES fly mode because holding a right-click through a whole flight means
// keeping two fingers pressed the entire time.
//
// This used to live inside ViewportPanel's Scene window. It is a module of its
// own because the class editor's viewport has to navigate IDENTICALLY: a second
// implementation would start out the same and drift on the first fix to either
// one — and every rule in here (relative-mouse capture, the NoMouse flags, the
// nav latch, the physical-button edge) was found the hard way, so drifting means
// re-finding them.
//
// The capture state is deliberately global rather than per viewport: relative-
// mouse mode is a property of the WINDOW, only one viewport can hold the cursor
// at a time, and the release paths already sit above panel level (a tab switch
// mid-look must be able to let go).
namespace EditorViewportNav
{
	// Read this frame's navigation input for the viewport image just submitted.
	// Call directly after the image item, passing its IsItemHovered().
	//
	// `owner` identifies the calling viewport — any stable address will do. It
	// is remembered when a fly-look capture engages, so a viewport that is not
	// drawn this frame can drop ITS capture without stealing another's.
	// `viewportHeight` is the image's logical height (pan scales with it).
	// Fills `out` and returns whether a navigation gesture is active — the caller
	// usually wants that to suppress its own click handling (picking, gizmos).
	//
	// Camera-agnostic on purpose: the Scene window and every asset viewport own
	// different EditorCamera instances, and this only describes the intent.
	bool gather(AppContext& ctx, const void* owner, bool imageHovered,
	            float dt, float viewportHeight, EditorCamera::Input& out);

	// Drop a fly-look capture: warp the cursor back to where the look started
	// BEFORE leaving relative mode (SDL applies the warp as the post-relative
	// position, landing it exactly at the press point), restore the cursor and
	// clear the ImGui flags. Safe to call when nothing is captured.
	//
	// Every teardown path goes through here — tab switch, play mode, Esc — so a
	// fly toggle can never outlive its capture.
	void releaseLookCapture(SDL_Window* win);

	// The same, but only if `owner` is the viewport that engaged the capture.
	// This is what a "I am not being drawn this frame, so let go of anything I
	// was holding" guard must use: the unconditional release would end a look
	// that a DIFFERENT viewport is in the middle of — which is precisely what
	// happens every frame an asset tab is open, since the editor drops the scene
	// viewport's capture there.
	void releaseLookCaptureFor(const void* owner, SDL_Window* win);

	// Once per frame, BEFORE any early-out: a held-button fly-look must never
	// outlive a physically-held right mouse button. Recovers from any path that
	// latched the capture without releasing it. Reads the PHYSICAL SDL button,
	// not ImGui's io.MouseDown (which the NoMouse flag zeroes during a real
	// look), so an actual fly-look is never cut short. A toggled trackpad fly
	// holds no button by design and is exempt.
	void enforceCaptureInvariant(SDL_Window* win);
}
