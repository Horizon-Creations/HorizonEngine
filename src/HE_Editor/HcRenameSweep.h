#pragma once
#include "HcRename.h"

#include <Types/UUID.h>

#include <functional>
#include <string>
#include <vector>

class ContentManager;

// ── The rest of the project, when a HorizonCode member is renamed ────────────
// HcRename decides what ONE graph's share of a rename is. This walks the project
// and asks it of every graph there is, then writes the ones that answered.
//
// Graphs live in four places: a HorizonCode class asset, a widget asset, an
// animator state machine's sync graph, and a scene's level script (a plain JSON
// section inside the .hescene). The two the editor always holds in memory — the
// open scene's level script and the app-wide Game Instance — are NOT written
// here: their file on disk is stale by definition, so the caller plans and
// applies those itself and passes their paths in as `handledInMemory`.
//
// Nothing here draws: the dialog that shows the result and asks before writing
// is the panel's business. Splitting it that way is also what keeps the writing
// half honest — `scan` never writes, `apply` never decides.
namespace HcRenameSweep
{
	enum class Kind { Class, Widget, AnimatorSync, Scene };

	struct Entry
	{
		Kind           kind = Kind::Class;
		std::string    display;   // content-relative path, what the dialog lists
		std::string    path;      // how it is loaded again on apply
		HE::UUID       id{};      // asset kinds only; scenes are read as files
		HcRename::Plan plan;
		// Non-empty: this graph would change, but writing it here would take
		// somebody else's work with it (a collaborator's lock, an open tab with
		// unsaved edits). Reported, never written.
		std::string    skipWhy;

		bool writable() const { return plan.touches() && skipWhy.empty(); }
	};

	struct Report
	{
		std::vector<Entry> entries;
		// The walk could not finish (a directory it could not read). The list is
		// then a LOWER bound, and the dialog must not present it as the whole
		// answer.
		bool incomplete = false;

		int  assetsToWrite() const;   // entries that would actually be rewritten
		int  renameCount() const;     // nodes and declarations inside them
		int  unsureCount() const;     // named the old name, target unprovable
		bool anything() const;
	};

	// `classKey` and every class asset deriving from it, nearest or not. A call
	// on a Goblin reaches the function Enemy declared, so a rename on Enemy has
	// to follow calls aimed at Goblin too.
	std::vector<std::string> classAndDescendants(ContentManager& cm, const std::string& classKey);

	// Walk everything and plan it. `lockedOrDirty` is asked for each candidate and
	// returns why it must not be written, or "" when it may be.
	Report scan(ContentManager& cm, const HcRename::Target& t,
	            const std::vector<std::string>& targetKeys,
	            const std::vector<std::string>& handledInMemory,
	            const std::function<std::string(const std::string&)>& lockedOrDirty);

	// Write every writable entry. Returns how many assets were actually changed.
	int apply(ContentManager& cm, const Report& r, const HcRename::Target& t);
}
