#pragma once

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
}
