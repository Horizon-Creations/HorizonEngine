#include "HcWatchPanel.h"
#include "HcWatch.h"               // the rows
#include "HcExecTrace.h"           // the attached runtime, the paused site, reveal
#include "EditorApplication.h"     // AppContext
#include "EditorWidgets.h"         // the help-aware button / menu item
#include "EditorHelp.h"            // "Watch/<label>" scope for its controls
#include "EditorTheme.h"           // the accent for the stop line

#include <HorizonCode/HorizonCodeRuntime.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>   // InputTextWithHint over std::string
#endif

namespace HcWatchPanel
{
#ifdef HE_IMGUI_ENABLED

namespace
{
	// Which of several stopped runs the window shows. Clamped every frame:
	// Continue shortens the list under it.
	int         s_runIndex = 0;
	// The filter: a row shows when its name contains this (case-insensitive).
	// Kept while the window is closed — reopening it on the same variable is
	// the common case.
	std::string s_filter;

	std::string lower(std::string v)
	{
		for (char& c : v) c = (char)std::tolower((unsigned char)c);
		return v;
	}

	bool passesFilter(const HcWatch::Row& r, const std::string& lowerFilter)
	{
		return lowerFilter.empty() || lower(r.name).find(lowerFilter) != std::string::npos;
	}

	// One section as a three-column table: name, type, value. Defined AFTER
	// the window below, whose "Watch" help scope its row menu belongs to —
	// the help audit reads a file top to bottom and files a control under the
	// last scope it saw.
	void drawRows(const std::vector<HcWatch::Row>& rows, const std::string& lowerFilter, const char* id);
}

void DrawWatchWindow(AppContext& ctx, bool& open)
{
	// Every control here is looked up as "Watch/<its label>".
	HE::Ed::Help::Scope helpScope("Watch");
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(380.0f, 420.0f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Watch", &open)) { ImGui::End(); return; }

	HorizonCode::Runtime* rt = HcExecTrace::attachedRuntime();
	if (!ctx.projectLoaded || !rt)
	{
		ImGui::TextDisabled("No project is open.");
		ImGui::End();
		return;
	}

	// ── Nothing stopped: the live Game Instance, or a hint ──
	if (!rt->isSuspended())
	{
		const bool running = ctx.isPlaying || ctx.appLivePreview;
		if (!running)
		{
			ImGui::TextWrapped("Nothing is running. Play the scene; a HorizonCode run that reaches a "
			                   "breakpoint stops there and its values appear here.");
			ImGui::End();
			return;
		}
		ImGui::TextDisabled("No run is stopped.");
		ImGui::TextWrapped("Right-click a node in a graph and Add Breakpoint; the run stops before "
		                   "that node. Until then, the Game Instance's variables, live:");
		// The pause that lands on a node: arm the runtime's one-shot, and show
		// that it is armed until something runs — a game waiting on input
		// executes no node at all, and a silently armed stop would surprise
		// the next click. Only a flag is flipped here; the stop itself
		// happens inside the tick, like a breakpoint's.
		if (rt->debugBreakNextArmed())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::AccentBright);
			ImGui::TextUnformatted("Armed: stops at the next node.");
			ImGui::PopStyleColor();
			ImGui::SameLine();
			if (EditorWidgets::smallButton("Disarm")) rt->debugBreakNext(false);
		}
		else if (EditorWidgets::button("Break on Next Node"))
			rt->debugBreakNext();
		ImGui::Separator();
		ImGui::InputTextWithHint("##watchFilter", "Filter", &s_filter);
		const std::vector<HcWatch::Row> live = HcWatch::variableRows(*rt, rt->gameInstance());
		drawRows(live, lower(s_filter), "##liveVars");
		ImGui::End();
		return;
	}

	// ── A stopped run ──
	const size_t runCount = rt->suspendedSites().size();
	s_runIndex = std::clamp(s_runIndex, 0, (int)runCount - 1);
	const HcWatch::Snapshot snap = HcWatch::build(*rt, (size_t)s_runIndex);
	if (!snap.valid) { ImGui::End(); return; }   // raced a Continue between frames

	// Where: the node, the class, the instance — and the way there.
	ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::AccentBright);
	ImGui::TextUnformatted("Stopped at");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::TextUnformatted(snap.nodeLabel.c_str());
	{
		char where[128];
		std::snprintf(where, sizeof(where), "%s  #%u", HcWatch::classLabel(snap.classKey).c_str(), snap.instance);
		ImGui::TextDisabled("%s", where);
	}
	ImGui::SameLine();
	{
		const float goW = ImGui::CalcTextSize("Go to Node").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - goW);
		if (EditorWidgets::smallButton("Go to Node")) HcExecTrace::requestReveal(snap.classKey, snap.nodeId);
	}
	// Several runs stopped (two instances of one class on the same
	// breakpoint): pick which one to look at. The first is what Step acts on.
	if (runCount > 1)
	{
		ImGui::BeginDisabled(s_runIndex == 0);
		if (ImGui::ArrowButton("##prevRun", ImGuiDir_Left)) --s_runIndex;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::Text("Run %d of %zu%s", s_runIndex + 1, runCount, s_runIndex == 0 ? " (Step acts on this one)" : "");
		ImGui::SameLine();
		ImGui::BeginDisabled(s_runIndex + 1 >= (int)runCount);
		if (ImGui::ArrowButton("##nextRun", ImGuiDir_Right)) ++s_runIndex;
		ImGui::EndDisabled();
	}
	ImGui::InputTextWithHint("##watchFilter", "Filter", &s_filter);
	ImGui::Separator();

	const std::string lowerFilter = lower(s_filter);
	ImGui::BeginChild("##watchSections", ImVec2(0, 0), ImGuiChildFlags_None);
	for (size_t i = 0; i < snap.sections.size(); ++i)
	{
		const HcWatch::Section& sec = snap.sections[i];
		ImGui::PushID((int)i);
		// The innermost frame is where the stop is; say so on its header.
		std::string title = sec.title;
		if (sec.kind == HcWatch::Section::Kind::EventArg) title = "Argument of " + title;
		if (sec.kind == HcWatch::Section::Kind::Frame &&
		    (i == 0 || snap.sections[i - 1].kind != HcWatch::Section::Kind::Frame))
			title += "  (stopped here)";
		title += "###sec";
		if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
			drawRows(sec.rows, lowerFilter, "##rows");
		ImGui::PopID();
	}
	ImGui::EndChild();

	ImGui::End();
}

namespace
{
	// The value column is clipped by the table; the whole line is in the
	// tooltip, which is where a long string or a full array is read.
	// Right-click copies either the value or the whole row.
	void drawRows(const std::vector<HcWatch::Row>& rows, const std::string& lowerFilter, const char* id)
	{
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
		                              ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
		if (!ImGui::BeginTable(id, 3, flags)) return;
		ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn("Type",  ImGuiTableColumnFlags_WidthStretch, 0.8f);
		ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
		int shown = 0;
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const HcWatch::Row& r = rows[i];
			if (!passesFilter(r, lowerFilter)) continue;
			++shown;
			ImGui::PushID((int)i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			// The name cell spans the row as the hover/click target, so the
			// tooltip and the context menu address the whole line.
			ImGui::Selectable(r.name.c_str(), false,
			                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
			const bool hovered = ImGui::IsItemHovered();
			if (ImGui::BeginPopupContextItem("##watchRow"))
			{
				if (EditorWidgets::menuItem("Copy Value")) ImGui::SetClipboardText(r.value.c_str());
				if (EditorWidgets::menuItem("Copy Row"))
					ImGui::SetClipboardText((r.name + "  " + r.type + "  " + r.value).c_str());
				ImGui::EndPopup();
			}
			ImGui::TableNextColumn();
			ImGui::TextColored(HE::Ed::Theme::TextDim, "%s", r.type.c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(r.value.c_str());
			if (hovered)
			{
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
				ImGui::TextUnformatted(r.name.c_str());
				ImGui::SameLine();
				ImGui::TextColored(HE::Ed::Theme::TextDim, "%s", r.type.c_str());
				ImGui::TextUnformatted(r.value.c_str());
				if (r.ref != 0) ImGui::TextDisabled("Object #%u", r.ref);
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
		if (shown == 0)
			ImGui::TextDisabled(rows.empty() ? "  (none)" : "  (nothing matches the filter)");
	}
}

#else
void DrawWatchWindow(AppContext&, bool&) {}
#endif

} // namespace HcWatchPanel
