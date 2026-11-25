#include "GitCloneView.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

#if __has_include(<imgui.h>)
#include "EditorHelp.h"      // "Source Control/<label>" for the buttons
#include "EditorTheme.h"
#include "EditorWidgets.h"
#include <imgui.h>
#endif

namespace GitCloneView
{

bool shouldAutoLoad(AutoLoad& state, bool signedIn, std::uint64_t generation,
                    bool listing, bool cloning)
{
	if (!signedIn || cloning || state.generation == generation) return false;
	state.generation = generation;
	// A listing already under way is this sign-in's load: asking again when it
	// lands would fetch the same list twice.
	return !listing;
}

#if __has_include(<imgui.h>)
namespace {

namespace Theme = HE::Ed::Theme;
inline ImU32 col(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

constexpr float kRowH = 50.0f;
constexpr float kGap  = 6.0f;

// The Project Hub's card (ProjectHubPanel.cpp): fill, then the accent outline.
void cardFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hovered, bool selected)
{
	dl->AddRectFilled(a, b, col(hovered ? Theme::warm(0.22f) : Theme::warm(0.175f)), 6.0f);
	if (selected)     dl->AddRect(a, b, col(Theme::Amber), 6.0f, 0, 2.0f);
	else if (hovered) dl->AddRect(a, b, col(Theme::alpha(Theme::AccentHi, 0.6f)), 6.0f, 0, 1.0f);
}

void groupLabel(const char* text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
}

// A letter on a tile: the account's or the repository's initial.
void initialBadge(ImDrawList* dl, ImVec2 center, float radius, const std::string& name, bool round)
{
	const ImVec2 a(center.x - radius, center.y - radius), b(center.x + radius, center.y + radius);
	const ImU32 fill = col(Theme::mix(Theme::warm(0.24f), Theme::Amber, round ? 0.75f : 0.30f));
	if (round) dl->AddCircleFilled(center, radius, fill, 32);
	else       dl->AddRectFilled(a, b, fill, 5.0f);
	const char letter[2] = { name.empty() ? '?' : char(std::toupper(static_cast<unsigned char>(name[0]))), 0 };
	const ImVec2 ts = ImGui::CalcTextSize(letter);
	dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), col(Theme::Ivory), letter);
}

// Centred in the list box, for every state that has no rows to show.
void boxMessage(const char* text, float height)
{
	const ImVec2 ts = ImGui::CalcTextSize(text);
	ImGui::SetCursorPos(ImVec2(std::max(0.0f, (ImGui::GetContentRegionAvail().x - ts.x) * 0.5f),
	                           std::max(0.0f, (height - ts.y) * 0.5f)));
	groupLabel(text);
}

bool containsNoCase(const std::string& hay, const char* needle)
{
	if (!needle || !*needle) return true;
	const std::string n(needle);
	return std::search(hay.begin(), hay.end(), n.begin(), n.end(), [](char a, char b) {
		       return std::tolower(static_cast<unsigned char>(a)) ==
		              std::tolower(static_cast<unsigned char>(b));
	       }) != hay.end();
}

} // namespace

void drawHeader(ImFont* subheading, float width)
{
	if (subheading) ImGui::PushFont(subheading);
	ImGui::TextUnformatted("Clone from GitHub");
	if (subheading) ImGui::PopFont();
	ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
	ImGui::TextWrapped("Pick a repository from your GitHub account. It is cloned into a new "
	                   "folder, Git LFS assets included, and opened as a project.");
	ImGui::PopStyleColor();

	// The hub's horizon line: amber fading out to the right.
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImU32 a = col(Theme::Amber), z = col(Theme::alpha(Theme::Amber, 0.0f));
	ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
		ImVec2(p.x, p.y + 6.0f), ImVec2(p.x + width * 0.6f, p.y + 8.0f), a, z, z, a);
	ImGui::Dummy(ImVec2(0.0f, 16.0f));
}

bool drawAccountCard(const std::string& login, bool busy, float width)
{
	HE::Ed::Help::Scope helpScope("Source Control");
	constexpr float kH = 54.0f;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + kH);
	cardFrame(dl, a, b, false, false);
	initialBadge(dl, ImVec2(a.x + 12.0f + 16.0f, a.y + kH * 0.5f), 16.0f, login, true);

	const float lh = ImGui::GetTextLineHeight();
	const float ty = a.y + (kH - lh * 2.0f - 2.0f) * 0.5f;
	dl->AddText(ImVec2(a.x + 56.0f, ty), col(Theme::Text), login.c_str());
	dl->AddText(ImVec2(a.x + 56.0f, ty + lh + 2.0f), col(Theme::Ok), "Signed in to GitHub");

	const float bw = ImGui::CalcTextSize("Sign out").x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::SetCursorScreenPos(ImVec2(b.x - 12.0f - bw, a.y + (kH - ImGui::GetFrameHeight()) * 0.5f));
	ImGui::BeginDisabled(busy);
	const bool out = EditorWidgets::button("Sign out");
	ImGui::EndDisabled();
	ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
	ImGui::Dummy(ImVec2(width, 10.0f));
	return out;
}

ListAction drawRepoList(ListState state, const std::vector<HE::Sc::RepoListEntry>& repos,
                        char* filter, std::size_t filterSize, std::string& selected,
                        bool disabled, float height)
{
	HE::Ed::Help::Scope helpScope("Source Control");
	ListAction action = ListAction::None;

	if (state == ListState::Ready && !repos.empty())
		groupLabel(("Your repositories (" + std::to_string(repos.size()) + ")").c_str());
	else
		groupLabel("Your repositories");

	const float refreshW = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
	ImGui::BeginDisabled(disabled || state != ListState::Ready);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - refreshW - 8.0f);
	ImGui::InputTextWithHint("##clonefilter", "Search repositories\xe2\x80\xa6", filter, filterSize);
	ImGui::EndDisabled();
	ImGui::SameLine(0.0f, 8.0f);
	ImGui::BeginDisabled(disabled || state == ListState::Loading);
	if (EditorWidgets::button("Refresh##repos", ImVec2(refreshW, 0.0f))) action = ListAction::Refresh;
	ImGui::EndDisabled();

	ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::warm(0.115f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
	ImGui::BeginChild("##clonerepos", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();

	if (state == ListState::Loading)
		boxMessage("Loading your repositories\xe2\x80\xa6", height);
	else if (state == ListState::Failed)
		boxMessage("No repository list. Refresh to try again.", height);
	else if (repos.empty())
		boxMessage("This account has no repositories yet.", height);
	else
	{
		std::vector<int> shown;
		shown.reserve(repos.size());
		for (std::size_t i = 0; i < repos.size(); ++i)
			if (containsNoCase(repos[i].fullName, filter)) shown.push_back(int(i));
		if (shown.empty()) boxMessage("Nothing matches the search.", height);

		ImGui::BeginDisabled(disabled);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, kGap));
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float lh = ImGui::GetTextLineHeight();
		// Rows of one height, only the visible ones drawn: an organisation can
		// bring hundreds.
		ImGuiListClipper clipper;
		clipper.Begin(int(shown.size()), kRowH + kGap);
		while (clipper.Step())
			for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n)
			{
				const HE::Sc::RepoListEntry& r = repos[std::size_t(shown[std::size_t(n)])];
				ImGui::PushID(shown[std::size_t(n)]);
				const float w = ImGui::GetContentRegionAvail().x;
				const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + kRowH);
				if (ImGui::InvisibleButton("##repo", ImVec2(w, kRowH)))
				{
					selected = r.fullName;
					action   = ListAction::Picked;
				}
				const bool hovered = ImGui::IsItemHovered() && !disabled;
				cardFrame(dl, a, b, hovered, r.fullName == selected);
				initialBadge(dl, ImVec2(a.x + 10.0f + 15.0f, a.y + kRowH * 0.5f), 15.0f, r.name, false);

				// Visibility as a pill on the right, where a long name cannot push it away.
				const char* vis = r.isPrivate ? "Private" : "Public";
				const ImVec2 vs = ImGui::CalcTextSize(vis);
				const ImVec2 pa(b.x - 12.0f - vs.x - 16.0f, a.y + (kRowH - vs.y - 4.0f) * 0.5f);
				const ImVec2 pb(b.x - 12.0f, pa.y + vs.y + 4.0f);
				dl->AddRectFilled(pa, pb, col(r.isPrivate ? Theme::alpha(Theme::Amber, 0.22f)
				                                          : Theme::warm(0.25f)), 9.0f);
				dl->AddText(ImVec2(pa.x + 8.0f, pa.y + 2.0f),
				            col(r.isPrivate ? Theme::Gold : Theme::TextDim), vis);

				const std::string owner = r.fullName.substr(0, r.fullName.find('/'));
				const std::string meta  = owner + "  \xc2\xb7  " +
					(r.defaultBranch.empty() ? std::string("no commits yet")
					                         : "updated " + r.updatedAt.substr(0, 10));
				const float tx = a.x + 50.0f, ty = a.y + (kRowH - lh * 2.0f - 2.0f) * 0.5f;
				dl->PushClipRect(ImVec2(tx, a.y), ImVec2(pa.x - 8.0f, b.y), true);
				dl->AddText(ImVec2(tx, ty), col(Theme::Text), r.name.c_str());
				dl->AddText(ImVec2(tx, ty + lh + 2.0f), col(Theme::TextDim), meta.c_str());
				dl->PopClipRect();
				ImGui::PopID();
			}
		ImGui::PopStyleVar();
		ImGui::EndDisabled();
	}
	ImGui::EndChild();
	ImGui::Dummy(ImVec2(0.0f, 4.0f));
	return action;
}

TargetAction drawTarget(char* parentDir, std::size_t parentSize,
                        char* folderName, std::size_t nameSize,
                        bool picked, bool disabled, bool emptyRepository)
{
	HE::Ed::Help::Scope helpScope("Source Control");
	TargetAction action = TargetAction::None;
	const float avail   = ImGui::GetContentRegionAvail().x;
	const float nameW   = std::floor(avail * 0.34f);
	const float browseW = 80.0f;
	const float x0      = ImGui::GetCursorPosX();

	groupLabel("Folder name");
	ImGui::SameLine(x0 + nameW + 10.0f);
	groupLabel("Location");

	ImGui::BeginDisabled(disabled || !picked);
	ImGui::SetNextItemWidth(nameW);
	ImGui::InputTextWithHint("##clonename", "Folder name", folderName, nameSize);
	ImGui::SameLine(0.0f, 10.0f);
	ImGui::SetNextItemWidth(avail - nameW - 10.0f - browseW - 8.0f);
	ImGui::InputText("##cloneparent", parentDir, parentSize);
	ImGui::SameLine(0.0f, 8.0f);
	if (EditorWidgets::button("Browse##clone", ImVec2(browseW, 0.0f))) action = TargetAction::Browse;
	ImGui::EndDisabled();

	ImGui::Dummy(ImVec2(0.0f, 2.0f));
	const float cloneW = 140.0f;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail - cloneW - 12.0f);
	if (!picked)
		groupLabel("Pick a repository above.");
	else if (emptyRepository)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::Warn);
		ImGui::TextWrapped("This repository has no commits yet, so there is no project in it to open.");
		ImGui::PopStyleColor();
	}
	else
	{
		const std::string target = (std::filesystem::path(parentDir) / folderName).string();
		ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
		ImGui::TextWrapped("%s", target.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();
	ImGui::SameLine(x0 + avail - cloneW);
	ImGui::BeginDisabled(disabled || !picked || parentDir[0] == '\0' || folderName[0] == '\0');
	if (EditorWidgets::primaryButton("Clone", ImVec2(cloneW, 0.0f))) action = TargetAction::Clone;
	ImGui::EndDisabled();
	return action;
}
#else
void drawHeader(ImFont*, float) {}
bool drawAccountCard(const std::string&, bool, float) { return false; }
ListAction drawRepoList(ListState, const std::vector<HE::Sc::RepoListEntry>&, char*, std::size_t,
                        std::string&, bool, float) { return ListAction::None; }
TargetAction drawTarget(char*, std::size_t, char*, std::size_t, bool, bool, bool) { return TargetAction::None; }
#endif

} // namespace GitCloneView
