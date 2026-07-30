#pragma once

struct AppContext;

// ── Details panel (Inspector) ────────────────────────────────────────────────
// The per-entity property editor on the right: name, every component's fields,
// the asset slots that reference other .hassets, "Add Component" and the
// remove-component menus. Every widget routes its edits through the editor undo
// system (capturePre on the mouse press, stash/commit around the widget).
// Split out of EditorUI.cpp; it keeps no file-static state of its own beyond a
// couple of function-local caches and talks to the editor only via AppContext.
namespace InspectorPanel
{
	void render(AppContext& ctx);
}
