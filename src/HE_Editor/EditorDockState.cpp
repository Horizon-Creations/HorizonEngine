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

} // namespace EditorDockState
