#pragma once

struct AppContext;
struct SDL_Window;

// ── Secondary scene viewports ────────────────────────────────────────────────
// "Scene 2", "Scene 3", "Scene 4": three more dockable windows that look at
// the SAME level from cameras of their own — a Top, a Front and a Right view
// beside the perspective one, the four-pane layout every level editor grows
// into once a floor plan has to be lined up while the perspective view stays
// where it was. Each has its own EditorCamera (presets, orthographic, the
// keypad views, the bookmarks, F), navigated through the Scene window's
// gesture grammar (EditorViewportNav), and a small strip on top for the
// view picker and the grid.
//
// What they are NOT: a second copy of the Scene window. The main renderer has
// exactly one scene target and one frame's worth of temporal state (TAA
// history, GI, occlusion, the G-buffer), and drawing the level N times a
// frame through it would mean duplicating all of that per pane. These panes
// go through IRenderer::RenderWorldPreview instead — the forward pass the
// class editor and the mesh viewer already draw with: base colour, the sky
// and a sun (or a studio headlight in an axis view), a grid. No material
// graphs, no shadows, no post, no view modes, no picking or gizmo. They are
// there to SEE the level from another side while editing in the Scene window,
// not to edit in. The one overlay they draw themselves is the selection's
// box, projected with the pane's own matrix, so what the Scene window has
// selected is findable from above.
//
// Off by default; Window ▸ Scene 2 / 3 / 4 opens one, and a pane the user
// docked comes back with the layout (EditorUI's panel prefs).
namespace SecondaryViewportPanel
{
	constexpr int kCount = 3;

	// "Scene 2" … "Scene 4": the window title, also the dock/config identity.
	const char* title(int index);

	// Open/closed, the flag Window ▸ Scene N toggles and the window's X clears.
	bool& open(int index);

	// Draw every open pane. Call from the scene tab, after the Scene window —
	// these are part of the scene layout, not overlays that float over an
	// asset tab.
	void render(AppContext& ctx, float dt);

	// Drop any fly-look capture one of these panes is holding (the tab-switch
	// safety release, see ViewportPanel::releaseViewportLookCapture).
	void releaseLookCaptures(SDL_Window* win);
}
