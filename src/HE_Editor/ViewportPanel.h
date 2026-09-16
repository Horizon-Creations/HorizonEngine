#pragma once

struct AppContext;
struct SDL_Window;
class  DebugDrawBuffer;
class  EditorCamera;

// ── Scene viewport ───────────────────────────────────────────────────────────
// The centre dock window: the renderer's offscreen target as an ImGui image,
// the inline toolbar (mode / gizmo / play), editor-camera navigation with the
// RMB fly-look capture, the scene extract, drag-drop spawning, the ImGuizmo
// manipulation, mouse picking and the Landscape brush hand-off to TerrainTools.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace ViewportPanel
{
	void render(AppContext& ctx, float dt);

	// Last viewport RENDER resolution in framebuffer pixels (HiDPI-aware),
	// captured while the panel is drawn and shown in the footer beside the FPS
	// counter — hence readable from outside.
	void renderSizePx(int& outW, int& outH);

	// Drop THIS viewport's fly-look capture: warp the cursor back to where the look-drag
	// began, leave relative mode, re-show the OS cursor, and hand mouse control back to
	// ImGui. Safe to call every frame — a no-op unless this viewport is actually holding
	// a capture. Public because paths that DON'T draw the viewport have to force-release
	// it (e.g. switching to a material/script tab mid-look via a keyboard shortcut).
	//
	// Scoped to the Scene viewport on purpose: an asset tab's viewport navigates through
	// the same module now and can be mid-look while this is being called every frame.
	void releaseViewportLookCapture(SDL_Window* win);

	// Belt-and-suspenders invariant, run once per frame BEFORE any early-out: fly-look
	// capture must never outlive a physically-held right mouse button.
	void enforceViewportLookCaptureInvariant(SDL_Window* win);

	// ── Ground grid ──────────────────────────────────────────────────────────
	// The scene view's scale reference: an empty scene otherwise has no origin,
	// no unit and no horizon, so "is this cube one metre or fifty" has no answer
	// on screen. Emitted as debug LINES — the channel the collider and NavMesh
	// overlays already use — so the backend depth-tests it and geometry standing
	// on the ground occludes it per pixel instead of being drawn over.
	//
	// APPENDED to the editor's per-frame buffer rather than pushed to the
	// renderer directly: IRenderer::SetDebugLines replaces the whole list, so a
	// second push would drop the colliders and the collaboration markers. Call it
	// once per frame while filling that buffer. A no-op while `playing` (the game
	// camera is on screen then, and a grid is editor furniture) and while the
	// grid is switched off.
	void appendGroundGrid(const EditorCamera& cam, bool playing, DebugDrawBuffer& out);

	// Grid visibility. On by default; the pair exists so the viewport toolbar's
	// show-flag toggles ONE piece of state instead of keeping a second copy.
	// Today a view onto ShowFlags::groundGrid below — kept because the config
	// key and the callers predate the other flags.
	bool groundGridEnabled();
	void setGroundGridEnabled(bool on);

	// ── Show flags ───────────────────────────────────────────────────────────
	// What the editor draws OVER the scene, one switch per overlay — the strip
	// of checkboxes behind the toolbar's Show cell. Every one of them is editor
	// furniture: the game never draws any of it, so none of this is scene data
	// and none of it goes into the file. Persisted with the editor config
	// (EditorApplication, through `showFlagFields`), because "I switched the
	// colliders off" is a preference and not a per-scene decision.
	//
	// Read where the overlay is BUILT, not where it is drawn: the debug-line
	// block in EditorApplication appends nothing for a switched-off overlay,
	// the extractor emits no icon quads, the name tags are not projected. What
	// is off costs nothing, and the picker cannot hit an icon that is not there.
	struct ShowFlags
	{
		bool groundGrid    = true;  // the reference grid on the ground plane
		bool selection     = true;  // the amber box on each selected entity
		bool colliders     = true;  // collider wireframes (cyan / magenta)
		bool joints        = true;  // joint lines, anchors and hinge arcs
		bool navMesh       = true;  // baked NavMesh polygons (per component too)
		bool editorIcons   = true;  // light / camera / audio-source billboards
		bool guides        = true;  // rope + trail handles, root-motion and look-at previews
		bool collaborators = true;  // peers' rings, selections and name tags
		bool scriptDebug   = true;  // debug.* lines from scripts and HorizonCode
	};
	ShowFlags& showFlags();

	// The flags as a table: config key + member, so the editor config
	// round-trip is a loop rather than one hand-written pair per flag that a
	// new flag can forget. The ground grid keeps its historical key.
	struct ShowFlagField { const char* configKey; bool ShowFlags::* member; };
	const ShowFlagField* showFlagFields(int& outCount);
}
