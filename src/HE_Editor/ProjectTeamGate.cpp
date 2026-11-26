#include "ProjectTeamGate.h"

#include "EditorHelp.h"      // "Project Hub/<label>" scope for the buttons
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "TeamSyncView.h"

#include <imgui.h>

namespace ProjectTeamGate
{

Result evaluate(const Input& in)
{
	using TS = TeamController::Status;
	using Phase = HE::Sc::TeamSyncService::Phase;
	Result r;
	auto fail = [&](std::string headline, std::string reason, bool unreachable) {
		r.state = State::Failed;
		r.headline = std::move(headline);
		r.reason = std::move(reason);
		r.unreachable = unreachable;
		return r;
	};
	const std::string team = "\"" + in.teamName + "\"";

	// ── The team connection ──
	if (in.team == TS::Failed)
		return fail("The team server could not be reached.",
		            in.teamError.empty() ? std::string("The connection was refused or timed out.") : in.teamError, true);
	if (in.team == TS::SignedOut)
		return fail("Not signed in to team " + team + ".",
		            "Sign in in the Team window to get the team's newest version.", false);
	if (in.team == TS::Connecting || in.team == TS::Reconnecting)
	{
		if (in.elapsedMs >= kConnectTimeoutMs)
			return fail("The team server could not be reached.",
			            in.teamError.empty() ? std::string("It did not answer in time.") : in.teamError, true);
		r.stage = "Signing in to team " + team;
		return r;
	}
	if (in.blocker == TeamSyncController::Blocker::OtherTeam)
		return fail("Signed in to another team.",
		            "This project's history is kept by team " + team + "; sign in to that team to update it.", false);

	// ── The sync ──
	const HE::Sc::TeamSyncService::Status& s = in.sync;
	if (!s.firstSyncDiffers.empty()) { r.state = State::AskUser; return r; }
	if (s.projectMissing)
	{
		if (s.isAdmin) { r.state = State::AskUser; return r; }
		return fail("The project is not on the team server.",
		            "Team " + team + " has no project \"" + in.projectName + "\" yet; a team admin creates it.", false);
	}
	if (s.phase == Phase::Offline)
		return fail("The project could not be updated from the team server.",
		            s.error.empty() ? std::string("The sync connection failed.") : s.error, true);
	if (in.syncRunning && s.connected && s.phase == Phase::Idle && s.activity.empty() &&
	    static_cast<std::int64_t>(s.lastSyncUnix) >= in.startUnix)
	{
		r.state = State::Ready;
		return r;
	}
	if (!s.connected && in.elapsedMs >= kConnectTimeoutMs)
		return fail("The project could not be updated from the team server.",
		            s.error.empty() ? std::string("The sync connection did not come up in time.") : s.error, true);
	r.stage = !s.activity.empty() ? s.activity
	        : s.connected        ? std::string("Comparing with the team's version")
	                             : std::string("Connecting to the team server");
	return r;
}

Answer drawWaiting(const std::string& projectName, const Result& r, TeamSyncController* sync)
{
	HE::Ed::Help::Scope helpScope("Project Hub");
	Answer answer = Answer::None;
	ImGui::Text("Opening %s", projectName.c_str());
	ImGui::Separator();
	ImGui::Spacing();
	if (r.state == State::AskUser && sync)
	{
		ImGui::TextUnformatted("The team server needs an answer before this project opens.");
		TeamSyncView::draw(*sync);
	}
	else
	{
		ImGui::TextUnformatted("Getting the newest version from the team server");
		ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(480.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextUnformatted(r.stage.empty() ? " " : r.stage.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::Spacing();
	if (EditorWidgets::button("Open Without Updating", ImVec2(200.0f, 0.0f))) answer = Answer::OpenAnyway;
	ImGui::SameLine();
	// Named for what it does, and the same pair as the failed state's.
	if (EditorWidgets::button("Close Project", ImVec2(150.0f, 0.0f))) answer = Answer::CloseProject;
	return answer;
}

Answer drawFailed(const std::string& projectName, const Result& r)
{
	HE::Ed::Help::Scope helpScope("Project Hub");
	Answer answer = Answer::None;
	ImGui::Text("\"%s\" could not be updated", projectName.c_str());
	ImGui::Separator();
	ImGui::Spacing();
	{
		EditorWidgets::WrapText wrap(ImGui::GetCursorPosX() + 500.0f);   // released before the buttons
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::Warn);
		ImGui::TextWrapped("%s", r.headline.c_str());
		ImGui::PopStyleColor();
		if (!r.reason.empty()) ImGui::TextWrapped("%s", r.reason.c_str());
		ImGui::Spacing();
		ImGui::TextWrapped("If you open it now, you work on this machine's copy, which may be behind "
		                   "the team's. Your saves go to the team server once it is reachable again, and "
		                   "the newest version comes then.");
	}
	ImGui::Spacing();
	if (EditorWidgets::primaryButton("Try Again", ImVec2(130.0f, 0.0f))) answer = Answer::TryAgain;
	ImGui::SameLine();
	if (EditorWidgets::button("Open Without Updating", ImVec2(200.0f, 0.0f))) answer = Answer::OpenAnyway;
	ImGui::SameLine();
	if (EditorWidgets::button("Close Project", ImVec2(150.0f, 0.0f))) answer = Answer::CloseProject;
	return answer;
}

} // namespace ProjectTeamGate
