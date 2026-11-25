#include "TeamSyncView.h"

#include "EditorHelp.h"      // "Team Sync/<label>" scope for the tooltips
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "TeamController.h"
#include "TeamSyncController.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <unordered_map>

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

struct Noted
{
	TeamSyncView::Rect rect;
	int                frame = -1;
};
std::unordered_map<std::string, Noted> s_rects;

void note(const std::string& name)
{
	const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
	s_rects[name] = { { a.x, a.y, z.x, z.y }, ImGui::GetFrameCount() };
}

std::string s_projectField;   // the name typed in the switch popup
std::string s_switchError;

} // namespace

TeamSyncView::Rect TeamSyncView::controlRect(const std::string& name)
{
	// Only what the last frame drew: a closed popup's buttons are gone.
	const auto it = s_rects.find(name);
	return it == s_rects.end() || it->second.frame != ImGui::GetFrameCount() ? Rect{} : it->second.rect;
}

bool TeamSyncView::validServerName(const std::string& name)
{
	if (name.empty() || name.size() > 64) return false;
	for (const char c : name)
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
			return false;
	return true;
}

std::string TeamSyncView::serverProjectName(const std::string& projectName)
{
	std::string out;
	for (const char c : projectName)
	{
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
		if (ok) out += c;
		else if (!out.empty() && out.back() != '-') out += '-';   // "My Game 2" -> "My-Game-2"
	}
	while (!out.empty() && out.back() == '-') out.pop_back();
	if (out.size() > 64) out.resize(64);
	return out.empty() ? std::string("project") : out;
}

namespace
{
bool switchToTeamBody(const TeamController& team, const TeamSyncView::ApplyTeam& apply)
{
	HE::Ed::Help::Scope helpScope("Team Sync");
	EditorWidgets::WrapText wrap(ImGui::GetCursorPosX() + 420.0f);
	bool switched = false;
	ImGui::TextUnformatted("Keep this project's history on the team server");
	ImGui::Spacing();
	ImGui::TextWrapped("Every save goes to team \"%s\", which merges it with the others' saves: scenes "
	                   "entity by entity, settings key by key. Where two people changed the same thing, "
	                   "the later save wins and the other version is kept on the server.",
	                   team.team().c_str());
	ImGui::TextWrapped("The local git repository stays as it is and is not used while this is on. "
	                   "The setting is in Config/ProjectSettings.json, so everyone who gets the project "
	                   "switches with it.");
	ImGui::Spacing();
	ImGui::TextUnformatted("Project on the server");
	ImGui::SetNextItemWidth(260.0f);
	ImGui::InputText("##serverproject", &s_projectField);
	note("serverproject");
	const bool valid = TeamSyncView::validServerName(s_projectField);
	if (!valid)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Warn);
		ImGui::TextWrapped("Letters, digits, - and _ only, at most 64.");
		ImGui::PopStyleColor();
	}
	ImGui::TextDisabled("%s", team.isAdmin() ? "If it does not exist there yet, you can create it with this copy next."
	                                         : "A team admin creates the project on the server; ask one if it is not there yet.");
	if (!s_switchError.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Warn);
		ImGui::TextWrapped("%s", s_switchError.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::Spacing();
	ImGui::BeginDisabled(!valid);
	if (EditorWidgets::primaryButton("Switch to the Team Server"))
	{
		if (apply && apply(team.team(), s_projectField))
		{
			switched = true;
			ImGui::CloseCurrentPopup();
		}
		else s_switchError = "Config/ProjectSettings.json could not be written.";
	}
	note("Switch to the Team Server");
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (EditorWidgets::cancelButton("Cancel")) ImGui::CloseCurrentPopup();
	return switched;
}

bool switchToGitBody(const std::function<bool()>& apply)
{
	HE::Ed::Help::Scope helpScope("Team Sync");
	EditorWidgets::WrapText wrap(ImGui::GetCursorPosX() + 400.0f);
	bool switched = false;
	ImGui::TextUnformatted("Use git for this project again?");
	ImGui::TextWrapped("Saves stop going to the team server, and the local git repository picks up where "
	                   "it stopped at the switch: what changed since shows as changes to commit. The team "
	                   "server keeps its history. Everyone who gets this project's settings switches too.");
	if (EditorWidgets::dangerButton("Switch to Git"))
	{
		if (apply && apply()) switched = true;
		ImGui::CloseCurrentPopup();
	}
	note("Switch to Git");
	ImGui::SameLine();
	if (EditorWidgets::cancelButton("Cancel")) ImGui::CloseCurrentPopup();
	return switched;
}
} // namespace

bool TeamSyncView::drawSwitchToTeam(const TeamController* team, const std::string& projectName, bool locked,
                                    const ApplyTeam& apply)
{
	if (!team || !team->signedIn() || team->team().empty()) return false;
	HE::Ed::Help::Scope helpScope("Team Sync");
	bool switched = false;
	{
		EditorWidgets::WrapText wrap;   // released before anything else is drawn
		ImGui::TextDisabled("Team server \"%s\" can keep this project's history instead of git: it merges "
		                    "everyone's saves as they happen.", team->team().c_str());
	}
	if (locked) ImGui::BeginDisabled();
	if (EditorWidgets::button("Use the Team Server..."))
	{
		s_projectField = serverProjectName(projectName);
		s_switchError.clear();
		ImGui::OpenPopup("##toteam");
	}
	note("Use the Team Server...");
	if (locked) ImGui::EndDisabled();
	ImGui::Separator();

	if (ImGui::BeginPopup("##toteam"))
	{
		switched = switchToTeamBody(*team, apply);   // its text wrap ends inside, before EndPopup
		ImGui::EndPopup();
	}
	return switched;
}

bool TeamSyncView::drawSwitchToGit(bool locked, const std::function<bool()>& apply)
{
	HE::Ed::Help::Scope helpScope("Team Sync");
	bool switched = false;
	ImGui::Spacing();
	ImGui::Separator();
	if (locked) ImGui::BeginDisabled();
	if (EditorWidgets::button("Switch Back to Git...")) ImGui::OpenPopup("##togit");
	note("Switch Back to Git...");
	if (locked) ImGui::EndDisabled();
	if (ImGui::BeginPopup("##togit"))
	{
		switched = switchToGitBody(apply);   // its text wrap ends inside, before EndPopup
		ImGui::EndPopup();
	}
	return switched;
}

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
