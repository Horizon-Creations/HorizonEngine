#include "SourceControlPanel.h"
#include "EditorApplication.h"    // AppContext
#include "EditorSettingsPanel.h"  // repo/remote setup lives in the Preferences tab
#include "EditorToolbar.h"     // shared toolbar look (Scene bar uses the same)
#include "EditorTheme.h"       // brand palette (emphasis text)
#include "EditorHelp.h"        // "Source Control Panel/<label>" scope for the tooltips
#include "EditorWidgets.h"     // primary/danger/cancel buttons
#include "GitController.h"

#include <Diagnostics/GlobalState.h>
#include <SourceControl/GitCli.h>
#include <SourceControl/GitProbe.h>
#include <SourceControl/RepoStatus.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
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
bool s_prefsLoaded        = false;

// The settings the CONTROLLER acts on but the Preferences page owns: auto-push
// (read by requestCommitAll) and the background-fetch schedule (read by
// update()). Both have to be in the controller before the user visits that page,
// or the first commit of a session ignores auto-push and the fetch timer never
// starts for anyone who never opens Preferences.
//
// Called from the panel AND from the footer status, because the footer runs on
// every frame with a project open while the panel may never be opened at all —
// and a guest in a collaboration session, who sees no commit UI, still wants the
// ahead/behind counters to stay honest.
void ensureSettingsLoaded(GitController& git)
{
	if (s_prefsLoaded) return;
	s_prefsLoaded = true;
	GlobalState& gs = GlobalState::getInstance();
	git.autoPushAfterCommit = gs.getCustomConfigBool("GitAutoPushAfterCommit", false);
	git.autoFetch           = gs.getCustomConfigBool("GitAutoFetch", false);
	git.autoFetchMinutes    = gs.getCustomConfigInt("GitAutoFetchMinutes", 15);
}
// Changes as a folder tree (VS Code's tree mode) or a flat list. Persisted.
bool s_treeView       = true;
bool s_treeViewLoaded = false;
bool s_historyOpen    = true;
// Restore confirmation. Destructive enough to deserve a modal that states what
// will happen in full before it happens.
std::string s_restoreOid;
std::string s_restoreSubject;
// Branch creation. The start commit is empty when branching off the current
// state rather than off a specific commit in the history.
std::string s_branchFromOid;
std::string s_branchFromSubject;
char        s_branchName[128] = "";
bool        s_branchCheckout  = true;
bool        s_branchDialog    = false;

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

// ImGui pitfall these two modals kept walking into: AlwaysAutoResize together
// with TextWrapped has no width to wrap AGAINST, so the window grows to the
// longest unbroken run of text and gets clipped by the viewport edge. Pinning
// the width turns the flag back into what it is wanted for — auto HEIGHT.
//
// Sized in multiples of the font size rather than pixels, so it follows the
// editor's font scale instead of assuming one.
void beginModalSizing(float widthInChars = 26.0f)
{
	const float w = ImGui::GetFontSize() * widthInChars;
	ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0.0f), ImVec2(w, FLT_MAX));
}

// Two equal buttons filling the row, so they scale with the dialog rather than
// spilling out of a font-scaled window at a hardcoded 120 px.
bool modalButtonRow(const char* confirmLabel, const char* cancelLabel,
                    bool confirmDisabled, bool& outCancelled, bool danger = false)
{
	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float w       = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;

	// The way out first, the commitment second — reading order ends on the
	// action, and the filled button sits where the eye stops.
	outCancelled = EditorWidgets::cancelButton(cancelLabel, ImVec2(w, 0.0f));
	ImGui::SameLine();
	ImGui::BeginDisabled(confirmDisabled);
	const bool confirmed = danger
		? EditorWidgets::dangerButton(confirmLabel, ImVec2(w, 0.0f))
		: EditorWidgets::primaryButton(confirmLabel, ImVec2(w, 0.0f));
	ImGui::EndDisabled();
	return confirmed;
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
		// The tooltip that answers "which file is this really" — so it is a full
		// path, and for a rename, two of them. Left to SetTooltip that is one
		// unbroken line, and a tooltip is as wide as its longest line: a box
		// reaching across the editor to say one path, which is the look this pass
		// is here to stop. Wrapped at a fixed column because a tooltip has no
		// width of its own to wrap against — it is whatever its contents made it.
		// Asked as a question, because imgui.h says so in as many words: EndTooltip
		// is only to be called when BeginTooltip returned true.
		if (ImGui::BeginTooltip())
		{
			{
				EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
				if (!r.entry->origPath.empty())
					ImGui::Text("%s\nrenamed from %s", r.path->c_str(),
					            r.entry->origPath.c_str());
				else
					ImGui::TextUnformatted(r.path->c_str());
			}
			ImGui::EndTooltip();
		}
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
		// Sections share one ID scope, and the same folder legitimately shows up
		// in more than one of them (Content/ both changed and untracked). Without
		// a per-section scope those tree nodes collide on ID — they would fold
		// and unfold together, and ImGui flags the conflict.
		ImGui::PushID(title);
		if (s_treeView)
		{
			drawRowsAsTree(rows, useIndexState);
		}
		else
		{
			for (const Row& r : rows) drawFileRow(r, useIndexState, r.path->c_str());
		}
		ImGui::PopID();
	};

	// Conflicts first — they block a commit entirely, so burying them under a
	// long list of ordinary changes would be exactly wrong.
	section("Conflicts — resolve before committing", conflicts, false);
	section("Staged",    staged,    /*useIndexState=*/true);
	section("Changed",   unstaged,  false);
	section("Untracked", untracked, false);
}

// ── Header bar ───────────────────────────────────────────────────────────────
// Built from the same primitives as the Scene toolbar (EditorToolbar), so the
// two read as one editor rather than as two applications sharing a window:
//
//   [ ⑂ main ]   [ ⟳ │ ☁ │ ↓ 2 │ ↑ 1 ]                    [ tree │ ⚙ ]
//    ← where you are   ← what to do about the remote        ← how it looks
//
// It shrinks in defined steps rather than overflowing — first the sync labels
// go, then the view toggle, then the branch name — and everything that gets
// dropped stays reachable in the ⚙ popup, which is never dropped.

void drawBranchPopup(GitController& git, const HE::Sc::RepoStatus& st)
{
	// "Source Control Panel", not "Source Control": that scope belongs to the
	// Preferences page that INSTALLS git, and this is the window that uses it.
	HE::Ed::Help::Scope helpScope("Source Control Panel");
	ImGui::TextDisabled("Branches");
	ImGui::Separator();
	if (git.branches().empty())
	{
		ImGui::TextDisabled("(none yet)");
	}
	else
	{
		// Listed, not switchable: checking out replaces the whole working tree,
		// and doing that from a hover menu is how someone loses an afternoon.
		// Switching lives behind the branch dialog's explicit checkbox.
		for (const std::string& b : git.branches())
			ImGui::BulletText("%s%s", b.c_str(), b == st.branch ? "  (current)" : "");
	}
	ImGui::Separator();
	ImGui::BeginDisabled(git.busy() || st.initialCommit || !git.mayModify());
	if (EditorWidgets::menuItem("New branch…"))
	{
		s_branchFromOid.clear();          // empty start = branch off HEAD
		s_branchFromSubject.clear();
		s_branchName[0]  = '\0';
		s_branchCheckout = st.dirtyCount() == 0;
		s_branchDialog   = true;
	}
	ImGui::EndDisabled();
	if (st.initialCommit)
		ImGui::TextDisabled("Make the first commit first.");
}

void drawOptionsPopup(GitController& git, const HE::Sc::RepoStatus& st)
{
	// Wrapped at a fixed column rather than at the window edge, which is the one
	// case where the panel-wide rule does not apply: a popup sizes itself to its
	// contents, so "wrap where the window ends" is circular — the window ends
	// wherever the longest line put it. A remote URL is one unbroken run of
	// eighty characters, and the popup grew to fit it, right across the editor.
	// Twenty-six ems is the same measure the dialogs in this file are pinned to,
	// so the two look like they belong to one program.
	//
	// The scope is the function body, which ends before the caller's EndPopup() —
	// the pop has to happen while this popup is still the current window.
	EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 26.0f);
	HE::Ed::Help::Scope helpScope("Source Control Panel");

	ImGui::TextDisabled("Changes");
	ImGui::Separator();
	if (EditorWidgets::menuItem("Tree view", nullptr, s_treeView))
	{
		s_treeView = true;
		GlobalState::getInstance().setCustomConfigEntry("GitChangesTreeView", true);
	}
	if (EditorWidgets::menuItem("Flat list", nullptr, !s_treeView))
	{
		s_treeView = false;
		GlobalState::getInstance().setCustomConfigEntry("GitChangesTreeView", false);
	}

	ImGui::Spacing();
	ImGui::TextDisabled("Remote");
	ImGui::Separator();
	if (git.remoteUrl().empty())
	{
		ImGui::TextDisabled("None configured");
	}
	else
	{
		ImGui::TextDisabled("origin: %s", git.remoteUrl().c_str());
		if (!st.upstream.empty())
			ImGui::TextDisabled("tracking %s", st.upstream.c_str());
		// The schedule itself is a preference, not a per-panel switch — this is
		// only the readout, so the panel never disagrees with the settings page.
		ImGui::TextDisabled(git.autoFetch
			? "Fetching automatically every %d min"
			: "Automatic fetching is off", std::max(GitController::kMinFetchMinutes,
			                                        git.autoFetchMinutes));
	}

	ImGui::Spacing();
	ImGui::Separator();
	if (EditorWidgets::menuItem("Open source-control settings…"))
		EditorSettingsPanel::requestOpen(EditorSettingsPanel::Page::Repository);
}

void drawHeaderBar(GitController& git, const HE::Sc::RepoStatus& st)
{
	namespace T = EditorToolbar;

	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float  barW   = ImGui::GetContentRegionAvail().x;
	const T::Metrics m  = T::metrics(origin.y);

	const bool conflicts = st.hasConflicts();
	// A repository with unresolved conflicts is not in a state to be synced, and
	// that is worth seeing before reading a single filename — so it washes the
	// whole strip, exactly as play mode does in the Scene bar.
	T::bar(origin, barW, m,
	       conflicts ? T::kBadWell : 0u,
	       conflicts ? T::kBad : T::kBarLine,
	       conflicts ? 2.0f : 1.0f);

	const bool mayWrite  = git.mayModify();
	const bool hasRemote = !git.remoteUrl().empty();
	const bool idle      = !git.busy();

	const char* branchLabel = st.detached ? "detached"
	                        : st.branch.empty() ? "(no branch)"
	                                            : st.branch.c_str();

	char aheadTxt[16], behindTxt[16];
	std::snprintf(aheadTxt,  sizeof(aheadTxt),  "%d", st.ahead);
	std::snprintf(behindTxt, sizeof(behindTxt), "%d", st.behind);

	// ── Fit ─────────────────────────────────────────────────────────────────
	// Measured before anything is drawn, so the shrink steps are decided once
	// and the row cannot reflow halfway through.
	const float branchW = T::cellWidth(m, branchLabel);
	auto syncWidth = [&](bool labels)
	{
		float w = T::kWellPad * 2.0f + m.cell;                        // refresh
		if (hasRemote) w += T::kSegGap + m.cell;                      // fetch
		if (hasRemote && mayWrite)
		{
			w += T::kSegGap + (labels ? T::cellWidth(m, behindTxt) : m.cell);
			w += T::kSegGap + (labels ? T::cellWidth(m, aheadTxt)  : m.cell);
		}
		return w;
	};
	auto rightWidth = [&](bool viewToggle)
	{
		float w = T::kWellPad * 2.0f + m.cell;                        // options
		if (viewToggle) w += T::kSegGap + m.cell;
		return w;
	};

	bool labels = true, viewToggle = true, branchName = true;
	auto leftWidth = [&](bool name)
	{
		return T::kWellPad * 2.0f + (name ? branchW : m.cell);
	};
	auto fits = [&] {
		return leftWidth(branchName) + T::kGroupGap + syncWidth(labels) +
		       T::kGroupGap + rightWidth(viewToggle) + T::kEdgeGap * 2.0f <= barW;
	};
	if (!fits()) labels     = false;
	if (!fits()) viewToggle = false;
	if (!fits()) branchName = false;

	// ── Left: where you are ─────────────────────────────────────────────────
	float x = origin.x + T::kEdgeGap;
	{
		const float w = leftWidth(branchName);
		T::well(m, x, w);
		if (T::cell(m, x + T::kWellPad, w - T::kWellPad * 2.0f, "##branch",
		            T::iconBranch, branchName ? branchLabel : nullptr,
		            false, true,
		            st.detached ? "Detached HEAD — commits here belong to no branch"
		                        : "Branch. Click for the branch list."))
		{
			ImGui::OpenPopup("##branchPopup");
		}
		if (ImGui::BeginPopup("##branchPopup")) { drawBranchPopup(git, st); ImGui::EndPopup(); }
		x += w + T::kGroupGap;
	}

	// ── Middle: the remote ──────────────────────────────────────────────────
	{
		const float w = syncWidth(labels);
		T::well(m, x, w);
		float cx = x + T::kWellPad;

		if (T::cell(m, cx, m.cell, "##refresh", T::iconRefresh, nullptr, false, idle,
		            "Refresh status"))
		{
			git.requestRefresh();
		}
		cx += m.cell + T::kSegGap;

		if (hasRemote)
		{
			if (T::cell(m, cx, m.cell, "##fetch", T::iconCloud, nullptr, false, idle,
			            "Fetch — update what the remote has, without changing anything "
			            "here.\nAutomatic fetching is set up in Preferences \xe2\x96\xb8 "
			            "Source Control."))
			{
				git.requestFetch();
			}
			cx += m.cell + T::kSegGap;
		}

		if (hasRemote && mayWrite)
		{
			// Ahead and behind ARE the pull and push buttons: the count is the
			// reason you would press them, so putting it anywhere else means
			// reading one thing and clicking another.
			const float pullW = labels ? T::cellWidth(m, behindTxt) : m.cell;
			const bool  canPull = idle && !st.initialCommit;
			if (T::cellTinted(m, cx, pullW, "##pull", T::iconArrowDown,
			                  labels ? behindTxt : nullptr,
			                  st.behind > 0 ? T::kWarn : T::kFg, canPull,
			                  st.behind > 0
			                      ? "Pull — fast-forward only; a diverged branch is "
			                        "reported, never auto-merged."
			                      : "Pull (nothing to pull)"))
			{
				git.requestPull();
			}
			cx += pullW + T::kSegGap;

			const float pushW = labels ? T::cellWidth(m, aheadTxt) : m.cell;
			if (T::cellTinted(m, cx, pushW, "##push", T::iconArrowUp,
			                  labels ? aheadTxt : nullptr,
			                  st.ahead > 0 ? T::kGood : T::kFg, canPull,
			                  st.initialCommit ? "Make the first commit before pushing"
			                                   : "Push"))
			{
				git.requestPush();
			}
		}
		x += w + T::kGroupGap;
	}

	// ── Right: how it looks ─────────────────────────────────────────────────
	{
		const float w  = rightWidth(viewToggle);
		const float rx = origin.x + barW - T::kEdgeGap - w;
		T::well(m, rx, w);
		float cx = rx + T::kWellPad;

		if (viewToggle)
		{
			if (T::cell(m, cx, m.cell, "##view", s_treeView ? T::iconTree : T::iconList,
			            nullptr, false, true,
			            s_treeView ? "Showing changes as a folder tree — click for a flat list"
			                       : "Showing changes as a flat list — click for a folder tree"))
			{
				s_treeView = !s_treeView;
				GlobalState::getInstance().setCustomConfigEntry("GitChangesTreeView", s_treeView);
			}
			cx += m.cell + T::kSegGap;
		}

		if (T::cell(m, cx, m.cell, "##scopts", T::iconGear, nullptr, false, true, "Options"))
			ImGui::OpenPopup("##scOptions");
		if (ImGui::BeginPopup("##scOptions")) { drawOptionsPopup(git, st); ImGui::EndPopup(); }
	}

	// Busy and conflicts, on the strip itself rather than as a line under it —
	// they are states of the bar's own controls, and a line that appears and
	// disappears shoves the whole panel up and down.
	if (!idle || conflicts)
	{
		const char* note = conflicts ? "conflicts" : "working…";
		const float noteW = ImGui::CalcTextSize(note).x;
		const float slot  = origin.x + barW - T::kEdgeGap - rightWidth(viewToggle)
		                  - T::kGroupGap - noteW;
		if (slot > x)
		{
			ImGui::GetWindowDrawList()->AddText(
				ImVec2(std::floor(slot), std::floor(m.cy - ImGui::GetFontSize() * 0.5f)),
				conflicts ? T::kBad : T::kFgDim, note);
		}
	}

	// Hand the rest of the window to the panel body, exactly as the Scene bar
	// hands it to the viewport image.
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + m.bar));
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
	if (git)
	{
		git->setPanelVisible(open || EditorSettingsPanel::sourceControlPageActive());
		ensureSettingsLoaded(*git);
	}
	if (!open) return;

	ImGui::SetNextWindowSize(ImVec2(420.0f, 460.0f), ImGuiCond_FirstUseEver);
	// Passing &open lets the window's own X clear the View-menu toggle.
	if (!ImGui::Begin("Source Control", &open))
	{
		ImGui::End();
		return;
	}
	HE::Ed::Help::Scope helpScope("Source Control Panel");

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
			if (EditorWidgets::button("Set up in Preferences…", ImVec2(240.0f, 0.0f)))
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

	// ── Header bar ───────────────────────────────────────────────────────────
	// Branch, sync and view, in the same visual language as the Scene toolbar:
	// related controls in one rounded well, groups separated by a gap, cells
	// icon-first with a tooltip. The row used to be a text line, three loose
	// buttons and a SeparatorText per section, spread over a third of the panel
	// before the first change was visible.
	// Tree ⇄ flat, the same choice VS Code's source-control view offers. Sticky
	// across sessions — a layout preference, not a per-repo fact. Loaded before
	// the bar, which is where the toggle now lives.
	if (!s_treeViewLoaded)
	{
		s_treeViewLoaded = true;
		s_treeView = GlobalState::getInstance().getCustomConfigBool("GitChangesTreeView", true);
	}

	drawHeaderBar(*git, st);

	// ── What is in the way ───────────────────────────────────────────────────
	// Errors and the guest notice sit directly under the bar and nowhere else.
	// The old layout scattered status text between every section, so the panel
	// grew and shrank as things happened and the change list moved with it.
	if (git->blockedByCollabSession())
	{
		EditorWidgets::WrapText wrap;
		// Explained rather than silently absent: a user who cannot find the
		// commit button should learn why, not conclude the feature is broken.
		ImGui::PushStyleColor(ImGuiCol_Text, HE::Ed::Theme::TextHeading);
		ImGui::TextWrapped("You are a guest in a collaboration session. The host manages "
		                   "source control for this project — your changes reach the "
		                   "others through the session.");
		ImGui::PopStyleColor();
	}
	if (!git->lastError().empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
		ImGui::TextWrapped("%s", git->lastError().c_str());
		ImGui::PopStyleColor();
	}
	else if (!git->lastInfo().empty())
	{
		// The one line here that was not already wrapped, and it is a whole
		// sentence from git ("Fetched: 3 new commits on origin/main") in a panel
		// docked at 420 px.
		EditorWidgets::WrapText wrap;
		ImGui::TextDisabled("%s", git->lastInfo().c_str());
	}

	// ── Commit ───────────────────────────────────────────────────────────────
	// Hidden — not greyed out — for a session guest: reading status is always
	// allowed, it is writing and moving HEAD that would desync the session.
	if (!git->blockedByCollabSession())
	{
		// One wrap position per block rather than one for the window: this
		// panel's ImGui::End() is at the bottom of the function, and a guard
		// opened next to Begin() would pop after it — onto whichever window is
		// current by then. A block is the lifetime that matches, and between them
		// these guards cover everything the panel draws below the header bar.
		EditorWidgets::WrapText wrap;

		const bool identityOk = !ctx.gitProbe || ctx.gitProbe->identityConfigured;
		if (!identityOk)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
			ImGui::TextWrapped("git has no user.name / user.email configured — commits "
			                   "will fail until they are set.");
			ImGui::PopStyleColor();
		}

		ImGui::InputTextWithHint("##commitmsg", "Message for this commit",
		                         s_commitMessage, sizeof(s_commitMessage));

		const std::size_t dirty     = st.dirtyCount();
		const bool hasConflicts     = st.hasConflicts();
		const bool canCommit = !git->busy() && s_commitMessage[0] != '\0' &&
		                       dirty > 0 && !hasConflicts;

		char commitLabel[64];
		if (dirty == 0)      std::snprintf(commitLabel, sizeof(commitLabel), "Nothing to commit");
		else if (dirty == 1) std::snprintf(commitLabel, sizeof(commitLabel), "Commit 1 change");
		else std::snprintf(commitLabel, sizeof(commitLabel), "Commit %zu changes", dirty);

		ImGui::BeginDisabled(!canCommit);
		if (ImGui::Button(commitLabel, ImVec2(-FLT_MIN, 0.0f)))
		{
			git->requestCommitAll(s_commitMessage);
			s_commitMessage[0] = '\0';
		}
		ImGui::EndDisabled();
		// By key, not by label: the label is built at run time ("Commit 3
		// changes"), so there is no fixed string to key the table on. The lookup
		// allows a disabled item, which is exactly when this gets asked.
		EditorWidgets::helpForKey("sc.commit");

		if (hasConflicts)
		{
			// A commit with unresolved conflict markers preserves the mess
			// forever; refusing is the only correct behaviour.
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
			ImGui::TextWrapped("Conflicts must be resolved before committing.");
			ImGui::PopStyleColor();
		}
		else if (st.initialCommit && git->remoteUrl().empty())
		{
			ImGui::TextDisabled("No remote yet — set one up in Preferences \xe2\x96\xb8 "
			                    "Source Control.");
		}

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
		// The list this panel exists for, and every row in it is a path. In the
		// flat view that path is the full one from the project root, which is
		// longer than the panel every time — and it is the END that identifies
		// the file. Cut off at the right edge, three assets in the same folder
		// read as three copies of the same row. A child is its own window, so it
		// needs its own wrap position; this one ends before EndChild() below.
		EditorWidgets::WrapText wrap;

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
			EditorWidgets::WrapText wrap;

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
					// Branching is non-destructive as long as it only writes a
					// ref, so it stays available with a dirty tree — the
					// dialog's "switch to it" is what the guard applies to.
					ImGui::BeginDisabled(git->busy());
					if (EditorWidgets::menuItem("Create branch from this commit…"))
					{
						s_branchFromOid     = c.shortOid;
						s_branchFromSubject = c.subject;
						s_branchName[0]     = '\0';
						s_branchCheckout    = st.dirtyCount() == 0;
						s_branchDialog      = true;
					}
					ImGui::EndDisabled();

					ImGui::BeginDisabled(git->busy() || st.dirtyCount() != 0);
					if (EditorWidgets::menuItem("Restore project to this commit…"))
					{
						s_restoreOid     = c.shortOid;
						s_restoreSubject = c.subject;
					}
					ImGui::EndDisabled();
					if (st.dirtyCount() != 0)
					{
						// Wrapped at a column, not at the window edge: a popup is
						// as wide as its widest line, so wrapping "where the
						// window ends" would be asking the sentence where it
						// ends. This is the only prose in a menu of two entries,
						// and unwrapped it stretched the menu to twice their
						// width. Ends before EndPopup(), like every other guard.
						// Named apart from the history child's guard it sits
						// inside, so neither reads as the other one.
						EditorWidgets::WrapText menuWrap(ImGui::GetFontSize() * 26.0f);
						ImGui::TextDisabled("Restoring needs a clean project — commit or "
						                    "discard your changes first.");
					}
					ImGui::EndPopup();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	// ── Create branch ────────────────────────────────────────────────────────
	if (s_branchDialog) ImGui::OpenPopup("Create branch");
	beginModalSizing();
	if (ImGui::BeginPopupModal("Create branch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		// beginModalSizing() pinned the width, so wrapping at the window edge means
		// something here — and the two lines that were not already wrapped are the
		// ones that matter: the commit subject this branch starts from, and the
		// note about a dirty project, which was hand-broken at a column that stops
		// being right the moment the editor font is scaled.
		{
			EditorWidgets::WrapText wrap;

			if (s_branchFromOid.empty())
			{
				ImGui::TextWrapped("New branch from the current state (%s).",
				                   st.branch.empty() ? "no branch" : st.branch.c_str());
			}
			else
			{
				ImGui::TextWrapped("New branch starting at:");
				// One wrapped run, not TextDisabled + SameLine + TextWrapped: at a
				// fixed dialog width the second half would wrap back under the
				// first and read as two unrelated lines.
				ImGui::TextWrapped("%s  %s", s_branchFromOid.c_str(),
				                   s_branchFromSubject.c_str());
			}
			ImGui::Spacing();

			ImGui::SetNextItemWidth(-FLT_MIN);   // fill the (now fixed) width
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextWithHint("##branchname", "feature/new-lighting",
			                         s_branchName, sizeof(s_branchName));

			// Validate while typing: git's own rules, asked of git, so the answer
			// cannot drift from what the command will accept.
			const bool empty = s_branchName[0] == '\0';
			bool nameOk = false, taken = false;
			if (!empty && !git->projectRoot().empty())
			{
				nameOk = HE::Sc::GitCli::isValidBranchName(git->projectRoot(), s_branchName);
				taken  = nameOk && HE::Sc::GitCli::branchExists(git->projectRoot(), s_branchName);
			}
			if (!empty && !nameOk)
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f),
				                   "Not a usable branch name.");
			else if (taken)
				ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f),
				                   "A branch with this name already exists.");

			const bool dirty = st.dirtyCount() != 0;
			ImGui::BeginDisabled(dirty);
			EditorWidgets::checkbox("Switch to it right away", &s_branchCheckout);
			ImGui::EndDisabled();
			if (dirty)
			{
				// Creating the ref is still fine — it touches nothing on disk — so
				// the useful half stays available and only the switch is blocked.
				s_branchCheckout = false;
				ImGui::TextDisabled("Switching needs a clean project; the branch is created\n"
				                    "anyway and you can switch to it once you have committed.");
			}

			ImGui::Spacing();
			bool cancelled = false;
			const bool create = modalButtonRow(
				"Create", "Cancel", empty || !nameOk || taken || git->busy(), cancelled);
			if (create)
				git->requestCreateBranch(s_branchName, s_branchFromOid, s_branchCheckout);
			if (create || cancelled)
			{
				s_branchDialog = false;
				s_branchFromOid.clear();
				s_branchFromSubject.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}

	// ── Restore confirmation ─────────────────────────────────────────────────
	// Every file in the project changes at once, so the modal spells out the
	// full consequence AND the reassurance: the restore is recorded as a new
	// commit, so nothing in the history is lost and this itself is undoable.
	if (!s_restoreOid.empty()) ImGui::OpenPopup("Restore project?");
	beginModalSizing();
	if (ImGui::BeginPopupModal("Restore project?", nullptr,
	                           ImGuiWindowFlags_AlwaysAutoResize))
	{
		// Every paragraph here is already wrapped by hand; the guard is for the
		// next line somebody adds to this dialog, which will be a TextDisabled or
		// a TextColored and will otherwise be the one sentence that runs off the
		// edge — in the dialog that spells out an irreversible-looking change.
		{
			EditorWidgets::WrapText wrap;

			ImGui::TextWrapped("Every file in the project folder will be put back to how "
			                   "it was at this commit:");
			ImGui::Spacing();
			ImGui::TextWrapped("%s  %s", s_restoreOid.c_str(), s_restoreSubject.c_str());
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

			bool cancelled = false;
			const bool restore = modalButtonRow("Restore", "Cancel", git->busy(), cancelled,
			                                    /*danger=*/true);
			if (restore) git->requestRestoreTo(s_restoreOid, s_restoreOid);
			if (restore || cancelled)
			{
				s_restoreOid.clear();
				s_restoreSubject.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}

	ImGui::End();
#else
	(void)ctx; (void)open;
#endif
}

bool DrawFooterStatus(AppContext& ctx)
{
#ifdef HE_IMGUI_ENABLED
	GitController* git = ctx.git;
	// No project, no build support, or discovery has not finished: draw nothing
	// at all rather than a placeholder. A footer that permanently reads "no
	// repository" for people who do not use git is noise in every session.
	if (!git || !ctx.projectLoaded) return false;
	ensureSettingsLoaded(*git);

	const HE::Sc::RepoStatus& st = git->status();

	// dirtyCount(), hasConflicts() and hasStagedChanges() each walk the whole
	// file map. That is nothing for a handful of edits and quite a lot in a
	// freshly created project where every asset is still untracked — and the
	// footer asks every frame, forever. RepoStatus::generation exists precisely
	// so a consumer can tell "same answer as last time" without comparing maps.
	static std::uint64_t s_generation = ~0ull;
	static const void*   s_source     = nullptr;
	static std::size_t   s_dirty      = 0;
	static bool          s_conflicts  = false;
	static bool          s_staged     = false;
	// The generation restarts at 0 for a different project, so the identity of
	// the status object is part of the key — otherwise switching projects could
	// leave the previous one's counts on screen.
	if (s_generation != st.generation || s_source != static_cast<const void*>(&st))
	{
		s_generation = st.generation;
		s_source     = &st;
		s_dirty      = st.dirtyCount();
		s_conflicts  = st.hasConflicts();
		s_staged     = st.hasStagedChanges();
	}

	// Colour carries the state, the text carries the detail. Amber for "there is
	// uncommitted work", red for conflicts, muted for a clean tree — a status bar
	// that is bright when nothing is wrong trains people to ignore it.
	ImVec4      tint  = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	std::string label;
	std::string tip;

	if (!git->isRepo())
	{
		label = "No repository";
		tip   = "This project is not under source control.\nClick to open Source Control.";
	}
	else
	{
		const std::size_t dirty     = s_dirty;
		const bool        conflicts = s_conflicts;
		const std::string branch    = st.detached ? std::string("detached")
		                            : st.branch.empty() ? std::string("(no branch)")
		                                                : st.branch;

		label = branch + "   ";
		if (conflicts)
		{
			label += "conflicts";
			tint   = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
		}
		else if (dirty == 0)
		{
			label += "no changes";
		}
		else
		{
			label += std::to_string(dirty) + (dirty == 1 ? " change" : " changes");
			tint   = ImVec4(1.0f, 0.78f, 0.35f, 1.0f);
		}

		// Ahead/behind belongs in the tooltip rather than the line: it matters
		// when you go looking, and it is one more thing to read past when you do
		// not. Same for the upstream and the last error.
		tip = "Branch: " + branch;
		if (!st.upstream.empty()) tip += "\nUpstream: " + st.upstream;
		if (st.ahead > 0 || st.behind > 0)
		{
			tip += "\n" + std::to_string(st.ahead) + " to push, " +
			       std::to_string(st.behind) + " to pull";
		}
		if (st.initialCommit) tip += "\nNothing committed yet";
		tip += "\n" + std::to_string(dirty) + " changed file" + (dirty == 1 ? "" : "s");
		if (s_staged) tip += " (some staged)";
		if (!git->lastError().empty()) tip += "\n\n" + git->lastError();
		tip += "\n\nClick to open Source Control.";
	}

	// A flat, borderless button so the footer keeps reading as a status strip
	// rather than sprouting a toolbar.
	ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1, 1, 1, 0.20f));
	ImGui::PushStyleColor(ImGuiCol_Text,          tint);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 0.0f));

	const bool clicked = ImGui::Button(label.c_str());

	ImGui::PopStyleVar();
	ImGui::PopStyleColor(4);

	// Spelled out rather than SetTooltip, because this tooltip carries git's own
	// error text when there is one, and a git error is a full line of prose with
	// a path in it. A tooltip is sized by its longest line, so that one line
	// stretched the box across the whole screen — the panel's other lines then sat
	// alone on a strip of grey, which is the "cheap" look this pass is about. A
	// fixed column instead of the window edge for the reason a tooltip cannot be
	// asked where its edge is: it is wherever its contents put it.
	if (ImGui::IsItemHovered())
	{
		// Asked as a question for the reason imgui.h gives: EndTooltip belongs to a
		// BeginTooltip that returned true.
		if (ImGui::BeginTooltip())
		{
			{
				EditorWidgets::WrapText wrap(ImGui::GetFontSize() * 35.0f);
				ImGui::TextUnformatted(tip.c_str());
			}
			ImGui::EndTooltip();
		}
	}
	return clicked;
#else
	(void)ctx;
	return false;
#endif
}

} // namespace SourceControlPanel
