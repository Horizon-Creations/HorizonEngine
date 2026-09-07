#pragma once

struct AppContext;

// ── Startup source-control capability check (git, git-lfs, identity, helper) ──
// Reacts to the background probe (EditorApplication::startGitProbe), in the same
// shape as ToolchainDialog: self-triggering, no `open` flag, and the null
// pointer on AppContext means "still checking" so it never flashes during
// startup.
//
// Deliberately does NOT offer an auto-install. Installing git is a
// several-hundred-megabyte, per-platform affair with its own privilege prompts,
// and getting it wrong leaves a half-installed toolchain; the dialog gives the
// exact command instead. git-lfs is the piece that will eventually ship with the
// editor, which removes the most common gap without installing anything.
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

	// Platform-specific install instructions (copy-a-command + open-a-page pair),
	// shared with the Preferences ▸ Editor ▸ Source Control page.
	void drawGitInstallRemedy();
	void drawLfsInstallRemedy();
}
