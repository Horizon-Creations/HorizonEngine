#include "HcRenameDialog.h"
#include "HcRenameSweep.h"

#include "CollabController.h"
#include "EditorApplication.h"   // AppContext
#include "EditorHelp.h"
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "HorizonCodeClassPanel.h"
#include "NotificationStore.h"
#include "UIEditorPanel.h"

#include <ContentManager/ContentManager.h>
#include <Diagnostics/Logger.h>
#include <HorizonScene/HorizonWorld.h>

#include <imgui.h>

namespace HcRenameDialog
{
namespace
{
	struct State
	{
		bool                  open = false;
		HcRename::Target      target;
		HcRenameSweep::Report report;
	};
	State s;

	const char* memberWord(HcRename::Member m)
	{
		switch (m)
		{
			case HcRename::Member::Function: return "function";
			case HcRename::Member::Variable: return "variable";
			case HcRename::Member::Event:    return "event";
		}
		return "member";
	}

	// Why this asset must not be written from here. An open tab with unsaved
	// changes is the dangerous one: its panel holds its own copy of the graph and
	// would write it back over ours the next time the user hits save.
	std::string blockedBecause(AppContext& ctx, const std::string& relPath)
	{
		if (ctx.collab && ctx.collab->assetLockedByOther(relPath))
			return "locked by someone else in this session";
		if (UIEditorPanel::isDirty(relPath) || HorizonCodeClassPanel::isDirty(relPath))
			return "open with unsaved changes";
		return {};
	}

	void drawList(const char* heading, const std::vector<const HcRenameSweep::Entry*>& rows,
	              bool showCounts, ImU32 color)
	{
		if (rows.empty()) return;
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::TextUnformatted(heading);
		ImGui::PopStyleColor();
		for (const HcRenameSweep::Entry* e : rows)
		{
			const int n = showCounts ? (int)e->plan.rename.size() : (int)e->plan.unsure.size();
			ImGui::BulletText("%s  (%d)", e->display.c_str(), n);
			if (!e->skipWhy.empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("— %s", e->skipWhy.c_str());
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				const auto& hits = showCounts ? e->plan.rename : e->plan.unsure;
				for (const HcRename::Hit& h : hits) ImGui::TextUnformatted(h.what.c_str());
				ImGui::EndTooltip();
			}
		}
	}
} // namespace

void requestAfterRename(AppContext& ctx, const HcRename::Target& t,
                        const std::vector<std::string>& handledInMemory)
{
	if (!ctx.contentManager || t.classKey.empty() || t.oldName.empty() || t.newName.empty()) return;

	const std::vector<std::string> keys =
		HcRenameSweep::classAndDescendants(*ctx.contentManager, t.classKey);

	// The two graphs the editor always holds open — the current scene's level
	// script and the app-wide Game Instance — are renamed HERE, in the copies
	// that are actually live. Letting the sweep write their files instead would
	// be worse than doing nothing: the stale copy in memory would be saved back
	// over it at the next Ctrl+S.
	std::vector<std::string> skip = handledInMemory;
	auto inMemory = [&](HorizonCode::Graph* g, const std::string& key, const char* what)
	{
		if (!g) return false;
		const HcRename::Plan p = HcRename::planGraph(*g, HcRename::Role::Other, keys, key,
		                                            "Game Instance", t);
		if (!HcRename::apply(*g, p, t)) return false;
		HE_LOG_INFO(Editor, "Rename: followed \"%s\" into the open %s.", t.oldName.c_str(), what);
		return true;
	};
	if (ctx.world && inMemory(&ctx.world->levelScript(), ctx.currentScenePath, "level script"))
		ctx.sceneDirty = true;
	if (inMemory(ctx.gameInstanceGraph, "Game Instance", "Game Instance") && ctx.commitGameInstance)
		ctx.commitGameInstance();
	if (!ctx.currentScenePath.empty()) skip.push_back(ctx.currentScenePath);

	HcRenameSweep::Report r = HcRenameSweep::scan(
		*ctx.contentManager, t, keys, skip,
		[&ctx](const std::string& rel) { return blockedBecause(ctx, rel); });

	if (!r.anything()) return;   // nothing else in the project names it: say nothing
	s.target = t;
	s.report = std::move(r);
	s.open   = true;
}

void Draw(AppContext& ctx)
{
	if (!s.open) return;
	HE::Ed::Help::Scope helpScope("Rename Across Project");

	const char* kTitle = "Rename across the project";
	if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
	ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

	ImGui::TextWrapped("The %s \"%s\" is now called \"%s\". Other graphs in this project name it too.",
	                   memberWord(s.target.member), s.target.oldName.c_str(), s.target.newName.c_str());

	std::vector<const HcRenameSweep::Entry*> write, skipped, unsure;
	for (const HcRenameSweep::Entry& e : s.report.entries)
	{
		if (e.writable())                                  write.push_back(&e);
		else if (e.plan.touches() && !e.skipWhy.empty())   skipped.push_back(&e);
		if (!e.plan.unsure.empty() || !e.plan.blocked.empty()) unsure.push_back(&e);
	}

	ImGui::Separator();
	drawList("Will be renamed", write, true, ImGui::GetColorU32(ImGuiCol_Text));
	drawList("Skipped", skipped, true, IM_COL32(235, 180, 90, 255));
	drawList("Names it, but nothing says whose — check these by hand", unsure, false,
	         IM_COL32(235, 120, 90, 255));

	if (s.report.incomplete)
	{
		ImGui::Spacing();
		ImGui::TextDisabled("The project could not be read to the end, so this list may be short.");
	}

	ImGui::Spacing();
	ImGui::TextDisabled("Undo only reaches the graph you are standing in. The files below are\n"
	                    "written and saved, so this is the moment to say no.");
	ImGui::Separator();

	const int assets = s.report.assetsToWrite();
	const int hits   = s.report.renameCount();
	char go[128];
	snprintf(go, sizeof go, "Rename in %d place%s across %d asset%s",
	         hits, hits == 1 ? "" : "s", assets, assets == 1 ? "" : "s");

	ImGui::BeginDisabled(assets == 0);
	if (EditorWidgets::primaryButton(go))
	{
		const int written = HcRenameSweep::apply(*ctx.contentManager, s.report, s.target);
		HE::Ed::notify(HE::Ed::NoteLevel::Info,
		               "Renamed \"" + s.target.oldName + "\" to \"" + s.target.newName + "\"",
		               "Rewritten in " + std::to_string(written) + " asset(s).");
		s.open = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	EditorWidgets::helpForLabel("Rename in place");
	ImGui::SameLine();
	if (EditorWidgets::cancelButton("Leave the others alone"))
	{
		s.open = false;
		ImGui::CloseCurrentPopup();
	}
	EditorWidgets::helpForLabel("Leave the others alone");

	ImGui::EndPopup();
}

} // namespace HcRenameDialog
