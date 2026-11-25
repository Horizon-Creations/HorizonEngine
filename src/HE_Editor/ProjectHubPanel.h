#pragma once
#include "ProjectManager.h"   // ProjectPreset — the enum this file's names index

#include <functional>

struct AppContext;
class IRenderer;

// ── Project Hub ──────────────────────────────────────────────────────────────
// The full-window start screen shown while no project is open: the header bar,
// "Projects" (recent-project cards with the picture each project was closed on,
// plus Browse / Clone) and "New Project" (template tiles + script language +
// target folder), and the async open-project / choose-folder dialogs.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace ProjectHubPanel
{
	void render(AppContext& ctx);

	// The template picker: one tile per template with a drawn picture, games
	// and applications in two groups, `width` wide. Shared with the in-editor
	// File ▸ New Project popup (EditorUI.cpp). Returns true when a tile was
	// clicked this frame.
	bool drawTemplateGrid(int& selectedPreset, float width);

	// The recent-project cards' textures (ProjectThumbnail.h) go back to the
	// renderer here, from OnShutdown while it still exists.
	void releaseTextures(IRenderer* renderer);

	// ── Test hook ────────────────────────────────────────────────────────────
	// Where the card pictures' textures come from. Unset (the editor) it is
	// ctx.renderer->CreateImGuiTexture / DestroyImGuiTexture; the headless shot
	// installs the software rasterizer's registry instead (same seam as
	// TextureViewerPanel::setTextureHooks).
	using TextureUpload  = std::function<void*(const void* rgba8, int width, int height)>;
	using TextureRelease = std::function<void(void*)>;
	void setTextureHooks(TextureUpload upload, TextureRelease release);

	// ── Project templates ────────────────────────────────────────────────────
	// Offered by BOTH create forms: this panel's own and the in-editor
	// File ▸ New Project popup (EditorUI.cpp). They used to carry a copy each and
	// had to be kept in step by hand. Index order MUST match ProjectPreset.
	inline constexpr const char* kPresetNames[] = {
		"Empty Project",
		"Game",
		"Simulation",
		"Tool",
		"Tutorial Sandbox",
		"Third Person",
		"Application",
		"App: Sidebar",
		"App: Wizard",
		"App: Dashboard",
		"App: Form / Editor",
		"App: Tool",
	};
	inline constexpr const char* kPresetDescs[] = {
		"Only the basic folder skeleton, no extra content.",
		"Assets, Scenes and Scripts folders + a sample scene file.",
		"Assets, Scenes and Data folders.",
		"Assets and Source folders.",
		"A furnished scene (sky, ground, cube, light) to follow the interactive tutorial in.",
		"Playable on the first press of Play: ground, sky, and a character on WASD, "
		"the mouse and Space. HorizonCode.",
		"A desktop application: UI, textures and fonts, no scene and no world. Draws only "
		"when something changes.",
		"An application with a navigation sidebar on the left and a page area on the right, "
		"split by a divider you can drag.",
		"An application that asks one thing at a time: a progress bar, a step, and a "
		"Back/Next bar at the bottom.",
		"An application whose front page is a grid of cards with numbers, bars and a "
		"refresh button.",
		"An application built around one form: labelled fields in two columns, scrollable, "
		"with a Cancel/Save bar.",
		"An application shaped like a tool: toolbar on top, a list beside a work area, and "
		"a status bar along the bottom.",
	};
	inline constexpr int kPresetCount =
		static_cast<int>(sizeof(kPresetNames) / sizeof(kPresetNames[0]));
	static_assert(sizeof(kPresetNames) == sizeof(kPresetDescs),
		"every project template needs a name and a description");
	// The picker hands its INDEX straight to createNewProject as a ProjectPreset,
	// so a name added here without a value there (or the other way round) makes
	// the wrong template. That failed silently once — a sixth template the list
	// never showed — hence a compile error instead.
	static_assert(kPresetCount == static_cast<int>(ProjectPreset::COUNT),
		"kPresetNames and ProjectPreset must list the same templates, in the same order");
}
