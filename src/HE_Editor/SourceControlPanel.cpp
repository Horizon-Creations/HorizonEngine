#include "SourceControlPanel.h"
#include "EditorApplication.h"    // AppContext
#include "EditorSettingsPanel.h"  // repo/remote setup lives in the Preferences tab
#include "GitController.h"

#include <Diagnostics/GlobalState.h>
#include <SourceControl/GitProbe.h>
#include <SourceControl/RepoStatus.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace SourceControlPanel
{

#ifdef HE_IMGUI_ENABLED
namespace {

// Panel-local input state. A commit message is short-lived by design. (Repo
// init and remote/GitHub setup live in Preferences ▸ Source Control now.)
char s_commitMessage[512] = "";
bool s_autoPushLoaded     = false;
// Changes as a folder tree (VS Code's tree mode) or a flat list. Persisted.
bool s_treeView       = true;
bool s_treeViewLoaded = false;
bool s_historyOpen    = true;
// Restore confirmation. Destructive enough to deserve a modal that states what
// will happen in full before it happens.
std::string s_restoreOid;
std::string s_restoreSubject;

// Colours chosen so the meaning survives a glance: green for "will be
// committed", amber for "changed but not staged", grey for "git does not know
// about this", red for "needs a human".
ImVec4 colourFor(HE::Sc::FileState s)
{
	using FS = HE::Sc::FileState;
	switch (s)
	{
	case FS::Added:       return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
	case FS::Modified:    return ImVec4(1.00f, 0.78f, 0.35f, 1.0f);
	case FS::Deleted:     return ImVec4(0.95f, 0.50f, 0.45f, 1.0f);
	case FS::Renamed:
	case FS::Copied:      return ImVec4(0.60f, 0.75f, 1.00f, 1.0f);
	case FS::TypeChanged: return ImVec4(0.80f, 0.70f, 1.00f, 1.0f);
	case FS::Untracked:   return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
	case FS::Conflicted:  return ImVec4(1.00f, 0.40f, 0.40f, 1.0f);
	case FS::Ignored:
	case FS::Unmodified:  break;
	}
	return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
}

// One letter per state, the same alphabet git itself uses, so anyone who has
// read `git status` output already knows this.
const char* letterFor(HE::Sc::FileState s)
{
	using FS = HE::Sc::FileState;
	switch (s)
	{
	case FS::Added:       return "A";
	case FS::Modified:    return "M";
	case FS::Deleted:     return "D";
	case FS::Renamed:     return "R";
	case FS::Copied:      return "C";
	case FS::TypeChanged: return "T";
	case FS::Untracked:   return "?";
	case FS::Conflicted:  return "!";
	case FS::Ignored:     return "i";
	case FS::Unmodified:  break;
	}
	return " ";
}

struct Row
{
	const std::string*      path  = nullptr;
	const HE::Sc::FileEntry* entry = nullptr;
};

// One line for one file: coloured state letter + name, full path on hover.
void drawFileRow(const Row& r, bool useIndexState, const char* label)
{
	const HE::Sc::FileState s = useIndexState ? r.entry->index : r.entry->worktree;
	ImGui::TextColored(colourFor(s), "%s", letterFor(s));
	ImGui::SameLine();
	ImGui::TextUnformatted(label);
	if (ImGui::IsItemHovered())
	{
		if (!r.entry->origPath.empty())
			ImGui::SetTooltip("%s\nrenamed from %s", r.path->c_str(),
			                  r.entry->origPath.c_str());
		else
			ImGui::SetTooltip("%s", r.path->c_str());
	}
}

// The folder hierarchy of one section's rows, built per frame from the sorted
// row list. Cheap: dirty sets are small (tens, rarely hundreds), and building
// on the fly means no cache to invalidate against status generations.
struct DirNode
{
	std::map<std::string, DirNode> dirs;
	std::vector<std::pair<std::string, Row>> files;   // leaf name → row
};

void insertRow(DirNode& root, const Row& r)
{
	DirNode* node = &root;
	const std::string& p = *r.path;
	std::size_t start = 0;
	while (true)
	{
		const std::size_t slash = p.find('/', start);
		if (slash == std::string::npos)
		{
			node->files.emplace_back(p.substr(start), r);
			return;
		}
		node = &node->dirs[p.substr(start, slash - start)];
		start = slash + 1;
	}
}

void drawDirNode(const std::string& name, const DirNode& node, bool useIndexState)
{
	// Compact single-child chains the way VS Code does: "Content/Meshes/Props"
	// as one node instead of three nested ones with one child each.
	const DirNode* n = &node;
	std::string label = name;
	while (n->files.empty() && n->dirs.size() == 1)
	{
		label += "/" + n->dirs.begin()->first;
		n = &n->dirs.begin()->second;
	}

	ImGui::PushID(label.c_str());
	if (ImGui::TreeNodeEx(label.c_str(),
	                      ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
	{
		for (const auto& [subName, sub] : n->dirs)
			drawDirNode(subName, sub, useIndexState);
		for (const auto& [leaf, row] : n->files)
			drawFileRow(row, useIndexState, leaf.c_str());
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void drawRowsAsTree(const std::vector<Row>& rows, bool useIndexState)
{
	DirNode root;
	for (const Row& r : rows) insertRow(root, r);
	for (const auto& [name, sub] : root.dirs)
		drawDirNode(name, sub, useIndexState);
	for (const auto& [leaf, row] : root.files)
		drawFileRow(row, useIndexState, leaf.c_str());
}

void drawChangeList(const HE::Sc::RepoStatus& st)
{
	std::vector<Row> conflicts, staged, unstaged, untracked;
	for (const auto& [path, entry] : st.files)
	{
		if (!entry.dirty()) continue;               // ignored files are not changes
		if (entry.conflicted()) { conflicts.push_back({ &path, &entry }); continue; }
		if (entry.worktree == HE::Sc::FileState::Untracked)
		{
			untracked.push_back({ &path, &entry });
			continue;
		}
		if (entry.staged())                          staged.push_back({ &path, &entry });
		if (entry.worktree != HE::Sc::FileState::Unmodified)
			unstaged.push_back({ &path, &entry });
	}

	auto byPath = [](const Row& a, const Row& b) { return *a.path < *b.path; };
	std::sort(conflicts.begin(), conflicts.end(), byPath);
	std::sort(staged.begin(),    staged.end(),    byPath);
	std::sort(unstaged.begin(),  unstaged.end(),  byPath);
	std::sort(untracked.begin(), untracked.end(), byPath);

	auto section = [](const char* title, const std::vector<Row>& rows, bool useIndexState)
	{
		if (rows.empty()) return;
		ImGui::Spacing();
		ImGui::SeparatorText(title);
		if (s_treeView)
		{
			drawRowsAsTree(rows, useIndexState);
		}
		else
		{
			for (const Row& r : rows) drawFileRow(r, useIndexState, r.path->c_str());
		}
	};

	// Conflicts first — they block a commit entirely, so burying them under a
	// long list of ordinary changes would be exactly wrong.
	section("Conflicts — resolve before committing", conflicts, false);
	section("Staged",    staged,    /*useIndexState=*/true);
	section("Changed",   unstaged,  false);
	section("Untracked", untracked, false);
}

} // namespace
#endif // HE_IMGUI_ENABLED

void DrawSourceControlWindow(AppContext& ctx, bool& open)
{
#ifdef HE_IMGUI_ENABLED
	GitController* git = ctx.git;
	// The controller only polls often while the panel is on screen, so it has to
	// be told even on the frame where the window is closed. The Preferences tab's
	// Source Control pages count as "on screen" too — they show the same status.
	if (git) git->setPanelVisible(open || EditorSettingsPanel::sourceControlPageActive());
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(420.0f, 460.0f), ImGuiCond_FirstUseEver);
	// Passing &open lets the window's own X clear the View-menu toggle.
	if (!ImGui::Begin("Source Control", &open))
	{
		ImGui::End();
		return;
	}

	if (!git)
	{
		ImGui::TextDisabled("Source control is unavailable in this build.");
		ImGui::End();
		return;
	}

	if (!ctx.projectLoaded)
	{
		ImGui::TextWrapped("Open a project to see its source-control status.");
		ImGui::End();
		return;
	}

	const HE::Sc::RepoStatus& st = git->status();

	if (!git->isRepo())
	{
		ImGui::TextWrapped("This project is not in a git repository yet.");
		ImGui::Spacing();

		if (git->blockedByCollabSession())
		{
			ImGui::TextDisabled("The session host manages source control for this "
			                    "project.");
		}
		else
		{
			ImGui::TextWrapped("Repository setup (init, remote, GitHub token) lives "
			                   "in Preferences \xe2\x96\xb8 Source Control.");
			ImGui::Spacing();
			if (ImGui::Button("Set up in Preferences…", ImVec2(240.0f, 0.0f)))
				EditorSettingsPanel::requestOpen(EditorSettingsPanel::Page::Repository);
		}

		if (!git->lastError().empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
			ImGui::TextWrapped("%s", git->lastError().c_str());
			ImGui::PopStyleColor();
		}
		if (git->busy()) { ImGui::Spacing(); ImGui::TextDisabled("Working…"); }
		ImGui::End();
		return;
	}

	// ── Branch line ─────────────────────────────────────────────────────────
	if (st.detached)
	{
		// Worth calling out: commits made here belong to no branch and are easy
		// to lose.
		ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "Detached HEAD");
	}
	else
	{
		ImGui::Text("Branch: %s", st.branch.empty() ? "(unknown)" : st.branch.c_str());
	}

	if (st.initialCommit)
	{
		ImGui::TextDisabled("No commits yet.");
	}
	else if (!st.upstream.empty())
	{
		ImGui::SameLine();
		if (st.ahead == 0 && st.behind == 0)
			ImGui::TextDisabled("(up to date with %s)", st.upstream.c_str());
		else
			ImGui::TextDisabled("(%d ahead, %d behind %s)", st.ahead, st.behind,
			                    st.upstream.c_str());
	}
	else
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(no remote configured)");
	}

	// ── Who may write ────────────────────────────────────────────────────────
	if (git->blockedByCollabSession())
	{
		// Explained rather than silently absent: a user who cannot find the
		// commit button should learn why, not conclude the feature is broken.
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.80f, 1.0f, 1.0f));
		ImGui::TextWrapped("You are a guest in a collaboration session. The host "
		                   "manages source control for this project — your changes "
		                   "reach the others through the session.");
		ImGui::PopStyleColor();
	}

	if (!git->lastError().empty())
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
		ImGui::TextWrapped("%s", git->lastError().c_str());
		ImGui::PopStyleColor();
	}

	if (!git->lastInfo().empty() && git->lastError().empty())
	{
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.6f, 0.85f, 0.6f, 1.0f), "%s", git->lastInfo().c_str());
	}

	// ── Commit / sync ────────────────────────────────────────────────────────
	// Hidden — not greyed out — for a session guest: reading status is always
	// allowed, it is writing and moving HEAD that would desync the session.
	if (!git->blockedByCollabSession())
	{
		ImGui::Spacing();
		ImGui::SeparatorText("Commit");

		const bool identityOk = !ctx.gitProbe || ctx.gitProbe->identityConfigured;
		if (!identityOk)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
			ImGui::TextWrapped("git has no user.name / user.email configured — commits "
			                   "will fail until they are set.");
			ImGui::PopStyleColor();
		}

		ImGui::InputTextMultiline("##commitmsg", s_commitMessage, sizeof(s_commitMessage),
		                          ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.0f));

		const bool hasConflicts = st.hasConflicts();
		const bool canCommit = !git->busy() && s_commitMessage[0] != '\0' &&
		                       st.dirtyCount() > 0 && !hasConflicts;
		if (hasConflicts)
		{
			// A commit with unresolved conflict markers preserves the mess
			// forever; refusing is the only correct behaviour.
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
			ImGui::TextWrapped("Conflicts must be resolved before committing.");
			ImGui::PopStyleColor();
		}
		ImGui::BeginDisabled(!canCommit);
		if (ImGui::Button("Commit all changes", ImVec2(180.0f, 0.0f)))
		{
			git->requestCommitAll(s_commitMessage);
			s_commitMessage[0] = '\0';
		}
		ImGui::EndDisabled();

		// ── Remote ───────────────────────────────────────────────────────────
		ImGui::Spacing();
		ImGui::SeparatorText("Remote");
		if (git->remoteUrl().empty())
		{
			// Setup (GitHub create + token, or pasting an existing URL) lives in
			// the Preferences tab — this window is the daily driver, not the
			// one-time configuration.
			ImGui::TextWrapped("No remote configured — set one up in "
			                   "Preferences \xe2\x96\xb8 Source Control.");
			if (ImGui::Button("Set up remote…"))
				EditorSettingsPanel::requestOpen(EditorSettingsPanel::Page::Repository);
		}
		else
		{
			ImGui::TextDisabled("origin: %s", git->remoteUrl().c_str());

			ImGui::BeginDisabled(git->busy() || st.initialCommit);
			if (ImGui::Button("Push", ImVec2(86.0f, 0.0f))) git->requestPush();
			ImGui::SameLine();
			if (ImGui::Button("Pull", ImVec2(86.0f, 0.0f))) git->requestPull();
			ImGui::EndDisabled();

			// Auto-push is toggled in Preferences ▸ Source Control ▸ Repository,
			// but requestCommitAll reads the flag — so the persisted value must be
			// loaded here too, before the first commit, even if the Preferences
			// page was never opened this session.
			if (!s_autoPushLoaded)
			{
				s_autoPushLoaded = true;
				git->autoPushAfterCommit = GlobalState::getInstance()
					.getCustomConfigBool("GitAutoPushAfterCommit", false);
			}

			if (st.initialCommit)
				ImGui::TextDisabled("Make the first commit before pushing.");
			else if (st.behind > 0)
				ImGui::TextDisabled("Pulling fast-forwards only — a diverged branch is "
				                    "reported, never auto-merged.");
		}
	}

	ImGui::Spacing();
	ImGui::Separator();
	if (git->busy()) ImGui::BeginDisabled();
	if (ImGui::Button("Refresh")) git->requestRefresh();
	if (git->busy()) ImGui::EndDisabled();
	ImGui::SameLine();
	if (git->busy()) ImGui::TextDisabled("Working…");
	else             ImGui::TextDisabled("%zu change(s)", st.dirtyCount());

	// Tree ⇄ flat, the same toggle VS Code's source-control view has. Sticky
	// across sessions — a layout choice, not a per-repo fact.
	if (!s_treeViewLoaded)
	{
		s_treeViewLoaded = true;
		s_treeView = GlobalState::getInstance().getCustomConfigBool("GitChangesTreeView", true);
	}
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 74.0f);
	if (ImGui::SmallButton(s_treeView ? "List view" : "Tree view"))
	{
		s_treeView = !s_treeView;
		GlobalState::getInstance().setCustomConfigEntry("GitChangesTreeView", s_treeView);
	}

	// ── Changes ──────────────────────────────────────────────────────────────
	// The history section below claims a fixed slice while it is open; the
	// changes list takes whatever remains.
	ImGui::Separator();
	const float historyH = s_historyOpen
		? 190.0f
		: ImGui::GetFrameHeightWithSpacing();
	if (ImGui::BeginChild("##changes", ImVec2(0.0f, -historyH), false))
	{
		if (st.dirtyCount() == 0)
			ImGui::TextDisabled("Nothing has changed.");
		else
			drawChangeList(st);
	}
	ImGui::EndChild();

	// ── History ──────────────────────────────────────────────────────────────
	// The recent commits, newest first — the "where is this repository" view.
	// An ↑ marks commits the upstream does not have yet, which is the honest
	// answer to "did my push go through".
	s_historyOpen = ImGui::CollapsingHeader("History", ImGuiTreeNodeFlags_DefaultOpen);
	if (s_historyOpen)
	{
		if (ImGui::BeginChild("##history", ImVec2(0.0f, 0.0f), false))
		{
			const auto& commits = git->recentCommits();
			if (commits.empty())
			{
				ImGui::TextDisabled("No commits yet.");
			}
			// Reloading the scene after a restore is the caller's business; the
			// panel says so rather than silently leaving a stale world open.
			for (const auto& c : commits)
			{
				ImGui::PushID(c.shortOid.c_str());
				if (c.unpushed)
				{
					ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", "\xE2\x86\x91");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Not pushed yet");
				}
				else
				{
					ImGui::TextDisabled(" ");
				}
				ImGui::SameLine();
				ImGui::TextDisabled("%s", c.shortOid.c_str());
				ImGui::SameLine();
				ImGui::Selectable(c.subject.c_str());
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s — %s\n(right-click to restore the project to "
					                  "this state)", c.author.c_str(), c.relTime.c_str());

				if (ImGui::BeginPopupContextItem("##commitctx"))
				{
					ImGui::BeginDisabled(git->busy() || st.dirtyCount() != 0);
					if (ImGui::MenuItem("Restore project to this commit…"))
					{
						s_restoreOid     = c.shortOid;
						s_restoreSubject = c.subject;
					}
					ImGui::EndDisabled();
					if (st.dirtyCount() != 0)
						ImGui::TextDisabled("Commit or discard your changes first.");
					ImGui::EndPopup();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	// ── Restore confirmation ─────────────────────────────────────────────────
	// Every file in the project changes at once, so the modal spells out the
	// full consequence AND the reassurance: the restore is recorded as a new
	// commit, so nothing in the history is lost and this itself is undoable.
	if (!s_restoreOid.empty()) ImGui::OpenPopup("Restore project?");
	if (ImGui::BeginPopupModal("Restore project?", nullptr,
	                           ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextWrapped("Every file in the project folder will be put back to how "
		                   "it was at this commit:");
		ImGui::Spacing();
		ImGui::TextDisabled("%s", s_restoreOid.c_str());
		ImGui::SameLine();
		ImGui::TextWrapped("%s", s_restoreSubject.c_str());
		ImGui::Spacing();
		ImGui::TextWrapped("Files added since are removed, changed files are reverted, "
		                   "deleted files come back.");
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 0.6f, 1.0f));
		ImGui::TextWrapped("Your history is kept: this is recorded as a new commit, so "
		                   "you can undo it by restoring to a later one.");
		ImGui::PopStyleColor();
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
		ImGui::TextWrapped("Save and close what you are working on first — open scenes "
		                   "and assets in the editor still hold the old contents and "
		                   "would write them back.");
		ImGui::PopStyleColor();
		ImGui::Spacing();

		if (ImGui::Button("Restore", ImVec2(120.0f, 0.0f)))
		{
			git->requestRestoreTo(s_restoreOid, s_restoreOid);
			s_restoreOid.clear();
			s_restoreSubject.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
		{
			s_restoreOid.clear();
			s_restoreSubject.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	ImGui::End();
#else
	(void)ctx; (void)open;
#endif
}

} // namespace SourceControlPanel
