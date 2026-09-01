#pragma once
#include "HcRename.h"

#include <string>
#include <vector>

struct AppContext;

// ── "This rename reaches other files. Here is which." ────────────────────────
// Renaming a HorizonCode member inside its own graph is an edit like any other:
// undoable, confined, invisible until saved. Renaming it across the project is
// not. It writes files the user did not open, some of which are in version
// control and one of which a colleague may be editing right now, and the undo
// stack of the graph they are standing in cannot take any of it back.
//
// So it is asked first, and asked with the list in hand rather than in the
// abstract: these assets, that many places, these ones skipped and why, and
// these ones that name the old name without the graph being able to prove whom
// they meant. The last group is the point of the whole exercise — those are
// exactly the places that used to break in silence.
//
// Called right after a rename was committed in a graph editor. When the sweep
// finds nothing, nothing is shown at all.
namespace HcRenameDialog
{
	// `handledInMemory` are the graph keys the editor holds open and has already
	// renamed itself — their file on disk is stale, so the sweep must not read it.
	void requestAfterRename(AppContext& ctx, const HcRename::Target& t,
	                        const std::vector<std::string>& handledInMemory);

	// Drawn once per frame from EditorUI, like the other editor-wide dialogs.
	void Draw(AppContext& ctx);
}
