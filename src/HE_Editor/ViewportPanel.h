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
	bool groundGridEnabled();
	void setGroundGridEnabled(bool on);
}
