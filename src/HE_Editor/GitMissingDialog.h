#pragma once

struct AppContext;

// ── Startup source-control capability check (git, git-lfs, identity, helper) ──
// Reacts to the background probe (EditorApplication::startGitProbe), in the same
// shape as ToolchainDialog: self-triggering, no `open` flag, and the null
// pointer on AppContext means "still checking" so it never flashes during
// startup.
//
// Offers to install git (HE::Sc::runGitInstall through ctx.startGitInstall), but
// only when asked: installing git is several hundred megabytes and usually a
// password prompt, so it starts on "Install Git" and shows the installer's
// output while it runs — never in the background. Where the editor cannot do it
// (no package manager it knows, no way to get administrator rights, Windows
// without winget) the same spot says why and gives the command and the download
// page instead. git-lfs is not installed here; only Git for Windows brings it.
//
// It DOES fix the identity in place, because that one is a two-field form rather
// than an install: user.name / user.email being unset is the most common reason
// a first commit fails, and the remedy is one git config call.
namespace GitMissingDialog
{
	void DrawGitMissingDialog(AppContext& ctx);

	// Force the dialog open on the next completed probe even when the user
	// permanently dismissed it (an explicit request should always show the result).
	void requestShow();

	// "git is missing, here is how it gets here": the Install Git button with its
	// live log when the probe found a route the editor can run, otherwise the
	// reason plus the command to copy and the download page. Drawn wherever an
	// action needs git — this dialog, Preferences ▸ Source Control, the clone
	// dialog and the Source Control panel — so the first of them the user reaches
	// is where the install starts. Opens its own "Source Control" help scope.
	void drawGitInstallRemedy(AppContext& ctx);
	void drawLfsInstallRemedy();
}
