#include "TeamSyncView.h"

#include "EditorHelp.h"      // "Team Sync/<label>" scope for the tooltips
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include "TeamController.h"
#include "TeamSyncController.h"

#include "EditorToolbar.h"   // the icons the git view uses

#include <SourceControl/TextMerge.h>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

namespace
{
namespace T = EditorToolbar;
using Icon = void (*)(ImDrawList*, const ImVec2&, float, ImU32);

// A section title: icon, name, count, hairline to the right edge.
void sectionHeader(Icon icon, ImU32 iconCol, const char* title, std::size_t count = 0)
{
	ImGui::Spacing();
	ImGui::Spacing();
	const float h = ImGui::GetFrameHeight();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	icon(dl, ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.62f, iconCol);
	const std::string label = count ? std::string(title) + "  " + std::to_string(count) : std::string(title);
	dl->AddText(ImVec2(p.x + h + 4.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f), HE::Ed::Theme::u32(HE::Ed::Theme::TextHeading), label.c_str());
	const float tx = p.x + h + 12.0f + ImGui::CalcTextSize(label.c_str()).x;
	if (tx < p.x + w) dl->AddLine(ImVec2(tx, p.y + h * 0.5f), ImVec2(p.x + w, p.y + h * 0.5f), HE::Ed::Theme::u32(HE::Ed::Theme::warm(0.24f)));
	ImGui::Dummy(ImVec2(w, h));
}

// A row: icon, the file's name, its folder dim beside it, a dim note at the right.
void fileRow(Icon icon, ImU32 iconCol, const std::string& path, const std::string& note, ImU32 noteCol)
{
	const float h = ImGui::GetFrameHeight();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const std::size_t slash = path.find_last_of('/');
	const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	const std::string dir  = slash == std::string::npos ? std::string() : path.substr(0, slash);
	icon(dl, ImVec2(p.x + 10.0f + h * 0.5f, p.y + h * 0.5f), h * 0.55f, iconCol);
	const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
	const float noteW = ImGui::CalcTextSize(note.c_str()).x;
	const float right = p.x + w - noteW - 6.0f;
	dl->PushClipRect(p, ImVec2(std::max(p.x, right - 8.0f), p.y + h), true);
	float x = p.x + h + 16.0f;
	dl->AddText(ImVec2(x, ty), HE::Ed::Theme::u32(HE::Ed::Theme::Text), name.c_str());
	x += ImGui::CalcTextSize(name.c_str()).x + 8.0f;
	if (!dir.empty()) dl->AddText(ImVec2(x, ty), HE::Ed::Theme::u32(HE::Ed::Theme::TextDim), dir.c_str());
	dl->PopClipRect();
	dl->AddText(ImVec2(right, ty), noteCol, note.c_str());
	ImGui::Dummy(ImVec2(w, h));
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
}

// A member's initial in a circle of a colour of their own.
void memberBadge(ImDrawList* dl, ImVec2 c, float r, const std::string& member)
{
	std::uint32_t hsh = 2166136261u;
	for (const char ch : member) hsh = (hsh ^ static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(ch)))) * 16777619u;
	float rr = 0, gg = 0, bb = 0;
	ImGui::ColorConvertHSVtoRGB(static_cast<float>(hsh % 360u) / 360.0f, 0.45f, 0.75f, rr, gg, bb);
	dl->AddCircleFilled(c, r, ImGui::GetColorU32(ImVec4(rr, gg, bb, 1.0f)));
	const char initial[2] = { member.empty() ? '?' : static_cast<char>(std::toupper(static_cast<unsigned char>(member[0]))), 0 };
	const ImVec2 ts = ImGui::CalcTextSize(initial);
	dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(20, 18, 16, 255), initial);
}

Icon outcomeIcon(const std::string& outcome)
{
	if (outcome == "merged" || outcome == "merged-arrival") return T::iconBranch;
	if (outcome == "replaced" || outcome == "delete-dropped") return T::iconWarning;
	if (outcome == "restored") return T::iconHistory;
	return T::iconCheck;
}

// ── The conflict window's state ──
struct ConflictView
{
	bool                         open = false;
	HE::Sc::TeamSyncService::Lost lost;
	bool                         fetchAsked = false;
	bool                         loaded = false;
	bool                         wholeFile = false;   // binary or a graph: one side or the other
	std::string                  mine, theirs;
	HE::Sc::TextDiff             diff;
	std::vector<HE::Sc::Pick>    picks;
	std::string                  error;
};
ConflictView s_conflict;
bool         s_windowRequested = false;
std::uint64_t s_restoreRev = 0;

bool readAll(const std::filesystem::path& p, std::string& out)
{
	std::ifstream in(p, std::ios::binary);
	if (!in) return false;
	out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return true;
}

// Graph formats and assets merge whole on the server too: a line picked from each side of a node graph is no graph.
bool wholeFileByName(const std::string& path)
{
	static const char* const kWhole[] = { ".hasset", ".hmat", ".hmatfn", ".hcode", ".hpart", ".hasm", ".huiw" };
	for (const char* e : kWhole)
	{
		const std::size_t n = std::strlen(e);
		if (path.size() >= n && path.compare(path.size() - n, n, e) == 0) return true;
	}
	return false;
}
} // namespace

void TeamSyncView::openConflict(const HE::Sc::TeamSyncService::Lost& lost)
{
	s_conflict = ConflictView{};
	s_conflict.open = true;
	s_conflict.lost = lost;
	s_windowRequested = true;
}

bool TeamSyncView::takeWindowRequest()
{
	const bool r = s_windowRequested;
	s_windowRequested = false;
	return r;
}

bool TeamSyncView::conflictOpen() { return s_conflict.open; }

std::string TeamSyncView::conflictResult()
{
	return s_conflict.wholeFile ? std::string() : s_conflict.diff.merge(s_conflict.picks);
}

void TeamSyncView::setConflictPicks(const std::vector<HE::Sc::Pick>& picks) { s_conflict.picks = picks; }

bool TeamSyncView::resultIsValid(const std::string& mine, const std::string& theirs, const std::string& result)
{
	// JSON on either side (scenes, settings, planner files): the result must be JSON too.
	if (nlohmann::json::accept(mine) || nlohmann::json::accept(theirs)) return nlohmann::json::accept(result);
	return true;
}

namespace
{
std::string s_newBranch;

// The branch this editor works on, and the way to another one, a new one, or back into the project.
void drawBranchBar(TeamSyncController& sync)
{
	using namespace HE::Ed;
	HE::Sc::TeamSyncService& svc = sync.service();
	const std::string current = sync.branch();
	const std::string label = current.empty() ? sync.projectName() + " (main)" : current;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float fh = ImGui::GetFrameHeight();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	T::iconBranch(dl, ImVec2(p.x + fh * 0.5f, p.y + fh * 0.5f), fh * 0.6f, Theme::u32(current.empty() ? Theme::TextDim : Theme::Accent));
	ImGui::Dummy(ImVec2(fh, fh));
	ImGui::SameLine(0, 4);
	if (EditorWidgets::button(("Branch: " + label + "##branchbtn").c_str()))
	{
		svc.refreshBranches();
		s_newBranch.clear();
		ImGui::OpenPopup("##branches");
	}
	EditorWidgets::helpForKey("Team Sync/Branch");
	note("Branch");
	if (sync.merging())
	{
		ImGui::SameLine();
		ImGui::TextDisabled("merging %s...", current.c_str());
	}
	if (!current.empty())
	{
		ImGui::SameLine();
		ImGui::BeginDisabled(!sync.canSwitchBranch());
		if (EditorWidgets::button(("Merge into " + sync.projectName() + "##merge").c_str())) sync.mergeIntoMain();
		EditorWidgets::helpForKey("Team Sync/Merge into Project");
		note("Merge into Project");
		ImGui::EndDisabled();
	}

	if (!ImGui::BeginPopup("##branches")) return;
	const bool may = sync.canSwitchBranch();
	if (ImGui::Selectable((sync.projectName() + " (main)").c_str(), current.empty(), may ? 0 : ImGuiSelectableFlags_Disabled))
		sync.switchBranch("");
	note("branch:");
	for (const HE::Sc::TeamSyncService::Branch& b : svc.branches())
	{
		const std::string row = b.name + "##" + b.name;
		if (ImGui::Selectable(row.c_str(), b.name == current, may ? 0 : ImGuiSelectableFlags_Disabled))
			sync.switchBranch(b.name);
		note("branch:" + b.name);
		ImGui::SameLine(220.0f);
		ImGui::TextDisabled("from r%llu by %s", static_cast<unsigned long long>(b.forkRev), b.creator.c_str());
	}
	if (!may)
	{
		ImGui::Spacing();
		ImGui::TextDisabled("%s", sync.inSession() ? "Not during a live session."
		                                          : "Switching waits until your changes are on the team server.");
	}
	if (current.empty())
	{
		ImGui::Separator();
		ImGui::TextUnformatted("New branch from the current state");
		ImGui::SetNextItemWidth(200.0f);
		ImGui::InputTextWithHint("##newbranch", "name", &s_newBranch);
		note("newbranch");
		const bool valid = TeamSyncView::validServerName(s_newBranch) && s_newBranch.find("--") == std::string::npos &&
		                   s_newBranch.size() <= 48;
		ImGui::SameLine();
		ImGui::BeginDisabled(!valid || !may);
		if (EditorWidgets::primaryButton("Create Branch"))
		{
			sync.createBranch(s_newBranch);
			ImGui::CloseCurrentPopup();
		}
		note("Create Branch");
		ImGui::EndDisabled();
		ImGui::TextDisabled("Your saves then go to the branch; the others keep working on %s.", sync.projectName().c_str());
	}
	ImGui::EndPopup();
}
} // namespace

void TeamSyncView::draw(TeamSyncController& sync)
{
	using Phase = HE::Sc::TeamSyncService::Phase;
	using namespace HE::Ed;
	HE::Ed::Help::Scope helpScope("Team Sync");
	HE::Sc::TeamSyncService& svc = sync.service();
	const HE::Sc::TeamSyncService::Status s = svc.status();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float fh = ImGui::GetFrameHeight();

	// ── Where the project stands: a card with an icon, the state, the sync button ──
	{
		const bool blocked = sync.blocker() != TeamSyncController::Blocker::None;
		const bool trouble = blocked || s.phase == Phase::Offline || !s.error.empty();
		const bool working = s.phase == Phase::Working || s.phase == Phase::Connecting || s.phase == Phase::WaitingTicket;
		const bool conflicts = !svc.lost().empty();
		const ImVec4 tone = trouble ? Theme::Warn : conflicts ? Theme::Warn : working ? Theme::Accent : Theme::Ok;
		Icon icon = trouble || conflicts ? T::iconWarning : working ? T::iconRefresh : T::iconCheck;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		const float cardH = fh * 2.6f;
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + cardH), Theme::u32(Theme::warm(0.13f)), 8.0f);
		dl->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + cardH), Theme::u32(tone), 8.0f, ImDrawFlags_RoundCornersLeft);
		const ImVec2 ic(p.x + 26.0f, p.y + cardH * 0.5f);
		dl->AddCircleFilled(ic, fh * 0.62f, Theme::u32(Theme::alpha(tone, 0.18f)));
		icon(dl, ic, fh * 0.62f, Theme::u32(tone));
		const std::string line = sync.stateLine();
		const std::string sub = blocked ? std::string()
		    : "Team " + sync.team() + "  \xC2\xB7  " + sync.projectName() + "  \xC2\xB7  r" + std::to_string(s.rev) +
		      (conflicts ? "  \xC2\xB7  " + std::to_string(svc.lost().size()) + " to resolve" : std::string());
		const float textX = p.x + 50.0f;
		const float btnW = ImGui::CalcTextSize("Sync Now").x + ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
		dl->PushClipRect(p, ImVec2(p.x + w - btnW - 12.0f, p.y + cardH), true);
		dl->AddText(ImVec2(textX, p.y + cardH * 0.5f - ImGui::GetFontSize() - 1.0f), Theme::u32(trouble ? Theme::Warn : Theme::TextHeading), line.c_str());
		dl->AddText(ImVec2(textX, p.y + cardH * 0.5f + 2.0f), Theme::u32(Theme::TextDim), sub.c_str());
		dl->PopClipRect();
		ImGui::SetCursorScreenPos(ImVec2(p.x + w - btnW - 10.0f, p.y + (cardH - fh) * 0.5f));
		ImGui::BeginDisabled(!sync.running() || s.paused || s.phase == Phase::Working ||
		                     !s.firstSyncDiffers.empty() || s.projectMissing);
		if (EditorWidgets::button("Sync Now")) sync.syncNow();
		note("Sync Now");
		ImGui::EndDisabled();
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + cardH));
		ImGui::Dummy(ImVec2(w, 4.0f));
		if (blocked) return;
		drawBranchBar(sync);
		if (!s.error.empty() && s.phase != Phase::Offline)
		{
			EditorWidgets::WrapText wrap;
			ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warn);
			ImGui::TextWrapped("Last problem: %s", s.error.c_str());
			ImGui::PopStyleColor();
		}
	}

	// ── What needs a person ──
	if (!s.firstSyncDiffers.empty())
	{
		sectionHeader(T::iconWarning, Theme::u32(Theme::Warn), "First sync");
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextWrapped("This copy has never synced with the team, and these files differ "
			                   "from the team's version. Nothing has been uploaded or overwritten yet.");
		}
		const std::size_t shown = std::min<std::size_t>(s.firstSyncDiffers.size(), 8);
		for (std::size_t i = 0; i < shown; ++i)
			fileRow(T::iconWarning, Theme::u32(Theme::Warn), s.firstSyncDiffers[i], "differs", Theme::u32(Theme::TextDim));
		if (s.firstSyncDiffers.size() > shown)
			ImGui::TextDisabled("... and %zu more", s.firstSyncDiffers.size() - shown);
		if (EditorWidgets::button("Take the Team's Versions")) svc.resolveFirstSync(false);
		ImGui::SameLine();
		if (EditorWidgets::button("Keep Mine and Merge")) svc.resolveFirstSync(true);
	}
	if (s.projectMissing && s.isAdmin)
	{
		sectionHeader(T::iconCloud, Theme::u32(Theme::Accent), "Not on the team server yet");
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextWrapped("Create \"%s\" on the team server and upload this copy as its first "
			                   "revision. Everybody else then gets it from there.",
			                   sync.projectName().c_str());
		}
		if (EditorWidgets::button("Create Project on Team Server")) svc.createProject();
	}

	// ── Conflicts: both versions are on the server, a person picks ──
	const auto lost = svc.lost();
	if (!lost.empty())
	{
		sectionHeader(T::iconWarning, Theme::u32(Theme::Warn), "To resolve", lost.size());
		int row = 0;
		for (const auto& l : lost)
		{
			ImGui::PushID(row++);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			const float rowH = fh * 1.9f;
			dl->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH - 3.0f), Theme::u32(Theme::alpha(Theme::Warn, 0.07f)), 6.0f);
			T::iconWarning(dl, ImVec2(p.x + 18.0f, p.y + rowH * 0.5f - 1.5f), fh * 0.6f, Theme::u32(Theme::Warn));
			const std::string name = std::filesystem::path(l.path).filename().string();
			const std::string what = l.winner + "'s save (r" + std::to_string(l.rev) + ") " +
			    (l.outcome == "replaced" ? "replaced yours (r" : "won over yours where you both changed it (r") +
			    std::to_string(l.ourRev) + ")";
			const float btnW = ImGui::CalcTextSize("Resolve").x + 24.0f;
			dl->PushClipRect(p, ImVec2(p.x + w - btnW - 14.0f, p.y + rowH), true);
			dl->AddText(ImVec2(p.x + 36.0f, p.y + 5.0f), Theme::u32(Theme::Text), name.c_str());
			dl->AddText(ImVec2(p.x + 36.0f, p.y + 6.0f + ImGui::GetFontSize()), Theme::u32(Theme::TextDim), what.c_str());
			dl->PopClipRect();
			ImGui::SetCursorScreenPos(ImVec2(p.x + w - btnW - 8.0f, p.y + (rowH - 3.0f - fh) * 0.5f));
			if (EditorWidgets::primaryButton("Resolve", ImVec2(btnW, 0))) openConflict(l);
			note("Resolve:" + l.path);
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
			ImGui::Dummy(ImVec2(w, 1.0f));
			ImGui::PopID();
		}
	}

	// ── The last save, per file ──
	const auto results = svc.lastResults();
	if (!results.empty())
	{
		sectionHeader(T::iconArrowUp, Theme::u32(Theme::Accent), "Your last save", results.size());
		for (const auto& r : results)
		{
			const bool lostSome = outcomeLostSomething(r.outcome);
			fileRow(outcomeIcon(r.outcome), Theme::u32(lostSome ? Theme::Warn : r.outcome == "same" ? Theme::TextDim : Theme::Ok),
			        r.path, outcomeText(r.outcome), Theme::u32(lostSome ? Theme::Warn : Theme::TextDim));
		}
	}

	// ── The team's history: a timeline, newest first, each revision restorable ──
	const auto history = svc.history();
	if (!history.empty())
	{
		sectionHeader(T::iconHistory, Theme::u32(Theme::Accent), "Team history");
		const bool mayRestore = sync.running() && !s.paused && !sync.inSession() && s.firstSyncDiffers.empty() &&
		                        s.phase != Phase::Working;
		static std::uint64_t s_expanded = 0;
		for (std::size_t i = 0; i < history.size(); ++i)
		{
			const auto& v = history[i];
			ImGui::PushID(static_cast<int>(v.rev));
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			const float rowH = fh * 1.9f;
			const bool special = v.subject.rfind("Restore", 0) == 0 || v.subject.rfind("Resolve", 0) == 0;
			const ImU32 lineCol = Theme::u32(Theme::warm(0.30f));
			const float lx = p.x + 14.0f;
			if (i > 0) dl->AddLine(ImVec2(lx, p.y), ImVec2(lx, p.y + rowH * 0.5f), lineCol, 2.0f);
			if (i + 1 < history.size()) dl->AddLine(ImVec2(lx, p.y + rowH * 0.5f), ImVec2(lx, p.y + rowH), lineCol, 2.0f);
			const ImVec2 dot(lx, p.y + rowH * 0.5f);
			if (v.rev == s.rev) dl->AddCircleFilled(dot, 6.0f, Theme::u32(Theme::Accent));
			else                dl->AddCircleFilled(dot, 4.5f, Theme::u32(special ? Theme::Warn : Theme::TextDim));
			memberBadge(dl, ImVec2(p.x + 42.0f, p.y + rowH * 0.5f), fh * 0.5f, v.member);

			const bool clicked = ImGui::InvisibleButton("##rev", ImVec2(w, rowH));
			const bool hovered = ImGui::IsItemHovered();
			if (hovered) dl->AddRectFilled(ImVec2(p.x + 26.0f, p.y + 1.0f), ImVec2(p.x + w, p.y + rowH - 1.0f), IM_COL32(255, 255, 255, 14), 6.0f);
			if (clicked) s_expanded = s_expanded == v.rev ? 0 : v.rev;
			const std::string meta = "r" + std::to_string(v.rev) + "  \xC2\xB7  " + v.member + "  \xC2\xB7  " + agoText(v.time) +
			    (v.rev == s.rev ? std::string("  \xC2\xB7  this copy") : std::string());
			const float restoreW = fh;
			dl->PushClipRect(p, ImVec2(p.x + w - restoreW - 8.0f, p.y + rowH), true);
			// The server's "Save by X (rev N)" repeats the line under it: say what was saved instead.
			const std::string title = v.subject.rfind("Save by ", 0) == 0
			    ? "Saved " + std::to_string(v.merges.size()) + (v.merges.size() == 1 ? " file" : " files")
			    : v.subject;
			dl->AddText(ImVec2(p.x + 60.0f, p.y + 5.0f), Theme::u32(v.rev == s.rev ? Theme::TextHeading : Theme::Text), title.c_str());
			dl->AddText(ImVec2(p.x + 60.0f, p.y + 6.0f + ImGui::GetFontSize()), Theme::u32(Theme::TextDim), meta.c_str());
			dl->PopClipRect();
			// Restore, on the row under the mouse (never the revision the project already is).
			if ((hovered || ImGui::IsPopupOpen("##restore")) && v.rev != s.rev && mayRestore)
			{
				ImGui::SetCursorScreenPos(ImVec2(p.x + w - restoreW - 4.0f, p.y + (rowH - restoreW) * 0.5f));
				if (ImGui::InvisibleButton("##restorebtn", ImVec2(restoreW, restoreW)))
				{
					s_restoreRev = v.rev;
					ImGui::OpenPopup("##restore");
				}
				const bool hb = ImGui::IsItemHovered();
				EditorWidgets::helpForKey("Team Sync/Restore");
				const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
				if (hb) dl->AddRectFilled(a, z, IM_COL32(255, 255, 255, 30), 4.0f);
				T::iconHistory(dl, ImVec2((a.x + z.x) * 0.5f, (a.y + z.y) * 0.5f), restoreW * 0.6f, Theme::u32(hb ? Theme::AccentBright : Theme::Text));
				note("Restore:r" + std::to_string(v.rev));
			}
			if (ImGui::BeginPopup("##restore"))
			{
				{
					EditorWidgets::WrapText wrap(ImGui::GetCursorPosX() + 360.0f);
					ImGui::Text("Restore the project to r%llu?", static_cast<unsigned long long>(s_restoreRev));
					ImGui::TextWrapped("Every file goes back to how it was then, files added since are removed, and the "
					                   "result is saved for the team as a new revision. Nothing in the history is lost: "
					                   "you can restore the current state again from here.");
				}
				if (EditorWidgets::dangerButton("Restore")) { svc.restoreRevision(s_restoreRev); ImGui::CloseCurrentPopup(); }
				note("Restore");
				ImGui::SameLine();
				if (EditorWidgets::cancelButton("Cancel")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
			ImGui::Dummy(ImVec2(w, 0.0f));
			if (s_expanded == v.rev)
				for (const std::string& m : v.merges)
				{
					std::string outcome, path, detail;
					if (!HE::Sc::TeamSyncService::parseMerge(m, outcome, path, detail)) continue;
					ImGui::Indent(44.0f);
					fileRow(outcomeIcon(outcome), Theme::u32(outcomeLostSomething(outcome) ? Theme::Warn : Theme::TextDim), path,
					        outcomeText(outcome), Theme::u32(Theme::TextDim));
					ImGui::Unindent(44.0f);
				}
			ImGui::PopID();
		}
	}

	ImGui::Spacing();
	ImGui::TextDisabled("The connection to the team server is not encrypted yet.");
}

void TeamSyncView::drawConflictWindow(TeamSyncController& sync)
{
	using namespace HE::Ed;
	ConflictView& c = s_conflict;
	if (!c.open) return;
	HE::Ed::Help::Scope helpScope("Team Sync");
	HE::Sc::TeamSyncService& svc = sync.service();
	const std::filesystem::path mineFile = svc.lostCopyPath(c.lost);
	const std::filesystem::path theirsFile = sync.projectRoot() / c.lost.path;

	// Our version comes from the server once; theirs is the project's file.
	if (!c.loaded)
	{
		std::error_code ec;
		if (!std::filesystem::exists(mineFile, ec) && !c.fetchAsked)
		{
			svc.fetchLost(c.lost, /*reveal=*/false);
			c.fetchAsked = true;
		}
		if (std::filesystem::exists(mineFile, ec))
		{
			readAll(mineFile, c.mine);
			readAll(theirsFile, c.theirs);   // missing: deleted since, an empty text
			c.wholeFile = wholeFileByName(c.lost.path) || HE::Sc::looksBinary(c.mine) || HE::Sc::looksBinary(c.theirs);
			if (!c.wholeFile)
			{
				c.diff = HE::Sc::diffText(c.mine, c.theirs);
				c.picks.assign(c.diff.changes(), HE::Sc::Pick::Theirs);
			}
			c.loaded = true;
		}
	}

	ImGui::SetNextWindowSize(ImVec2(860.0f, 580.0f), ImGuiCond_FirstUseEver);
	bool open = true;
	if (!ImGui::Begin("Resolve Conflict###teamconflict", &open, ImGuiWindowFlags_NoCollapse))
	{
		ImGui::End();
		if (!open) c = ConflictView{};
		return;
	}
	EditorWidgets::clampCurrentWindowToEditorWindow();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float fh = ImGui::GetFrameHeight();
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		T::iconWarning(dl, ImVec2(p.x + fh * 0.5f, p.y + fh * 0.5f), fh * 0.7f, Theme::u32(Theme::Warn));
		ImGui::Dummy(ImVec2(fh, fh));
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(c.lost.path.c_str());
	}
	ImGui::TextDisabled("%s's save (r%llu) won over yours (r%llu). The team server keeps both versions.",
	                    c.lost.winner.c_str(), static_cast<unsigned long long>(c.lost.rev),
	                    static_cast<unsigned long long>(c.lost.ourRev));
	ImGui::Separator();

	bool resolveWith = false, keepTheirs = false;
	std::string result;
	if (!c.loaded)
	{
		ImGui::Spacing();
		ImGui::TextUnformatted("Getting your version from the team server...");
	}
	else if (c.wholeFile)
	{
		// One side or the other: two cards.
		ImGui::Spacing();
		{
			EditorWidgets::WrapText wrap;
			ImGui::TextWrapped("This kind of file is taken whole (a graph or an asset): pick the version to keep.");
		}
		ImGui::Spacing();
		const float w = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
		auto card = [&](const char* title, const std::string& who, const std::string& bytes, ImVec4 tone) {
			ImGui::BeginGroup();
			const ImVec2 p = ImGui::GetCursorScreenPos();
			dl->AddRectFilled(p, ImVec2(p.x + w, p.y + fh * 3.2f), Theme::u32(Theme::alpha(tone, 0.10f)), 8.0f);
			dl->AddText(ImVec2(p.x + 12.0f, p.y + 8.0f), Theme::u32(Theme::TextHeading), title);
			dl->AddText(ImVec2(p.x + 12.0f, p.y + 10.0f + ImGui::GetFontSize()), Theme::u32(Theme::TextDim), who.c_str());
			const std::string size = std::to_string(bytes.size()) + " bytes";
			dl->AddText(ImVec2(p.x + 12.0f, p.y + 12.0f + ImGui::GetFontSize() * 2.0f), Theme::u32(Theme::TextDim), size.c_str());
			ImGui::Dummy(ImVec2(w, fh * 3.2f));
			ImGui::EndGroup();
		};
		card("Yours", "your save, r" + std::to_string(c.lost.ourRev), c.mine, Theme::Accent);
		ImGui::SameLine(0, 12.0f);
		card("Theirs (in the project now)", c.lost.winner + "'s save, r" + std::to_string(c.lost.rev), c.theirs, Theme::Ok);
		ImGui::Spacing();
		if (EditorWidgets::primaryButton("Use Yours")) { resolveWith = true; result = c.mine; }
		note("Use Yours");
		ImGui::SameLine();
		if (EditorWidgets::button("Keep Theirs")) keepTheirs = true;
		note("Keep Theirs");
	}
	else
	{
		// Line blocks: unchanged runs folded, each changed block with its pick.
		const std::size_t changes = c.diff.changes();
		if (EditorWidgets::button("All Yours")) c.picks.assign(changes, HE::Sc::Pick::Ours);
		note("All Yours");
		ImGui::SameLine();
		if (EditorWidgets::button("All Theirs")) c.picks.assign(changes, HE::Sc::Pick::Theirs);
		note("All Theirs");
		ImGui::SameLine();
		std::size_t ours = 0;
		for (const HE::Sc::Pick pk : c.picks) ours += pk != HE::Sc::Pick::Theirs ? 1 : 0;
		ImGui::TextDisabled("%zu change%s, %zu taken from yours", changes, changes == 1 ? "" : "s", ours);

		const float footer = fh * 2.6f;
		{
			// Column heads over the two sides.
			const float pad = ImGui::GetStyle().WindowPadding.x;
			const float headW = (ImGui::GetContentRegionAvail().x - 10.0f - pad * 2.0f) * 0.5f;
			const ImVec2 hp = ImGui::GetCursorScreenPos();
			const std::string yours = "Yours (r" + std::to_string(c.lost.ourRev) + ")";
			const std::string theirs = c.lost.winner + "'s, in the project now (r" + std::to_string(c.lost.rev) + ")";
			dl->AddText(ImVec2(hp.x + pad + 6.0f, hp.y), Theme::u32(Theme::Bad), yours.c_str());
			dl->AddText(ImVec2(hp.x + pad + headW + 16.0f, hp.y), Theme::u32(Theme::Ok), theirs.c_str());
			ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight() + 2.0f));
		}
		ImGui::BeginChild("##blocks", ImVec2(0, -footer), ImGuiChildFlags_Borders);
		ImDrawList* bl = ImGui::GetWindowDrawList();
		const float colW = (ImGui::GetContentRegionAvail().x - 10.0f) * 0.5f;
		const float lineH = ImGui::GetTextLineHeight();
		std::size_t change = 0;
		for (std::size_t bi = 0; bi < c.diff.blocks.size(); ++bi)
		{
			const HE::Sc::DiffBlock& b = c.diff.blocks[bi];
			ImGui::PushID(static_cast<int>(bi));
			if (b.same)
			{
				// Context: the last two lines before a change and the first two after, the rest folded.
				const std::size_t n = b.theirs.size();
				auto line = [&](const std::string& t) { ImGui::TextDisabled("  %s", t.c_str()); };
				if (n <= 5) for (const std::string& t : b.theirs) line(t);
				else
				{
					if (bi > 0) { line(b.theirs[0]); line(b.theirs[1]); }
					ImGui::TextDisabled("  ... %zu unchanged lines ...", n - (bi > 0 ? 2 : 0) - (bi + 1 < c.diff.blocks.size() ? 2 : 0));
					if (bi + 1 < c.diff.blocks.size()) { line(b.theirs[n - 2]); line(b.theirs[n - 1]); }
				}
				ImGui::PopID();
				continue;
			}
			const std::size_t k = change++;
			HE::Sc::Pick& pick = c.picks[k];
			// The pick: Yours / Theirs / Both.
			const char* const names[] = { "Yours", "Theirs", "Both" };
			const HE::Sc::Pick values[] = { HE::Sc::Pick::Ours, HE::Sc::Pick::Theirs, HE::Sc::Pick::Both };
			for (int i = 0; i < 3; ++i)
			{
				if (i > 0) ImGui::SameLine(0, 2);
				const bool on = pick == values[i];
				ImGui::PushStyleColor(ImGuiCol_Button, on ? Theme::alpha(Theme::Accent, 0.35f) : Theme::warm(0.18f));
				if (ImGui::SmallButton((std::string(names[i]) + "##pick").c_str())) pick = values[i];
				EditorWidgets::helpForKey("Team Sync/Pick");
				ImGui::PopStyleColor();
				note("pick:" + std::to_string(k) + ":" + names[i]);
			}
			// Two columns: yours left, theirs right; what is not taken is dimmed.
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const std::size_t rows = std::max<std::size_t>(std::max(b.ours.size(), b.theirs.size()), 1);
			const float h = static_cast<float>(rows) * lineH + 6.0f;
			const bool takeOurs = pick != HE::Sc::Pick::Theirs, takeTheirs = pick != HE::Sc::Pick::Ours;
			bl->AddRectFilled(p, ImVec2(p.x + colW, p.y + h), Theme::u32(Theme::alpha(Theme::Bad, takeOurs ? 0.20f : 0.06f)), 4.0f);
			bl->AddRectFilled(ImVec2(p.x + colW + 10.0f, p.y), ImVec2(p.x + colW * 2.0f + 10.0f, p.y + h),
			                  Theme::u32(Theme::alpha(Theme::Ok, takeTheirs ? 0.20f : 0.06f)), 4.0f);
			bl->PushClipRect(p, ImVec2(p.x + colW - 4.0f, p.y + h), true);
			for (std::size_t r = 0; r < b.ours.size(); ++r)
				bl->AddText(ImVec2(p.x + 6.0f, p.y + 3.0f + lineH * static_cast<float>(r)), Theme::u32(takeOurs ? Theme::Text : Theme::TextDim), b.ours[r].c_str());
			if (b.ours.empty()) bl->AddText(ImVec2(p.x + 6.0f, p.y + 3.0f), Theme::u32(Theme::TextDim), "(nothing)");
			bl->PopClipRect();
			bl->PushClipRect(ImVec2(p.x + colW + 10.0f, p.y), ImVec2(p.x + colW * 2.0f + 6.0f, p.y + h), true);
			for (std::size_t r = 0; r < b.theirs.size(); ++r)
				bl->AddText(ImVec2(p.x + colW + 16.0f, p.y + 3.0f + lineH * static_cast<float>(r)), Theme::u32(takeTheirs ? Theme::Text : Theme::TextDim), b.theirs[r].c_str());
			if (b.theirs.empty()) bl->AddText(ImVec2(p.x + colW + 16.0f, p.y + 3.0f), Theme::u32(Theme::TextDim), "(nothing)");
			bl->PopClipRect();
			ImGui::Dummy(ImVec2(colW * 2.0f + 10.0f, h + 6.0f));
			ImGui::PopID();
		}
		ImGui::EndChild();

		result = c.diff.merge(c.picks);
		const bool valid = resultIsValid(c.mine, c.theirs, result);
		if (!valid)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warn);
			ImGui::TextUnformatted("This mix is not valid JSON any more: pick differently, or take one side for the block around it.");
			ImGui::PopStyleColor();
		}
		else ImGui::TextDisabled("Yours on the left (r%llu), theirs on the right: what the project has now.",
		                         static_cast<unsigned long long>(c.lost.ourRev));
		ImGui::BeginDisabled(!valid);
		if (EditorWidgets::primaryButton("Use This Result")) resolveWith = true;
		note("Use This Result");
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (EditorWidgets::button("Keep Theirs")) keepTheirs = true;
		note("Keep Theirs");
	}
	if (!c.error.empty())
	{
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Bad);
		ImGui::TextUnformatted(c.error.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::End();

	if (resolveWith || keepTheirs)
	{
		// A newer save of the file may have arrived while the window was open: then compare again
		// instead of writing over it (the server would take ours without a conflict).
		std::string now;
		readAll(theirsFile, now);
		if (now != c.theirs)
		{
			c.theirs = now;
			c.wholeFile = wholeFileByName(c.lost.path) || HE::Sc::looksBinary(c.mine) || HE::Sc::looksBinary(c.theirs);
			if (!c.wholeFile)
			{
				c.diff = HE::Sc::diffText(c.mine, c.theirs);
				c.picks.assign(c.diff.changes(), HE::Sc::Pick::Theirs);
			}
			c.error = "The project's file changed meanwhile (a newer save arrived): compare again.";
			return;
		}
		const bool same = resolveWith && result == c.theirs;   // nothing to write: the project has it already
		if (sync.resolve(c.lost, resolveWith && !same ? &result : nullptr)) c = ConflictView{};
		else c.error = "The project's file could not be written.";
	}
	else if (!open) c = ConflictView{};
}
