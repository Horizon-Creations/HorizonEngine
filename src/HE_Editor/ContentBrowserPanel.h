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

	// Which of the three roots the browser is currently showing: 0 Content,
	// 1 Engine, 2 Source. Read by the guided tour's "look at the Engine root" step
	// — the panel's own state is file-static, so this is the only way to see it.
	int browsedRootKind();

	// The absolute path of the folder on screen — the ROOT's own path while the
	// grid is at a root, and empty only before the first frame has drawn. Callers
	// still have to ask browsedRootKind(): an Engine or Source folder is not a
	// valid destination for anything the user creates or imports. This exists so
	// an import started from the MENU can land
	// where the user is standing: File ▸ Import Asset always wrote into the content
	// root, so importing into Meshes/Props meant importing and then dragging the
	// result there — every time. The panel's folder state is file-static, so this
	// is the only way out of it.
	std::string browsedFolderPath();
}
