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
class  EditorCamera;

#ifdef HE_IMGUI_ENABLED

#include <imgui.h>     // ImGuizmo.h uses ImVec2/ImU32/ImDrawList without declaring them
#include <ImGuizmo.h>
#include "Renderer/IRenderer.h"   // HE::ViewMode
#include <Types/UUID.h>           // the camera Look Through is locked to

namespace ViewportToolbar
{

// What the bar edits and the gizmo consumes.
struct State
{
	ImGuizmo::OPERATION op   = ImGuizmo::TRANSLATE;  // Move / Rotate / Scale (W/E/R)
	ImGuizmo::MODE      mode = ImGuizmo::LOCAL;      // gizmo axes: object or world

	// How the scene is drawn (Lit / Unlit / Wireframe / a G-buffer view). The
	// panel pushes it to the renderer every frame (IRenderer::SetViewMode);
	// per session, not persisted — an editor that came up in wireframe would
	// read as broken, and Lit is the one mode everyone wants first.
	HE::ViewMode viewMode = HE::ViewMode::Lit;

	// ImGuizmo's outer screen-space rotation ring (rotate about the view axis).
	// Off by default — its viewport-relative behaviour is confusing.
	bool  rotateScreenRing = false;

	// Snapping. One increment per operation, because "1" means a metre, a
	// degree and a factor in the three cases and sharing them is useless.
	bool  snapEnabled   = false;
	float snapTranslate = 1.0f;    // world units
	float snapRotate    = 15.0f;   // degrees
	float snapScale     = 0.25f;   // factor

	// What a MOVE snaps to. Grid is the increment above; Surface puts the
	// pivot on whatever scene surface lies under it as it is dragged (the
	// selection itself excluded), Vertex on the nearest vertex of another
	// mesh within `snapVertexRadiusPx` of it on screen — and falls back to a
	// free move when none is that close. Rotate and Scale keep their grid
	// increments in every mode: a surface has no angle to snap to.
	enum class SnapMode { Grid = 0, Surface = 1, Vertex = 2 };
	SnapMode snapMode = SnapMode::Grid;
	// Surface mode: lift the object so its bottom rests on the surface (the
	// pivot's height above its own bounds), rather than sinking the pivot
	// into it. On by default — a pivot at the centre is the common export.
	bool  snapSurfaceRest    = true;
	float snapVertexRadiusPx = 24.0f;

	// View ▸ Look Through Selected Camera: the scene camera the viewport
	// renders through, or a zero id. The ENTITY, not the selection — the point
	// is to select and move actors while the shot stays on screen. Held as its
	// EntityIdComponent uuid, not an entt handle: undo and a scene reload remap
	// handles, and a handle would then name nothing or a different camera. The
	// panel drops it when the camera goes, when play starts, and on the first
	// navigation (which then continues from the camera's pose). Per session.
	HE::UUID lookThrough{};

	// True while a translate drag is taken over by a surface/vertex probe
	// rather than ImGuizmo's own increment.
	bool probeSnapActive() const
	{
		return snapEnabled && snapMode != SnapMode::Grid && op == ImGuizmo::TRANSLATE;
	}

	// Snap triple for ImGuizmo::Manipulate matching `op`, or nullptr while
	// snapping is off. ImGuizmo reads three floats for TRANSLATE and one for
	// ROTATE/SCALE, so a single buffer serves all three. Inline so the gizmo,
	// which is the only reader, links wherever it is compiled — the test
	// binary carries the gizmo but not this bar (it is EditorApplication's).
	const float* activeSnap() const
	{
		if (!snapEnabled || probeSnapActive()) return nullptr;
		const float v = (op == ImGuizmo::ROTATE) ? snapRotate
		              : (op == ImGuizmo::SCALE)  ? snapScale
		                                         : snapTranslate;
		m_snapBuf[0] = m_snapBuf[1] = m_snapBuf[2] = v;
		return m_snapBuf;
	}

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

// The rows of the View cell's popup — the axis presets, the Orthographic
// switch and the camera bookmarks — over any editor camera. Public because
// the secondary viewports open the same picker over their own cameras; call
// it inside an open popup or menu.
void viewPopup(EditorCamera& cam);

// The Scene window's own row under those: Look Through Selected Camera, or Stop
// Looking Through while locked (State::lookThrough). Not part of viewPopup
// because a secondary viewport draws with the preview renderer over a camera
// of its own and has no scene camera to lock to.
void lookThroughRows(AppContext& ctx, State& st);

// The rows of the Show cell's popup (the overlay switches, grouped, with Show
// All / Hide All) and of the View Mode cell's popup (Lit / Unlit / Wireframe,
// the G-buffer views), over the given mode. Public for the same reason as
// viewPopup: the main bar's View menu offers the same switches, and a second
// list is how the two would drift. Call inside an open popup or menu.
void showRows(AppContext& ctx);
void viewModeRows(AppContext& ctx, HE::ViewMode& mode);

} // namespace ViewportToolbar

#endif // HE_IMGUI_ENABLED
