#pragma once

struct AppContext;
struct SDL_Window;

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

	// Drop any active fly-look capture: warp the cursor back to where the look-drag began,
	// leave relative mode, re-show the OS cursor, and hand mouse control back to ImGui.
	// Safe to call every frame — a no-op unless a capture is actually active.
	// Public because paths that DON'T draw the viewport have to force-release it
	// (e.g. switching to a material/script tab mid-look via a keyboard shortcut).
	void releaseViewportLookCapture(SDL_Window* win);

	// Belt-and-suspenders invariant, run once per frame BEFORE any early-out: fly-look
	// capture must never outlive a physically-held right mouse button.
	void enforceViewportLookCaptureInvariant(SDL_Window* win);
}
