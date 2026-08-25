#pragma once

// ── Scene-viewport toolbar ───────────────────────────────────────────────────
// The strip along the top edge of the Scene window. It used to be a row of
// default ImGui controls submitted left to right — a combo, three text buttons,
// another combo, a checkbox, and a Play button nudged towards the middle with a
// hand-tuned offset that only centred it at one particular panel width. Nothing
// grouped the controls that belong together, and the one control the eye looks
// for first (Play) was the least prominent thing in the row.
//
// This is that bar as three zones, drawn by hand:
//
//   [ mode | move rotate scale | local/world | snap ]   ( ▶ ⏸ ▶| )   [ speed | ⚙ ]
//    ← what the mouse does in the viewport                           ← how it looks
//                                         ↑ what the editor is doing
//
// Related controls sit in one rounded "well" and are separated from the next
// group by a gap, so the row parses as four things instead of eight. The
// transport — Play/Stop, Pause, Step — is genuinely centred (measured, not
// guessed) and tinted, and play mode also colours the bar's bottom edge: red
// while the scene runs, amber while it is frozen. From the corner of the eye it
// is obvious whether the scene is live. Everything is icon-first with a tooltip
// carrying the keyboard shortcut.
//
// Pause and Step keep their place while the scene is stopped, dimmed rather than
// dropped, so the press that happens most often never moves the row.
//
// The bar shrinks in defined steps rather than overflowing: first the text
// labels go, then the right-hand zone, then the snap group. Whatever gets
// dropped stays reachable in the ⚙ options popup, which is never dropped.
//
// State lives with the caller (ViewportPanel), which also drives the gizmo from
// it — the bar only edits it.

struct AppContext;

#ifdef HE_IMGUI_ENABLED

#include <imgui.h>     // ImGuizmo.h uses ImVec2/ImU32/ImDrawList without declaring them
#include <ImGuizmo.h>

namespace ViewportToolbar
{

// What the bar edits and the gizmo consumes.
struct State
{
	ImGuizmo::OPERATION op   = ImGuizmo::TRANSLATE;  // Move / Rotate / Scale (W/E/R)
	ImGuizmo::MODE      mode = ImGuizmo::LOCAL;      // gizmo axes: object or world

	// ImGuizmo's outer screen-space rotation ring (rotate about the view axis).
	// Off by default — its viewport-relative behaviour is confusing.
	bool  rotateScreenRing = false;

	// Snapping. One increment per operation, because "1" means a metre, a
	// degree and a factor in the three cases and sharing them is useless.
	bool  snapEnabled   = false;
	float snapTranslate = 1.0f;    // world units
	float snapRotate    = 15.0f;   // degrees
	float snapScale     = 0.25f;   // factor

	// Snap triple for ImGuizmo::Manipulate matching `op`, or nullptr while
	// snapping is off. ImGuizmo reads three floats for TRANSLATE and one for
	// ROTATE/SCALE, so a single buffer serves all three.
	const float* activeSnap() const;

private:
	mutable float m_snapBuf[3]{};
};

// Height of the strip in ImGui units. Scales with the font, so the bar keeps
// its proportions under Preferences ▸ UI Font Scale.
float height();

// Draws the bar at the current cursor position (expects the Scene window's
// zero-padding content origin) and leaves the cursor on the first row below it,
// ready for the viewport image.
void render(AppContext& ctx, State& st);

} // namespace ViewportToolbar

#endif // HE_IMGUI_ENABLED
