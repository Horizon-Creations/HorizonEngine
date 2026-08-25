#include "PanelSpotlight.h"

#if __has_include(<imgui.h>)

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName, ImGuiDockNode — no public equivalent

#include <cmath>
#include <string>

namespace HE::Ed::Spotlight
{

// The outline has to land on the panel the caller is actually talking about, in
// every layout the user can produce. Three things make that non-obvious:
//
//  * A DOCKED window's Pos/Size is the node's *inner* rect — it excludes the
//    tab bar. Outlining that draws a box that does not line up with what the
//    user perceives as the panel, so the dock node's rect is used instead.
//  * A docked window whose tab is NOT selected is inactive and has a stale
//    rect. Outlining it would put the box on top of whatever tab IS showing —
//    the wrong panel entirely. The node is outlined in that case, so the box
//    frames the tab bar the user has to click.
//  * A window that does not exist yet (never opened, or opened into another
//    viewport) must not be outlined at all rather than at {0,0}.
//
// Drawn on the target viewport's foreground list, so it sits over docked
// windows and lands in the right OS window when a panel was dragged out.
bool outline(const char* name, float time, bool dimmed)
{
	if (!name || name[0] == '\0') return false;
	ImGuiWindow* w = ImGui::FindWindowByName(name);
	if (!w) return false;

	ImVec2 pos, size;
	if (ImGuiDockNode* node = w->DockNode; node && node->HostWindow)
	{
		// The whole docked slot, tab bar included — and valid even while another
		// tab of the same node is the visible one. The host must be on screen,
		// though: a node inside a hidden host has a stale rect that would put the
		// outline somewhere the user is not looking.
		if (!node->HostWindow->WasActive) return false;
		pos  = node->Pos;
		size = node->Size;
	}
	else
	{
		if (!w->WasActive || w->Hidden || w->Collapsed) return false;
		pos  = w->Pos;
		size = w->Size;
	}
	if (size.x < 8.0f || size.y < 8.0f) return false;

	const float pulse = dimmed ? 0.22f
	                           : 0.45f + 0.35f * (0.5f + 0.5f * std::sin(time * 3.2f));
	const ImVec4 col = dimmed ? ImVec4(0.45f, 0.85f, 0.55f, pulse)   // already done
	                          : ImVec4(0.45f, 0.72f, 1.00f, pulse);
	ImDrawList* dl = ImGui::GetForegroundDrawList(w->Viewport);
	// Inset by half the stroke so the rectangle sits ON the panel edge instead
	// of half outside it (which reads as covering the neighbouring panel).
	const float inset = 2.0f;
	dl->AddRect(ImVec2(pos.x + inset, pos.y + inset),
	            ImVec2(pos.x + size.x - inset, pos.y + size.y - inset),
	            ImGui::GetColorU32(col), 6.0f, 0, 3.0f);
	return true;
}

// RootWindow deliberately does NOT cross dock nodes, so a docked panel reports
// itself while a child window inside it (the Content Browser's asset grid, the
// Details scroll region) reports the panel — which is what the user clicked.
//
// Everything from "###" on is stripped: the left panel is "Landscape###Quick
// Settings" in Landscape mode and "Quick Settings###Quick Settings" otherwise,
// and callers name panels by the stable id, not by the visible title.
const char* focusedPanel()
{
	static std::string s_name;   // outlives the call; overwritten each time
	s_name.clear();

	ImGuiContext* g = ImGui::GetCurrentContext();
	if (!g || !g->NavWindow) return "";
	const ImGuiWindow* w = g->NavWindow->RootWindow ? g->NavWindow->RootWindow
	                                               : g->NavWindow;
	if (!w->Name) return "";
	s_name = w->Name;
	if (const std::size_t hash = s_name.find("###"); hash != std::string::npos)
		s_name = s_name.substr(hash + 3);
	return s_name.c_str();
}

} // namespace HE::Ed::Spotlight

#endif // __has_include(<imgui.h>)
