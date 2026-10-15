#pragma once

#include <string>
#include <vector>

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
//
// The menus, in order, mirror EditorUI.cpp's ImGui bar (docs/editor-menu-
// restructure-2026-09-21.md has the map): File, Edit, Entity, Assets, Play,
// Build, View, Window, Help — plus the app menu macOS puts first.
namespace MacMenuBar
{
	enum class Cmd
	{
		None = 0,
		// ── File ──
		NewProject, OpenProject,
		// File ▸ Recent Projects ▸ <row>: arg() = index into the list last
		// handed to setRecentProjects.
		OpenRecentProject,
		CloseProject,
		// Save = whatever tab is in front (asset tab → that asset, Scene tab →
		// the scene); SaveAll = every unsaved asset plus a dirty scene.
		NewScene, OpenScene, AddSceneAdditive, Save, SaveAll, SaveSceneAs,
		Quit, Preferences,
		// ── Edit. Deliberately WITHOUT key equivalents — see install()'s Edit
		// block: a native ⌘Z (or ⌘C, ⌘V …) would swallow the keystroke before
		// the app sees it, and the per-panel undo stacks (material graph, UI
		// editor, text fields) all live on those keys.
		Undo, Redo,
		// The scene's clipboard verbs, on the selected ENTITY (not on text).
		Cut, Copy, Paste, Duplicate, Delete,
		// Edit ▸ Select All / Deselect All, on the scene's entities. No key
		// equivalents either: ⌘A selects the text in a field being edited.
		SelectAll, DeselectAll,
		// Edit ▸ Project Settings…: the tab that edits the PROJECT (Preferences
		// stays in the app menu and edits the editor).
		ProjectSettings,
		// ── Entity (game projects only) ──
		// Entity ▸ Create ▸ <row>: arg() = index into OutlinerPanel's preset
		// table, the same list the Outliner's right-click menu draws.
		CreateEntity,
		FocusSelected, SnapToGround, HideSelected, IsolateSelected, ShowAll,
		Group, Ungroup, ToggleLock, SaveAsPrefab,
		// ── Assets ──
		CreateAsset, ImportAsset, RefreshAssets,
		// Only added to the menu when HE_HAVE_LIBSSH2 AND ContentManager::
		// isEngineContentDevMode() are both true — see install()'s Assets block.
		PublishEngineContent, RebuildManifestFromServer,
		// ── Play. The toolbar's transport well as menu rows; the row titles
		// follow the state (Play/Stop, Pause/Resume/Continue) via setItemTitle.
		PlayToggle, PauseToggle, StepFrame, StepNode,
		// ── Build ──
		ExportProject,
		// Build ▸ Build and Reload Game Logic. Project-scoped like the export,
		// but NOT gated on the project's language here — the native menu is
		// built once and a project can be swapped underneath it, so the row
		// stays live and the action says what it did (GameLogicBuildPanel).
		BuildGameLogic,
		// ── View: how the Scene window draws. No Toggle Full Screen command
		// on purpose: the View menu carries the NATIVE one (toggleFullScreen:
		// on the responder chain, ⌃⌘F), and a second item on SDL's own
		// fullscreen would be a different behaviour under the same name.
		// SetViewMode: arg() = HE::ViewMode. ToggleShowFlag: arg() = index into
		// ViewportPanel::showFlagFields. SetViewPreset: arg() =
		// EditorCamera::ViewPreset.
		SetViewMode, ToggleShowFlag, ShowAllOverlays, HideAllOverlays,
		SetViewPreset, ToggleOrthographic,
		// ── Window: everything that opens a panel or a tab ──
		ToggleConsole, ToggleProfiler, ToggleEnvironment, ToggleCollab,
		ToggleSourceControl, ToggleAudioMixer, ToggleUndoHistory, ToggleWatch,
		// Window ▸ Scene 2 / 3 / 4, the secondary scene viewports. Game-only
		// like the grid: an application has no level to look at from above.
		ToggleScene2, ToggleScene3, ToggleScene4,
		OpenLevelScript, OpenGameInstance,
		// Window ▸ Landscape Tools: the Scene toolbar's Landscape mode, which
		// turns the Quick Settings panel into the landscape tool panel.
		ToggleLandscapeTools,
		ResetLayout,
		// ── Help ──
		OpenTutorial, ReportIssue,
		// Help ▸ Documentation opens the manual INSIDE the editor (DocsPanel);
		// DocumentationOnline is the website. Both exist because they answer
		// different needs — one keeps you where you are, the other is
		// shareable and always current.
		Documentation, SearchDocumentation, DocumentationOnline,
	};

	// Build + set NSApp.mainMenu (idempotent). Call after SDL created the app.
	void install();
	// True once install() succeeded (always false off-macOS).
	bool available();
	// Enable/disable the project-scoped items (scene ops, import, export, …).
	void setProjectLoaded(bool loaded);
	// An APPLICATION project has no scenes, no ground and no level script, so the
	// rows that act on them are HIDDEN rather than greyed out (docs/he-apps-plan.md
	// E2): a disabled row still says "this exists here", which is the wrong thing
	// to say. Kept in step with the same trimming in the ImGui menu bar.
	void setAppProject(bool isApp);

	// Tick or untick a toggle item. The ImGui menu row shows a panel's open
	// state through MenuItem's `selected` argument; the native menu has no such
	// thing on its own, so the same state has to be pushed in — otherwise the
	// one menu bar most users actually see is the one that cannot tell them
	// whether a panel is already open. Unknown or non-toggle commands are
	// ignored. The three-argument form is for the rows that carry an argument
	// (a view mode, a show flag, a view preset).
	void setToggleState(Cmd cmd, bool on);
	void setToggleState(Cmd cmd, int arg, bool on);
	// Retitle a row whose verb follows the editor's state ("Play" / "Stop").
	// Only rows without an argument; a no-op for an unknown command or the
	// same title again.
	void setItemTitle(Cmd cmd, const char* title);
	// File ▸ Recent Projects ▸: rebuilt only when the list changed. A path that
	// no longer exists is listed disabled, as the Project Hub shows it.
	void setRecentProjects(const std::vector<std::string>& paths);

	// Dequeue the next pending menu command (None when the queue is empty);
	// arg() is the argument that rode with the command take() just returned.
	Cmd take();
	int arg();
}
