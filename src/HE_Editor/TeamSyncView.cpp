#include "TeamSyncView.h"

#include "EditorHelp.h"      // "Team Sync/<label>" scope for the tooltips
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "TeamSyncController.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

namespace
{

// "3 min ago" for a unix time.
std::string agoText(std::int64_t unixTime)
{
	const auto now = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
	const std::int64_t s = unixTime > 0 && now > unixTime ? now - unixTime : 0;
	if (s < 60)    return "just now";
	if (s < 3600)  return std::to_string(s / 60) + " min ago";
	if (s < 86400) return std::to_string(s / 3600) + " h ago";
	return std::to_string(s / 86400) + " d ago";
}

// What the server did with one file, in words. The outcome names are the
// server's (CollabSession README, "Zusammenführen"); the detail says against whom.
std::string outcomeText(const std::string& outcome)
{
	if (outcome == "applied")        return "saved";
	if (outcome == "same")           return "already the team's";
	if (outcome == "merged")         return "merged with the team's changes";
	if (outcome == "merged-arrival") return "merged; where both changed the same thing, yours won";
	if (outcome == "replaced")       return "yours replaced the team's version";
	if (outcome == "delete-dropped") return "not deleted: somebody changed it since";
	if (outcome == "restored")       return "restored: somebody had deleted it";
	return outcome;
}

// Something of somebody's did not make it into the result.
bool outcomeLostSomething(const std::string& outcome)
{
	return outcome == "merged-arrival" || outcome == "replaced" || outcome == "delete-dropped";
}

} // namespace

void TeamSyncView::draw(TeamSyncController& sync)
{
	using Phase = HE::Sc::TeamSyncService::Phase;
	HE::Ed::Help::Scope helpScope("Team Sync");
	EditorWidgets::WrapText wrap;
	HE::Sc::TeamSyncService& svc = sync.service();
	const HE::Sc::TeamSyncService::Status s = svc.status();

	ImGui::Spacing();
	const std::string line = sync.stateLine();
	const bool trouble = sync.blocker() != TeamSyncController::Blocker::None || s.phase == Phase::Offline;
	ImGui::PushStyleColor(ImGuiCol_Text, trouble ? HE::Ed::Theme::Warn : HE::Ed::Theme::TextHeading);
	ImGui::TextWrapped("%s", line.c_str());
	ImGui::PopStyleColor();
	if (sync.blocker() != TeamSyncController::Blocker::None) return;
	if (!s.error.empty() && s.phase != Phase::Offline)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Warn);
		ImGui::TextWrapped("Last problem: %s", s.error.c_str());
		ImGui::PopStyleColor();
	}

	// ── What needs a person ──
	if (!s.firstSyncDiffers.empty())
	{
		ImGui::Spacing();
		ImGui::TextWrapped("This copy has never synced with the team, and these files differ "
		                   "from the team's version. Nothing has been uploaded or overwritten yet.");
		const std::size_t shown = std::min<std::size_t>(s.firstSyncDiffers.size(), 8);
		for (std::size_t i = 0; i < shown; ++i)
			ImGui::BulletText("%s", s.firstSyncDiffers[i].c_str());
		if (s.firstSyncDiffers.size() > shown)
			ImGui::TextDisabled("... and %zu more", s.firstSyncDiffers.size() - shown);
		if (EditorWidgets::button("Take the Team's Versions")) svc.resolveFirstSync(false);
		ImGui::SameLine();
		if (EditorWidgets::button("Keep Mine and Merge")) svc.resolveFirstSync(true);
	}
	if (s.projectMissing && s.isAdmin)
	{
		ImGui::Spacing();
		ImGui::TextWrapped("Create \"%s\" on the team server and upload this copy as its first "
		                   "revision. Everybody else then gets it from there.",
		                   sync.projectName().c_str());
		if (EditorWidgets::button("Create Project on Team Server")) svc.createProject();
	}

	ImGui::Spacing();
	ImGui::BeginDisabled(!sync.running() || s.paused || s.phase == Phase::Working ||
	                     !s.firstSyncDiffers.empty() || s.projectMissing);
	if (EditorWidgets::button("Sync Now")) sync.syncNow();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("Every save syncs by itself; this is for files changed outside the editor.");

	// ── The last save, per file ──
	const auto results = svc.lastResults();
	if (!results.empty())
	{
		ImGui::Spacing();
		ImGui::SeparatorText("Your last save");
		for (const auto& r : results)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, outcomeLostSomething(r.outcome) ? HE::Ed::Theme::Warn
			                                                                     : HE::Ed::Theme::TextHeading);
			ImGui::TextWrapped("%s: %s", r.path.c_str(), outcomeText(r.outcome).c_str());
			ImGui::PopStyleColor();
			if (!r.detail.empty()) ImGui::TextDisabled("    %s", r.detail.c_str());
		}
	}

	// ── Overruled versions of yours ──
	const auto lost = svc.lost();
	if (!lost.empty())
	{
		ImGui::Spacing();
		ImGui::SeparatorText("Where a later save won over yours");
		ImGui::TextDisabled("Your version is kept on the team server (branch horizon-conflicts).");
		int row = 0;
		for (const auto& l : lost)
		{
			ImGui::PushID(row++);
			ImGui::TextWrapped("%s: %s's save (r%llu) %s", l.path.c_str(), l.winner.c_str(),
			                   static_cast<unsigned long long>(l.rev),
			                   l.outcome == "replaced" ? "replaced your version"
			                                           : "won where you both changed the same thing");
			if (EditorWidgets::smallButton("Open My Version")) svc.fetchLost(l);
			ImGui::PopID();
		}
	}

	// ── The team's history ──
	const auto history = svc.history();
	if (!history.empty())
	{
		ImGui::Spacing();
		ImGui::SeparatorText("Team history");
		for (const auto& v : history)
		{
			char label[192];
			std::snprintf(label, sizeof(label), "r%llu  %s  %s##rev%llu",
			              static_cast<unsigned long long>(v.rev), v.member.c_str(),
			              agoText(v.time).c_str(), static_cast<unsigned long long>(v.rev));
			// The newest revision starts open: it is the one somebody just
			// looked here for.
			const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
			    (v.rev == history.front().rev ? ImGuiTreeNodeFlags_DefaultOpen : 0);
			if (ImGui::TreeNodeEx(label, flags))
			{
				ImGui::TextDisabled("%s", v.subject.c_str());
				for (const std::string& m : v.merges) ImGui::TextWrapped("%s", m.c_str());
				ImGui::TreePop();
			}
		}
	}

	ImGui::Spacing();
	ImGui::TextDisabled("The connection to the team server is not encrypted yet.");
}
