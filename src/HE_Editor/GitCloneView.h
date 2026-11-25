#pragma once
// ─── "Clone from GitHub": what the dialog looks like ─────────────────────────
// The drawing half of GitCloneDialog, in the Project Hub's look: a header with
// the brand's horizon line, the signed-in account as a card, the repositories
// as rows of cards (searchable, clipped, constant height) and the target form.
// Plain data in, what was pressed out; no git, no threads, no AppContext —
// which is what lets tests/test_project_hub_ui.cpp draw the real thing headless.
//
// Gated on __has_include(<imgui.h>) like GitHubSignInView: the tests compile
// this file without HE_IMGUI_ENABLED.
#include <SourceControl/GitHubApi.h>   // RepoListEntry

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ImFont;

namespace GitCloneView
{
	// ── When the dialog loads the list on its own ────────────────────────────
	// Once per dialog open and once per sign-in (GitHubSignIn::accountGeneration),
	// never in a loop: a failed listing stays failed until Refresh, or it would
	// ask GitHub again every frame. A listing already running when the dialog
	// opens counts as this sign-in's load. Reset `state` when the dialog opens.
	struct AutoLoad { std::uint64_t generation = ~std::uint64_t(0); };
	bool shouldAutoLoad(AutoLoad& state, bool signedIn, std::uint64_t generation,
	                    bool listing, bool cloning);

	// Title, one line of explanation, the amber line under them. `width` is the
	// dialog's content width.
	void drawHeader(ImFont* subheading, float width);

	// "<login> · GitHub account  [Sign out]". True when Sign out was pressed.
	bool drawAccountCard(const std::string& login, bool busy, float width);

	enum class ListState { Loading, Ready, Failed };
	enum class ListAction { None, Picked, Refresh };
	// Search + Refresh, then the rows in a box `height` tall (the same height in
	// every state, so the dialog does not jump when the list arrives). A pick
	// writes the repository's fullName to `selected`.
	ListAction drawRepoList(ListState state, const std::vector<HE::Sc::RepoListEntry>& repos,
	                        char* filter, std::size_t filterSize, std::string& selected,
	                        bool disabled, float height);

	enum class TargetAction { None, Browse, Clone };
	// "Clone into": folder name and location side by side, the resulting path,
	// Clone. Disabled (but drawn) while nothing is picked, so it does not jump in.
	TargetAction drawTarget(char* parentDir, std::size_t parentSize,
	                        char* folderName, std::size_t nameSize,
	                        bool picked, bool disabled, bool emptyRepository);
}
