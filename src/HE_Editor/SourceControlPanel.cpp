#include "SourceControlPanel.h"
#include "EditorApplication.h"    // AppContext
#include "GitController.h"

#include <Diagnostics/GlobalState.h>
#include <SourceControl/GitProbe.h>
#include <SourceControl/RepoStatus.h>

#include <algorithm>
#include <string>
#include <vector>

#ifdef HE_IMGUI_ENABLED
#include <imgui.h>
#endif

namespace SourceControlPanel
{

#ifdef HE_IMGUI_ENABLED
namespace {

// Panel-local input state. A commit message is short-lived by design; the
// remote URL survives only until it is applied.
char s_commitMessage[512] = "";
char s_remoteUrl[512]     = "";
// GitHub setup inputs. The token buffer is wiped the moment it is handed off —
// it must not sit in static memory for the rest of the session.
char s_ghRepoName[128] = "";
char s_ghToken[256]    = "";
bool s_ghPrivate       = true;
bool s_autoPushLoaded  = false;

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
		for (const Row& r : rows)
		{
			const HE::Sc::FileState s = useIndexState ? r.entry->index : r.entry->worktree;
			ImGui::TextColored(colourFor(s), "%s", letterFor(s));
			ImGui::SameLine();
			ImGui::TextUnformatted(r.path->c_str());
			if (!r.entry->origPath.empty() && ImGui::IsItemHovered())
				ImGui::SetTooltip("renamed from %s", r.entry->origPath.c_str());
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
	// be told even on the frame where the window is closed.
	if (git) git->setPanelVisible(open);
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
			const bool lfs = ctx.gitProbe && ctx.gitProbe->lfsFound;
			ImGui::TextWrapped("Initializing creates the repository with a generated "
			                   ".gitignore (engine output stays out) and .gitattributes "
			                   "(large binaries go through Git LFS).");
			if (!lfs)
			{
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
				ImGui::TextWrapped("git-lfs was not found — the repository will work, "
				                   "but large assets will not be tracked through LFS "
				                   "until it is installed.");
				ImGui::PopStyleColor();
			}
			ImGui::Spacing();
			if (git->busy()) ImGui::BeginDisabled();
			if (ImGui::Button("Initialize Git repository", ImVec2(240.0f, 0.0f)))
				git->requestInit(lfs);
			if (git->busy()) ImGui::EndDisabled();
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
			// Two ways in: let the editor create the repository on GitHub, or
			// paste the URL of one that already exists (any provider).
			ImGui::TextWrapped("Create the repository on GitHub directly:");

			if (s_ghRepoName[0] == '\0' && !git->projectRoot().empty())
			{
				// Default to the project folder's name — right nearly always,
				// editable when not.
				const std::string def = git->projectRoot().filename().string();
				std::snprintf(s_ghRepoName, sizeof(s_ghRepoName), "%s", def.c_str());
			}
			ImGui::SetNextItemWidth(180.0f);
			ImGui::InputText("Name##gh", s_ghRepoName, sizeof(s_ghRepoName));
			ImGui::SameLine();
			ImGui::Checkbox("Private", &s_ghPrivate);

			ImGui::SetNextItemWidth(-140.0f);
			ImGui::InputTextWithHint("##ghtoken", "Personal access token",
			                         s_ghToken, sizeof(s_ghToken),
			                         ImGuiInputTextFlags_Password);
			ImGui::SameLine();
			ImGui::BeginDisabled(git->busy() || s_ghToken[0] == '\0' ||
			                     s_ghRepoName[0] == '\0' || st.initialCommit);
			if (ImGui::Button("Create & push", ImVec2(130.0f, 0.0f)))
			{
				git->requestSetupGitHub(s_ghRepoName, s_ghPrivate, std::string(s_ghToken));
				// Wipe, not clear: the bytes must go, not just the length.
				std::fill(std::begin(s_ghToken), std::end(s_ghToken), '\0');
			}
			ImGui::EndDisabled();
			ImGui::TextDisabled("Token: github.com/settings/tokens — classic, 'repo' scope. "
			                    "It is handed to git's credential helper, stored nowhere else.");
			if (st.initialCommit)
				ImGui::TextDisabled("Make the first commit before setting up the remote.");

			ImGui::Spacing();
			ImGui::TextWrapped("Or paste an existing repository URL (GitHub, GitLab, "
			                   "Azure DevOps):");
			ImGui::SetNextItemWidth(-90.0f);
			ImGui::InputTextWithHint("##remoteurl", "https://github.com/you/project.git",
			                         s_remoteUrl, sizeof(s_remoteUrl));
			ImGui::SameLine();
			ImGui::BeginDisabled(git->busy() || s_remoteUrl[0] == '\0');
			if (ImGui::Button("Set##remote"))
			{
				git->requestSetRemote(s_remoteUrl);
				s_remoteUrl[0] = '\0';
			}
			ImGui::EndDisabled();
		}
		else
		{
			ImGui::TextDisabled("origin: %s", git->remoteUrl().c_str());

			ImGui::BeginDisabled(git->busy() || st.initialCommit);
			if (ImGui::Button("Push", ImVec2(86.0f, 0.0f))) git->requestPush();
			ImGui::SameLine();
			if (ImGui::Button("Pull", ImVec2(86.0f, 0.0f))) git->requestPull();
			ImGui::EndDisabled();

			// Auto-push: commit lands on the remote in the same action. The
			// preference survives restarts — per user, not per project file, so
			// nothing project-visible changes for collaborators.
			if (!s_autoPushLoaded)
			{
				s_autoPushLoaded = true;
				git->autoPushAfterCommit = GlobalState::getInstance()
					.getCustomConfigBool("GitAutoPushAfterCommit", false);
			}
			if (ImGui::Checkbox("Push automatically after each commit",
			                    &git->autoPushAfterCommit))
			{
				GlobalState::getInstance().setCustomConfigEntry(
					"GitAutoPushAfterCommit", git->autoPushAfterCommit);
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

	// ── Changes ──────────────────────────────────────────────────────────────
	ImGui::Separator();
	if (ImGui::BeginChild("##changes", ImVec2(0.0f, 0.0f), false))
	{
		if (st.dirtyCount() == 0)
			ImGui::TextDisabled("Nothing has changed.");
		else
			drawChangeList(st);
	}
	ImGui::EndChild();

	ImGui::End();
#else
	(void)ctx; (void)open;
#endif
}

} // namespace SourceControlPanel
