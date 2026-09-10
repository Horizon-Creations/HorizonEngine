#pragma once
#include "CollabDocSync.h"
#include "EditorUI.h"
#include <imgui.h>
#include <string>
#include <vector>

namespace HE { struct UIWidgetTree; }

// The UI Widget editor (UMG-style) — a top-level editor tab opened by
// double-clicking a UI Widget asset in the Content Browser. Edits the
// HE::UIWidgetTree stored in the UIWidgetAsset (treeJson = source of truth):
// a palette of element types, a WYSIWYG canvas with drag&drop placement,
// resize handles and parent-relative anchoring, a hierarchy panel, and a
// details panel that assigns per-element materials and behavior scripts.
//
// Panel state is keyed by asset path (survives tab switches / close+reopen),
// mirroring MaterialEditorPanel.
namespace UIEditorPanel
{
	// Render the editor for the widget at `assetPath`, filling the given rect.
	void render(AppContext& ctx, const std::string& assetPath,
	            const ImVec2& pos, const ImVec2& size);

	// True if the tree has edits not yet saved to disk (drives the tab's dirty mark).
	bool isDirty(const std::string& assetPath);

	// Re-read the file on the next frame (collab: a peer's change landed).
	bool reloadFromDisk(const std::string& assetPath);
	// Paths of every unsaved tab this panel holds, open or already closed.
	// See AssetPanelState::appendDirtyPaths — a closed dirty tab keeps its
	// state but leaves the tab vector, so the quit guard must ask here.
	void appendDirtyPaths(std::vector<std::string>& out);

	// Write this tab's tree + graph to disk, exactly like the header's Save button —
	// so the close/quit prompt can save this asset without the user having to walk
	// back into the tab. Returns true when nothing is left unsaved for this path
	// (including the "this panel never held it" case).
	bool save(AppContext& ctx, const std::string& assetPath);

	// Whether the .hasset at `path` is a UI widget asset (header sniff; cached).
	bool isWidgetAsset(const std::string& path);

	// Drop cached editor state for `path`.
	void forget(const std::string& assetPath);

	// The live documents behind this tab, for collaboration's item-level sync.
	// Empty when this panel does not hold `assetPath` — same "ask everyone, the
	// owner answers" dispatch as save() and reloadFromDisk().
	CollabDocSync::DocBindings collabDocs(const std::string& assetPath);

	// ── Authoring a widget from outside the editor (McpToolRegistry.h) ───────
	// The same tree collabDocs wraps, handed out raw so the MCP widget tools can
	// edit it. Null unless this panel already HOLDS the asset, and that is the
	// point: while a tab has it, the tab's tree is the truth and the loaded
	// asset is only a copy the tab refreshes — an MCP edit written into that
	// copy would be thrown away by the human's next Save.
	//
	// These four take the CONTENT-RELATIVE path, not the absolute one the
	// functions above are keyed by: that is the address every MCP tool uses, and
	// a full filesystem path on that interface would put the user's home
	// directory into a conversation for no gain.
	HE::UIWidgetTree* liveTree(const std::string& contentPath);
	// Finish an edit of that tree: the undo snapshot, the dirty mark and the
	// refresh of the loaded asset — commitEdit, which is what the panel itself
	// calls after a human's edit, so the two leave the tab in the same state.
	void markEdited(AppContext& ctx, const std::string& contentPath);
	// isDirty()/save(), addressed the same way.
	bool isDirtyByContentPath(const std::string& contentPath);
	bool saveByContentPath(AppContext& ctx, const std::string& contentPath);

}
