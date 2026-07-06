#pragma once
#include "EditorUI.h"
#include <imgui.h>
#include <string>

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

	// Whether the .hasset at `path` is a UI widget asset (header sniff; cached).
	bool isWidgetAsset(const std::string& path);

	// Drop cached editor state for `path`.
	void forget(const std::string& assetPath);
}
