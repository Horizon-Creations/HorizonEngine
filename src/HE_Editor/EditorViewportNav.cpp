#include "EditorViewportNav.h"
#include "EditorApplication.h"   // AppContext
#include "EditorInput.h"         // pointer-device grammar (trackpad tap vs mouse button)
#include <Window/Window.h>
#include <Diagnostics/Logger.h>
#include <SDL3/SDL.h>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace EditorViewportNav
{

#ifdef HE_IMGUI_ENABLED

// RMB fly-look capture state (SDL relative-mouse mode). File-scope, not viewport-
// local: the capture must be force-releasable from paths that DON'T draw a
// viewport — e.g. switching to a material/script tab mid-look via a keyboard
// shortcut. Otherwise relative mode + the ImGui NoMouse flag stay latched and the
// cursor is hidden and pinned with no way out but quitting.
static bool  s_rmbCaptured = false;
static float s_rmbStartX   = 0.f;
static float s_rmbStartY   = 0.f;
// Trackpad fly TOGGLE: on a pad, holding RMB for the whole flight means pressing
// with two fingers the whole time — so in trackpad mode a two-finger TAP
// (a right-click) toggles fly mode on/off instead, and no button is held while
// flying. File-scope for the same reason as s_rmbCaptured, and because the
// capture invariant must know a toggled fly is not a leaked capture.
static bool  s_flyToggle   = false;
// WHICH viewport engaged the capture. There is more than one 3D viewport now
// (the Scene window and the class editor's), and each has a "I am not drawn
// this frame, drop what I was holding" guard. Without an owner those guards
// cancel each other: the editor releases the scene viewport's capture on every
// frame an asset tab is open, which ended a fly-look in the class viewport one
// frame after it began.
static const void* s_captureOwner = nullptr;

void releaseLookCapture(SDL_Window* win)
{
	if (!s_rmbCaptured) return;
	ImGuiIO& io = ImGui::GetIO();
	if (win)
	{
		SDL_WarpMouseInWindow(win, s_rmbStartX, s_rmbStartY);
		SDL_SetWindowRelativeMouseMode(win, false);
	}
	SDL_ShowCursor();
	io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
	io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
	s_rmbCaptured = false;
	// Every teardown path runs through here, so the fly toggle can never outlive
	// its capture (tab switch, play mode, Esc — all of them land here).
	s_flyToggle   = false;
	s_captureOwner = nullptr;
}

void releaseLookCaptureFor(const void* owner, SDL_Window* win)
{
	if (!s_rmbCaptured || s_captureOwner != owner) return;
	releaseLookCapture(win);
}

void enforceCaptureInvariant(SDL_Window* win)
{
	if (!s_rmbCaptured) return;
	// A toggled (trackpad) fly holds NO button by design — the invariant below
	// only guards the held-RMB flavour of the capture.
	if (s_flyToggle) return;
	const bool rmbDown =
		(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0;
	if (!rmbDown) releaseLookCapture(win);
}

bool gather(AppContext& ctx, const void* owner, bool imageHovered,
            float dt, float viewportHeight, EditorCamera::Input& out)
{
	ImGuiIO& io = ImGui::GetIO();
	SDL_Window* sdlWin = ctx.window ? ctx.window->GetNativeWindow() : nullptr;
	const bool trackpad = EditorInput::trackpadPointer(ctx);

	// Trackpad fly toggle: a two-finger TAP (right-click) over the viewport
	// enters fly mode, a second tap — or Esc — leaves it. While flying, look and
	// WASDQE/Shift work with nothing held. The edge is read from the PHYSICAL
	// SDL button, not ImGui: mid-fly the NoMouse flag zeroes ImGui's mouse
	// state, so the exit tap would be invisible to IsMouseClicked.
	const bool physRmb =
		(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0;
	static bool s_prevPhysRmb = false;
	const bool physRmbTap = physRmb && !s_prevPhysRmb;
	s_prevPhysRmb = physRmb;
	if (trackpad)
	{
		if (physRmbTap)
		{
			if (s_flyToggle)          releaseLookCapture(sdlWin);   // clears the toggle too
			else if (imageHovered && !io.KeyAlt) s_flyToggle = true;
		}
		if (s_flyToggle && ImGui::IsKeyPressed(ImGuiKey_Escape))
			releaseLookCapture(sdlWin);
	}

	// In trackpad mode "RMB held" is replaced by the toggle; with a mouse it
	// stays the physically held button, exactly as before.
	const bool rmb    = trackpad ? s_flyToggle
	                             : ImGui::IsMouseDown(ImGuiMouseButton_Right);
	const bool mmb    = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
	const bool altLmb = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
	const bool anyNav = rmb || mmb || altLmb;

	// Latch navigation so a drag keeps going if the cursor leaves the image.
	static bool s_navActive = false;
	if (imageHovered && anyNav) s_navActive = true;
	if (!anyNav)                s_navActive = false;
	const bool navigating = s_navActive;

	// RMB fly-look capture: put the window into relative-mouse mode so we read
	// raw OS motion deltas (event.motion.xrel/yrel) instead of the absolute
	// cursor position. The previous approach sampled the absolute position once
	// per frame and warped the cursor back to the press point, which discarded
	// the sub-pixel remainder every frame. At high frame rates the per-frame
	// movement is tiny, so that cumulative loss (plus OS pointer-acceleration)
	// made looking feel sluggish and frame-rate dependent. Relative mode delivers
	// acceleration-free, frame-rate-independent deltas with no warping and no
	// display-edge collisions.
	//
	// Capture tracks the look predicate EXACTLY (rmb && !altLmb): Alt+LMB is the
	// orbit gesture, which needs a visible cursor and io.MouseDelta, so engaging
	// Alt mid-RMB must drop relative mode — otherwise orbit freezes and the stale
	// accumulator snaps the view when look resumes.
	if (sdlWin)
	{
		// Engage on a FRESH right-press over the viewport (click edge), never on
		// "RMB happens to be down" — otherwise arriving on the Scene tab with a
		// stale/held button state would capture the cursor without the user
		// starting a look here. In trackpad mode the fresh toggle IS the edge.
		const bool rmbClicked = trackpad
			? (s_flyToggle && !s_rmbCaptured)
			: ImGui::IsMouseClicked(ImGuiMouseButton_Right);
		if (rmbClicked && !altLmb && imageHovered && !s_rmbCaptured)
		{
			SDL_GetMouseState(&s_rmbStartX, &s_rmbStartY);
			SDL_SetWindowRelativeMouseMode(sdlWin, true);
			SDL_HideCursor(); // relative mode alone doesn't reliably hide the OS cursor (SDL3/macOS)
			// Discard any relative motion accumulated before capture so the first
			// look frame doesn't jump by a stale delta.
			SDL_GetRelativeMouseState(nullptr, nullptr);
			s_rmbCaptured  = true;
			s_captureOwner = owner;
		}
		else if ((!rmb || altLmb) && s_rmbCaptured)
		{
			releaseLookCapture(sdlWin);
		}
	}

	out = EditorCamera::Input{};
	out.dt             = dt;
	out.viewportHeight = viewportHeight;
	if (navigating)
	{
		out.orbit      = altLmb;
		out.pan        = mmb && !altLmb;
		out.look       = rmb && !altLmb;
		out.mouseDelta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
		if (out.look)
		{
			// Relative mode keeps the OS cursor pinned, so this is the raw frame
			// motion delta — no warp, no absolute-position truncation.
			if (s_rmbCaptured && sdlWin)
			{
				float rx = 0.f, ry = 0.f;
				SDL_GetRelativeMouseState(&rx, &ry);
				out.mouseDelta = glm::vec2(rx, ry);
				io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // don't let ImGui re-show it mid-look
				io.ConfigFlags |= ImGuiConfigFlags_NoMouse;            // block ImGui hover/click while free-looking
				SDL_HideCursor();
				ImGui::SetMouseCursor(ImGuiMouseCursor_None);
			}
			out.fast = io.KeyShift;
			if (ImGui::IsKeyDown(ImGuiKey_D)) out.moveAxis.x += 1.0f;
			if (ImGui::IsKeyDown(ImGuiKey_A)) out.moveAxis.x -= 1.0f;
			if (ImGui::IsKeyDown(ImGuiKey_E)) out.moveAxis.y += 1.0f;
			if (ImGui::IsKeyDown(ImGuiKey_Q)) out.moveAxis.y -= 1.0f;
			if (ImGui::IsKeyDown(ImGuiKey_W)) out.moveAxis.z += 1.0f;
			if (ImGui::IsKeyDown(ImGuiKey_S)) out.moveAxis.z -= 1.0f;
		}
	}
	// Wheel zoom works on hover without holding a button — on every pointer
	// device. (A swipe-orbits variant was tried here and reverted: it took the
	// bare scroll away from the dolly, which reads as "zoom is broken" on a pad.
	// Trackpad navigation is the fly TOGGLE above plus Alt+drag orbit;
	// swipe-pan belongs to the 2D canvases.)
	if (imageHovered) out.wheel = io.MouseWheel;

	// ── Self-diagnostic (throttled ~once/sec, only while it matters) ──
	// The field report is "WASD stopped working after joining a session on
	// Windows", and every link in the chain fails silently: hover, the nav latch,
	// relative-mouse capture, key delivery. Logging the state of each while the
	// user is actually trying makes the next report name the broken link instead
	// of the symptom.
	{
		const bool tryingToNavigate =
			ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
			ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_S);
		static int s_navDiagFrames = 0;
		if (tryingToNavigate && ++s_navDiagFrames >= 60)
		{
			s_navDiagFrames = 0;
			HE_LOG_INFO(Editor,
				"Edit-nav diagnostic: hovered=%d navigating=%d look=%d "
				"captured=%d relMode=%d W=%d rmb=%d wantKbd=%d wantTxt=%d "
				"focusWin=%s",
				static_cast<int>(imageHovered),
				static_cast<int>(navigating),
				static_cast<int>(out.look),
				static_cast<int>(s_rmbCaptured),
				static_cast<int>(sdlWin && SDL_GetWindowRelativeMouseMode(sdlWin)),
				static_cast<int>(ImGui::IsKeyDown(ImGuiKey_W)),
				static_cast<int>(ImGui::IsMouseDown(ImGuiMouseButton_Right)),
				static_cast<int>(io.WantCaptureKeyboard),
				static_cast<int>(io.WantTextInput),
				SDL_GetKeyboardFocus() == sdlWin ? "main" : "OTHER");
		}
		if (!tryingToNavigate) s_navDiagFrames = 0;
	}

	return navigating;
}

#else // !HE_IMGUI_ENABLED

bool gather(AppContext&, const void*, bool, float, float, EditorCamera::Input&) { return false; }
void releaseLookCapture(SDL_Window*) {}
void releaseLookCaptureFor(const void*, SDL_Window*) {}
void enforceCaptureInvariant(SDL_Window*) {}

#endif // HE_IMGUI_ENABLED

} // namespace EditorViewportNav
