#pragma once

// ── macOS native menu bar ─────────────────────────────────────────────────────
// On macOS the editor's main menu lives in the system menu bar (next to the
// Apple menu) like any Mac app, and the in-window ImGui menu row is dropped.
// The native menu is built once (install) and posts commands into a queue;
// EditorUI drains the queue each frame and runs the SAME actions the ImGui
// menu items trigger on other platforms. Compiled only on __APPLE__
// (MacMenuBar.mm); the header is safe to include everywhere.
namespace MacMenuBar
{
	enum class Cmd
	{
		None = 0,
		NewProject, OpenProject, CloseProject,
		NewScene, OpenScene, AddSceneAdditive, SaveScene, SaveSceneAs,
		Quit, Preferences,
		ResetLayout, ToggleProfiler, OpenLevelScript, OpenGameInstance,
		ImportAsset, ExportProject,
	};

	// Build + set NSApp.mainMenu (idempotent). Call after SDL created the app.
	void install();
	// True once install() succeeded (always false off-macOS).
	bool available();
	// Enable/disable the project-scoped items (scene ops, import, export, …).
	void setProjectLoaded(bool loaded);
	// Dequeue the next pending menu command (None when the queue is empty).
	Cmd take();
}
