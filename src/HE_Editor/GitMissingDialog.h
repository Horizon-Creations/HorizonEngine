#pragma once

struct AppContext;

// ── The "git is missing" remedy, drawn in place ──────────────────────────────
// The startup dialog that used to live here (and ToolchainDialog, its twin) is now
// ToolSetupDialog: one list for cmake, the compiler, git, Git LFS and the git
// identity. What stays is the remedy itself, for the places that need git or Git
// LFS right now and show it where the user already is: Preferences » Source Control,
// the clone dialog, and the Source Control panel.
//
// Offers to install (HE::Sc::runGitInstall through ctx.startGitInstall /
// startGitLfsInstall), but only when asked: installing is hundreds of megabytes and
// usually a password prompt, so it starts on "Install Git" / "Install Git LFS" and
// shows the installer's output while it runs — never in the background. Where the
// editor cannot do it (no package manager it knows, no way to get administrator
// rights, Windows without winget) the same spot says why and gives the command and
// the download page instead.
namespace GitMissingDialog
{
	// "git is missing, here is how it gets here": the install button with its live
	// log when the probe found a route the editor can run, otherwise the reason plus
	// the command to copy and the download page. Opens its own "Source Control" help
	// scope, so it resolves the same entries wherever it is drawn.
	void drawGitInstallRemedy(AppContext& ctx);
	void drawLfsInstallRemedy(AppContext& ctx);
}
