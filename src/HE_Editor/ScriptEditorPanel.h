#pragma once
#include <string>
#include <vector>
#include <imgui.h>

struct AppContext;

// A syntax-highlighting code editor for script assets (Lua / Python), shown as a
// top-level editor tab (opened by double-clicking a script in the Content Browser).
// Editor state — the ImGuiColorTextEdit instance, its buffer and dirty tracking —
// is kept internally, keyed by the asset's file path, so it survives tab switches
// and even a tab close+reopen within the same session.
namespace ScriptEditorPanel
{
	// Render the editor for the script at `assetPath`, filling the given rect
	// (below the tab bar, above the footer). Lazy-loads the source on first use.
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// True if the editor for `assetPath` has unsaved edits (drives the tab's dirty mark).
	bool isDirty(const std::string& assetPath);

	// Re-read the file on the next frame (collab: a peer's change landed).
	bool reloadFromDisk(const std::string& assetPath);
	// Paths of every unsaved tab this panel holds, open or already closed.
	// See AssetPanelState::appendDirtyPaths — a closed dirty tab keeps its
	// state but leaves the tab vector, so the quit guard must ask here.
	void appendDirtyPaths(std::vector<std::string>& out);

	// Write the editor's buffer for `assetPath` to disk, exactly like the tab's own
	// Save button — so the close/quit prompt can save this asset without the user
	// having to walk back into the tab. Returns true when nothing is left unsaved
	// for this path (including the "this panel never held it" case).
	bool save(const std::string& assetPath);

	// Whether the .hasset at `path` is a script asset (reads the HAsset header type).
	bool isScriptAsset(const std::string& path);

	// Drop cached editor state for `path` (optional; state otherwise persists for the
	// session so unsaved edits survive a tab close+reopen).
	void forget(const std::string& assetPath);
}
