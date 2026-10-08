#pragma once
#include <functional>
#include <string>

class ContentManager;
struct AppContext;
namespace HorizonCode { struct Graph; struct Variable; }

// ── Pull on Construct in the variable details (design §2.9) ──────────────────
// One drawing for both copies of the variable details — LevelScriptPanel's and
// the widget editor's — so the two cannot drift on this block the way the rest
// of those copies is allowed to. The decisions live in HcPull; this file only
// draws them and writes "Add to Target" into the source.
namespace HcPullUi
{
	// What the block needs from the application. The level-script details are
	// reached without an AppContext (drawGraphBody hands down a ContentManager
	// and nothing else), so the panel entry points set this once per frame,
	// before they draw. Writing a SOURCE is the only thing that needs it: the
	// Game Instance graph is live in memory and goes through its commit, a class
	// asset is written through the ContentManager.
	struct Targets
	{
		HorizonCode::Graph*   gameInstance = nullptr;
		std::function<void()> commitGameInstance;
		ContentManager*       content = nullptr;
		// Why a class asset must not be written from here, or "" — the rename
		// dialog's rule (an open tab with unsaved changes would save its own
		// copy over ours).
		std::function<std::string(const std::string& relPath)> blockedBecause;
	};
	void setTargets(Targets t);
	// setTargets from the application's context, with the rename dialog's rule
	// for class assets. What the panel entry points call (HcPullUiBind.cpp).
	void bindFrom(AppContext& ctx);

	// Draw the block for `v`, an instance variable of `owner`. Not shown for
	// a function-local, nor in the Game Instance's own graph (it never pulls,
	// design §2.3). True when `v` changed; a change to a SOURCE graph is
	// committed by the block itself.
	bool drawSection(HorizonCode::Variable& v, const HorizonCode::Graph& owner);

	// The label of the default-value section: "Fallback" while the variable
	// pulls, because that is what its default has become.
	const char* defaultSectionLabel(const HorizonCode::Variable& v);

	// The variable list's mark: the type line with "pulled" after it, as
	// variableRow's `note` (empty = draw the row as always), and the source as
	// the row's tooltip — call listTooltip right after the row.
	std::string listNote(const HorizonCode::Variable& v);
	void        listTooltip(const HorizonCode::Variable& v);
}
