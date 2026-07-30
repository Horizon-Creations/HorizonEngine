#pragma once
#include <functional>
#include <string>

struct AppContext;

// ── Content Browser ───────────────────────────────────────────────────────────
// The bottom dock panel: folder tree + asset grid over the three content roots
// (Content, Engine, and — for C++ projects — Source), plus everything that hangs
// off them: create/import/rename/delete, drag-to-move, the item context menus and
// the double-click dispatch that opens an asset's editor tab.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace ContentBrowserPanel
{
	// `tabSelectRequest` is the editor shell's one-shot "select this top-level
	// tab" slot: double-clicking an asset opens its editor tab and has to force
	// the tab bar to focus it next frame. `openSceneGuarded` runs a scene open
	// through the shell's unsaved-changes guard (double-clicking a .hescene).
	// Both are owned by renderEditor — hence passed in rather than duplicated.
	void render(AppContext& ctx, int& tabSelectRequest,
	            const std::function<void(const std::string&)>& openSceneGuarded);

	// Requested by fast local content edits (create/rename) that want the file list
	// updated this frame without the heavyweight "##ContentRefresh" progress modal.
	// Consumed at the top of EditorUI::render(), outside the content-folder shared lock.
	bool quietRefreshRequested();
	void clearQuietRefreshRequest();
}
