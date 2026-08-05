#pragma once

#include <imgui.h>   // ImGuiID

// ── "Is this panel part of the docked layout?" ───────────────────────────────
// One question, its own translation unit, so the answer can be asserted
// headless instead of trusted.
//
// Not ImGui::IsWindowDocked(): that reads the CURRENT window and is only valid
// between a window's Begin and End. The visibility bookkeeping that needs this
// lives in EditorUI, outside every panel's Begin/End, so using it would mean
// threading a dock flag back out of four panels — state duplicated in four
// places for a fact imgui already stores. ImGuiWindow::DockNode is that fact,
// reachable by window name from anywhere.
//
// A node that is itself floating (panels dragged together into a loose group)
// does not count: that still looks and behaves like a floating window rather
// than part of the layout.
//
// Kept ImGui-only and free of AppContext so tests/test_editor_dock_state.cpp
// can drive real docking with nothing but a context and a display size.
namespace EditorDockState
{
	// False when no window by that name exists yet (never opened this session),
	// which is the same answer a closed panel deserves — and false with no
	// ImGui context at all, which is how shutdown reaches this.
	bool isDockedInLayout(const char* windowTitle);

	// ── The editor's one dockspace ───────────────────────────────────────────
	// The host window EditorUI submits the layout into, and the label it hashes
	// the dockspace node out of. Named here because mainDockspaceId() derives
	// the id from these exact two strings — EditorUI must not spell them a
	// second time, or the id would silently drift from the one in imgui.ini and
	// every saved layout would be read as "no layout".
	inline constexpr const char* kHostWindowName = "##EditorDockSpace";
	inline constexpr const char* kDockspaceLabel = "##MainDockSpace";

	// The id of that node — the value ImGui::GetID(kDockspaceLabel) returns
	// inside the host window, computed from the names rather than read back off
	// the live window, so it is also correct on a frame where the host is not
	// submitted (an asset tab fills the editor) or does not exist yet. The two
	// spellings are pinned against each other in the tests.
	ImGuiID mainDockspaceId();

	// Tell ImGui the dockspace still exists on a frame where it is NOT drawn.
	//
	// Panels that may float (the View-menu ones) are submitted on every tab, so
	// without this a DOCKED one meets a dockspace node that went missing, and
	// ImGui's answer to that is to undock it: it then draws as a loose window
	// over the asset tab — the thing that made panels "stay on top" — and its
	// place in the user's layout is gone for good. Keeping the node alive
	// instead leaves it docked, and since the node is not visible this frame
	// ImGui hides every window docked into it (their Begin returns false).
	// Which is exactly the rule: a docked panel belongs to the scene tab.
	//
	// Must run before any docked window's Begin in the same frame. No-op
	// without an ImGui context.
	void keepMainDockspaceAlive();
}
