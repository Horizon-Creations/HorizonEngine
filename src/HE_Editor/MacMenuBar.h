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
		ResetLayout, ToggleProfiler, ToggleEnvironment, ToggleCollab, ToggleSourceControl,
		OpenLevelScript, OpenGameInstance,
		ImportAsset, ExportProject,
		OpenTutorial, ReportIssue,
	};

	// Build + set NSApp.mainMenu (idempotent). Call after SDL created the app.
	void install();
	// True once install() succeeded (always false off-macOS).
	bool available();
	// Enable/disable the project-scoped items (scene ops, import, export, …).
	void setProjectLoaded(bool loaded);

	// Tick or untick a toggle item. The ImGui menu row shows a panel's open
	// state through MenuItem's `selected` argument; the native menu has no such
	// thing on its own, so the same state has to be pushed in — otherwise the
	// one menu bar most users actually see is the one that cannot tell them
	// whether a panel is already open. Unknown or non-toggle commands are
	// ignored.
	void setToggleState(Cmd cmd, bool on);
	// Dequeue the next pending menu command (None when the queue is empty).
	Cmd take();
}
