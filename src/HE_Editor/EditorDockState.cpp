#include "EditorDockState.h"

#include <imgui.h>
#include <imgui_internal.h>   // ImGuiWindow::DockNode — see the header
#include <vector>

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

namespace
{
// One dock node whose remembered tab is still being held (holdSavedTabs).
struct HeldTab
{
	ImGuiID nodeId        = 0;
	ImGuiID tabId         = 0;   // the tab imgui.ini said was selected
	int     lastTabCount  = 0;   // tabs in the node's bar when last looked at
	int     steadyFrames  = 0;   // frames since a tab last joined
};
std::vector<HeldTab> s_held;
ImGuiContext*        s_heldFor     = nullptr;   // the context s_held was taken from
int                  s_lastFrame   = 0;         // its frame count when last looked at
int                  s_heldFrames  = 0;         // frames with a tab bar to look at, since then
// How long the hold lasts at most, and how long a node must stay as it is to
// count as settled: a panel that appears 20 s in is a person opening it.
constexpr int kHoldMaxFrames   = 60 * 45;
constexpr int kHoldSteadyFrames = 60 * 4;
// How soon after a tab joined a changed selection still counts as the arrival's doing:
// the focus a new window takes lands a frame or two after it appears.
constexpr int kHoldSettleFrames = 12;
} // namespace

void forgetSavedTabs()
{
	s_held.clear();
	s_heldFrames = kHoldMaxFrames;   // and do not capture again for this context
}

void holdSavedTabs()
{
	ImGuiContext* g = ImGui::GetCurrentContext();
	if (!g) { s_held.clear(); s_heldFor = nullptr; return; }

	// A new context can be allocated where the last one was; its frame count
	// starting over is what gives it away.
	const bool newContext = s_heldFor != g || g->FrameCount < s_lastFrame;
	s_lastFrame = g->FrameCount;
	if (newContext)
	{
		// First call for this context: the ini has been read (the first NewFrame
		// does that) and no window has submitted anything yet, so every node's
		// SelectedTabId is exactly what the file said.
		s_heldFor    = g;
		s_heldFrames = 0;
		s_held.clear();
		for (int n = 0; n < g->DockContext.Nodes.Data.Size; ++n)
		{
			const ImGuiDockNode* node = static_cast<const ImGuiDockNode*>(g->DockContext.Nodes.Data[n].val_p);
			if (node && node->IsLeafNode() && node->SelectedTabId != 0)
				s_held.push_back({ node->ID, node->SelectedTabId, 0, 0 });
		}
		return;
	}
	if (s_held.empty()) return;

	bool anyBar = false;
	for (size_t i = 0; i < s_held.size();)
	{
		HeldTab& h = s_held[i];
		ImGuiDockNode* node = ImGui::DockContextFindNodeByID(g, h.nodeId);
		ImGuiTabBar*   bar  = node ? node->TabBar : nullptr;
		if (!bar) { ++i; continue; }   // the node has no windows yet
		anyBar = true;

		const int tabs = bar->Tabs.Size;
		const bool joined = tabs > h.lastTabCount;
		h.lastTabCount = tabs;
		h.steadyFrames = joined ? 0 : h.steadyFrames + 1;

		// A click anywhere in the node is a person choosing: let go for good.
		// (Not a click elsewhere — the one that picks a project in the hub comes
		// long before any panel exists.)
		bool done = false;
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			const ImVec2 mouse = g->IO.MousePos;
			if (mouse.x >= node->Pos.x && mouse.x < node->Pos.x + node->Size.x &&
			    mouse.y >= node->Pos.y && mouse.y < node->Pos.y + node->Size.y)
				done = true;
		}

		const bool here     = ImGui::TabBarFindTabByID(bar, h.tabId) != nullptr;
		const bool selected = node->SelectedTabId == h.tabId;
		if (done || !here)
		{
			// Not arrived yet (the time limit above ends the wait), or let go.
		}
		else if (selected)
		{
			if (h.steadyFrames > kHoldSteadyFrames) done = true;
		}
		else if (h.steadyFrames <= kHoldSettleFrames)
		{
			// Tabs joined lately, and one of them has the selection — by being new,
			// or by having been given the focus a frame after it appeared. Give it
			// back.
			node->SelectedTabId = h.tabId;
			bar->SelectedTabId = bar->NextSelectedTabId = h.tabId;
			// imgui also re-selects the tab of the focused window in the node each
			// frame, and a panel that has just appeared took the focus: move the
			// focus along, or the tab just put back is taken away again next frame.
			// Only when the focus is in this node — a person typing in another
			// panel is none of our business.
			if (g->NavWindow && g->NavWindow->RootWindow->DockNode == node)
				for (ImGuiWindow* w : node->Windows)
					if (w->TabId == h.tabId) { ImGui::FocusWindow(w); break; }
		}
		else
		{
			// Changed long after anything joined, with no click in the node: a
			// panel was revealed (the footer, a menu). Not ours to undo.
			done = true;
		}
		if (done) s_held.erase(s_held.begin() + static_cast<std::ptrdiff_t>(i));
		else      ++i;
	}
	// The clock only runs while there are tabs to look at: the project hub can
	// sit on screen for as long as somebody takes to pick a project, with no
	// panel submitted at all.
	if (anyBar && ++s_heldFrames > kHoldMaxFrames) s_held.clear();
}

} // namespace EditorDockState
