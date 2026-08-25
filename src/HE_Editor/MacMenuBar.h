#pragma once

// ── macOS native menu bar ─────────────────────────────────────────────────────
// On macOS the editor's main menu lives in the system menu bar (next to the
// Apple menu) like any Mac app, and the in-window ImGui menu row is dropped
// ENTIRELY (EditorUI.cpp: `if (!nativeMenu) { ... ImGui::BeginMainMenuBar() ... }`).
// The native menu is built once (install) and posts commands into a queue;
// EditorUI drains the queue each frame and runs the SAME actions the ImGui
// menu items trigger on other platforms. Compiled only on __APPLE__
// (MacMenuBar.mm); the header is safe to include everywhere.
//
// A menu item added to ONLY the ImGui path is invisible on macOS — not
// disabled, not greyed out, simply never built (this bit the EngineContent
// SFTP-sync menu items once already: they existed only in EditorUI.cpp's ImGui
// block for a while and could never be reached on a Mac, regardless of any
// runtime gate like isEngineContentDevMode()). Any new top-level menu action
// needs an entry HERE too, wired the same way ImportAsset/ExportProject are.
namespace MacMenuBar
{
	enum class Cmd
	{
		None = 0,
		NewProject, OpenProject, CloseProject,
		// Save = whatever tab is in front (asset tab → that asset, Scene tab →
		// the scene); SaveAll = every unsaved asset plus a dirty scene.
		NewScene, OpenScene, AddSceneAdditive, Save, SaveAll, SaveSceneAs,
		Quit, Preferences,
		// Edit. Deliberately WITHOUT key equivalents — see install()'s Edit block:
		// a native ⌘Z would swallow the keystroke before the app sees it, and the
		// per-panel undo stacks (material graph, UI editor, text fields) all live
		// on that key.
		Undo, Redo,
		ResetLayout, ToggleProfiler, ToggleEnvironment, ToggleCollab, ToggleSourceControl,
		// View, continued. No Toggle Full Screen here on purpose: the View menu
		// already carries the NATIVE one (toggleFullScreen: on the responder
		// chain, ⌃⌘F), and a second item on SDL's own fullscreen would be a
		// different behaviour under the same name.
		ToggleConsole, ToggleGroundGrid,
		OpenLevelScript, OpenGameInstance,
		ImportAsset, RefreshAssets, ExportProject,
		OpenTutorial, ReportIssue, Documentation,
		// Only added to the menu when HE_HAVE_LIBSSH2 AND ContentManager::
		// isEngineContentDevMode() are both true — see install()'s Assets block.
		PublishEngineContent, RebuildManifestFromServer,
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
