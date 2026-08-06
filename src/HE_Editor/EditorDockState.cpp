#include "EditorDockState.h"

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiWindow::DockNode — see the header

namespace EditorDockState
{

bool isDockedInLayout(const char* windowTitle)
{
	if (!windowTitle || !ImGui::GetCurrentContext()) return false;

	const ImGuiWindow* w = ImGui::FindWindowByName(windowTitle);
	if (!w || !w->DockNode) return false;
	return !w->DockNode->IsFloatingNode();
}

ImGuiID mainDockspaceId()
{
	// ImGui::GetID() hashes against the top of the current window's id stack,
	// and a window's stack starts at ImHashStr(name) — so this is that call,
	// spelled without needing the window to be current (or to exist).
	return ImHashStr(kDockspaceLabel, 0, ImHashStr(kHostWindowName, 0, 0));
}

void keepMainDockspaceAlive()
{
	if (!ImGui::GetCurrentContext()) return;
	// KeepAliveOnly submits nothing into the current window (it may be called
	// from anywhere in the frame) — it only marks the node as still there.
	ImGui::DockSpace(mainDockspaceId(), ImVec2(0.0f, 0.0f),
	                 ImGuiDockNodeFlags_KeepAliveOnly);
}

} // namespace EditorDockState
