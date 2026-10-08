#pragma once

#include <string>

struct AppContext;

// ── Clone a repository from the user's own GitHub account as a project ───────
// The counterpart of Preferences ▸ Source Control's "Create & push": that one
// takes a project and makes a repository of it, this one takes a repository
// and makes a project of it. GitHub sign-in or token → the account's repository
// list (searchable) → a folder → clone (LFS assets included) → the project opens.
//
// Raised from the Project Hub and from the Preferences page, and drawn once for
// both screens (EditorUI, next to GitMissingDialog), because a clone is what
// someone does BEFORE they have a project open.
//
// The dialog never opens the project itself. Opening has two different paths —
// the Hub loads directly, the editor has to ask about unsaved work and end the
// current session first — so the dialog only leaves the .heproj path behind and
// each screen takes it through its own path (takeOpenRequest).
//
// The token follows the source-control rules: typed into a password field,
// handed to GitHub's API in a header and to git's credential helper for the
// clone, never placed in a URL, and wiped from the field once the clone starts
// or the dialog closes.
namespace GitCloneDialog
{
	// Open the dialog on the next frame.
	void requestOpen();

	void Draw(AppContext& ctx);

	// The project file of a finished clone, once, for the caller's open path.
	bool takeOpenRequest(std::string& heprojPath);
}
