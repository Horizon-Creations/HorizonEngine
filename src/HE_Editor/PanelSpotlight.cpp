#include "PanelSpotlight.h"

#if __has_include(<imgui.h>)

#include <imgui.h>
#include <imgui_internal.h>   // FindWindowByName, ImGuiDockNode — no public equivalent

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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
bool panelRect(const char* name, ImVec2& pos, ImVec2& size, ImGuiViewport** viewport)
{
	if (!name || name[0] == '\0') return false;
	ImGuiWindow* w = ImGui::FindWindowByName(name);
	if (!w) return false;

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
	if (viewport) *viewport = w->Viewport;
	return true;
}

bool outline(const char* name, float time, bool dimmed)
{
	ImVec2 pos, size;
	ImGuiViewport* vp = nullptr;
	if (!panelRect(name, pos, size, &vp)) return false;

	const float pulse = dimmed ? 0.22f
	                           : 0.45f + 0.35f * (0.5f + 0.5f * std::sin(time * 3.2f));
	const ImVec4 col = dimmed ? ImVec4(0.45f, 0.85f, 0.55f, pulse)   // already done
	                          : ImVec4(0.45f, 0.72f, 1.00f, pulse);
	ImDrawList* dl = ImGui::GetForegroundDrawList(vp);
	// Inset by half the stroke so the rectangle sits ON the panel edge instead
	// of half outside it (which reads as covering the neighbouring panel).
	const float inset = 2.0f;
	dl->AddRect(ImVec2(pos.x + inset, pos.y + inset),
	            ImVec2(pos.x + size.x - inset, pos.y + size.y - inset),
	            ImGui::GetColorU32(col), 6.0f, 0, 3.0f);
	return true;
}

float coveredArea(ImVec2 pos, ImVec2 size, const std::vector<Box>& avoid)
{
	// Summed per box, so two boxes that overlap each other count their shared
	// part twice. That is fine for what this is for — comparing spots — and it
	// makes a spot over a panel that is outlined twice no cheaper than over one.
	float area = 0.0f;
	for (const Box& b : avoid)
	{
		const float w = std::min(pos.x + size.x, b.max.x) - std::max(pos.x, b.min.x);
		const float h = std::min(pos.y + size.y, b.max.y) - std::max(pos.y, b.min.y);
		if (w > 0.0f && h > 0.0f) area += w * h;
	}
	return area;
}

ImVec2 placeClearOf(ImVec2 cardPos, ImVec2 cardSize, ImVec2 areaMin, ImVec2 areaMax,
                    const std::vector<Box>& avoid, float margin, float gap)
{
	if (avoid.empty()) return cardPos;

	// The same clamp clampCurrentWindowToEditorWindow applies after Begin: a spot
	// that only exists outside the editor window would be undone the next frame
	// and the card would oscillate between the two. Where the card is bigger than
	// the area, the top-left corner wins, as there.
	const auto clampToArea = [&](ImVec2 p) {
		const float minX = areaMin.x + margin;
		const float minY = areaMin.y + margin;
		const float maxX = std::max(minX, areaMax.x - cardSize.x - margin);
		const float maxY = std::max(minY, areaMax.y - cardSize.y - margin);
		return ImVec2(std::clamp(p.x, minX, maxX), std::clamp(p.y, minY, maxY));
	};

	const float here = coveredArea(cardPos, cardSize, avoid);
	if (here <= 0.0f) return cardPos;

	Box all = avoid.front();
	for (const Box& b : avoid)
	{
		all.min = ImVec2(std::min(all.min.x, b.min.x), std::min(all.min.y, b.min.y));
		all.max = ImVec2(std::max(all.max.x, b.max.x), std::max(all.max.y, b.max.y));
	}

	std::vector<ImVec2> candidates = {
		ImVec2(areaMin.x, areaMin.y), ImVec2(areaMax.x, areaMin.y),
		ImVec2(areaMin.x, areaMax.y), ImVec2(areaMax.x, areaMax.y),
	};
	// Beside a box, keeping the card's other coordinate: the smallest step that
	// clears that side. The corners above cover the cases where no single side
	// is wide enough.
	const auto besides = [&](const Box& b) {
		candidates.emplace_back(b.min.x - gap - cardSize.x, cardPos.y);   // left of
		candidates.emplace_back(b.max.x + gap,              cardPos.y);   // right of
		candidates.emplace_back(cardPos.x, b.min.y - gap - cardSize.y);   // above
		candidates.emplace_back(cardPos.x, b.max.y + gap);                // below
	};
	besides(all);
	if (avoid.size() > 1)
		for (const Box& b : avoid) besides(b);

	ImVec2 best      = cardPos;
	float  bestArea  = here;
	float  bestDist2 = 0.0f;
	for (ImVec2 c : candidates)
	{
		c = clampToArea(c);
		const float area  = coveredArea(c, cardSize, avoid);
		const float dx    = c.x - cardPos.x;
		const float dy    = c.y - cardPos.y;
		const float dist2 = dx * dx + dy * dy;
		// "Less covered" with a point of slack, so float noise between two
		// equally clear spots is decided by distance and not by rounding.
		if (area < bestArea - 1.0f ||
		    (area <= bestArea + 1.0f && area < here - 1.0f && dist2 < bestDist2))
		{
			best      = c;
			bestArea  = area;
			bestDist2 = dist2;
		}
	}
	return best;
}

// Checked every frame rather than once per subject, because a panel can appear
// late — "open the Profiler" points at a window that only exists once the user
// opened it, possibly right under the card. What keeps that from becoming a
// card that fights the user is that a drag wins (m_dragged, until reset()).
void KeepClear::update(const char* cardName, const std::vector<std::string>& panels, float dt)
{
	ImGuiWindow* card = ImGui::FindWindowByName(cardName);
	if (!card || !card->WasActive) return;

	const ImGuiContext& g = *ImGui::GetCurrentContext();
	if (g.MovingWindow && g.MovingWindow->RootWindow == card)
	{
		m_dragged = true;
		m_gliding = false;
		return;
	}
	// Resizing it or pressing one of its buttons: not now, but not "theirs"
	// either — a card resized onto the panel still steps aside afterwards.
	if (m_dragged || (g.ActiveId != 0 && g.ActiveIdWindow &&
	                  g.ActiveIdWindow->RootWindow == card))
		return;

	if (!m_gliding)
	{
		std::vector<Box> avoid;
		for (const std::string& name : panels)
		{
			ImVec2 pos, size;
			ImGuiViewport* vp = nullptr;
			if (!panelRect(name.c_str(), pos, size, &vp)) continue;
			// Dragged out into its own OS window: the card cannot cover it.
			if (vp != card->Viewport) continue;
			avoid.push_back({ pos, ImVec2(pos.x + size.x, pos.y + size.y) });
		}
		// The area EditorWidgets::clampCurrentWindowToEditorWindow keeps a
		// floating card in, with its default margin — a spot outside it would be
		// clamped back the next frame and the card would never settle.
		const ImGuiViewport* main = ImGui::GetMainViewport();
		const ImVec2 want = placeClearOf(card->Pos, card->Size, main->WorkPos,
			ImVec2(main->WorkPos.x + main->WorkSize.x, main->WorkPos.y + main->WorkSize.y),
			avoid);
		if (std::fabs(want.x - card->Pos.x) < 0.5f && std::fabs(want.y - card->Pos.y) < 0.5f)
			return;
		m_gliding = true;
		m_to      = want;
		m_glideFrames = 0;
	}

	// Glide rather than jump: a card that teleports mid-sentence loses the
	// reader, one that slides over in a fifth of a second is followed by eye.
	// The target is fixed for the glide — re-deciding from every in-between spot
	// could pick a different, equally clear one each frame.
	const float k  = 1.0f - std::exp(-std::max(dt, 0.0f) * 14.0f);
	const float dx = m_to.x - card->Pos.x;
	const float dy = m_to.y - card->Pos.y;
	ImVec2 p(card->Pos.x + dx * k, card->Pos.y + dy * k);
	// The frame cap is a backstop for a target that stopped being reachable
	// mid-glide (the editor window was resized under it): land, re-decide.
	if (dx * dx + dy * dy < 1.0f || ++m_glideFrames > 45)
	{
		p         = m_to;
		m_gliding = false;
	}
	ImGui::SetNextWindowPos(p, ImGuiCond_Always);
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
