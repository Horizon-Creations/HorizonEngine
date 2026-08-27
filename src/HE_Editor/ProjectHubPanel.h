#pragma once

struct AppContext;

// ── Project Hub ──────────────────────────────────────────────────────────────
// The full-window start screen shown while no project is open: the header bar,
// the "New Project" form (template preset + script language + target folder),
// the recent-project list and the async open-project / choose-folder dialogs.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace ProjectHubPanel
{
	void render(AppContext& ctx);

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
		"Application",
	};
	inline constexpr const char* kPresetDescs[] = {
		"Only the basic folder skeleton, no extra content.",
		"Assets, Scenes and Scripts folders + a sample scene file.",
		"Assets, Scenes and Data folders.",
		"Assets and Source folders.",
		"A furnished scene (sky, ground, cube, light) to follow the interactive tutorial in.",
		"A desktop application: UI, textures and fonts, no scene and no world. Draws only "
		"when something changes.",
	};
	inline constexpr int kPresetCount =
		static_cast<int>(sizeof(kPresetNames) / sizeof(kPresetNames[0]));
	static_assert(sizeof(kPresetNames) == sizeof(kPresetDescs),
		"every project template needs a name and a description");
}
