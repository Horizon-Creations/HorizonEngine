#pragma once

struct AppContext;

// ── Missing-tools setup: one dialog for everything the editor needs from outside ──
// cmake and a C++ compiler (HorizonCode export, native GameLogic) and git, Git LFS
// and a git identity (source control). It replaces the two startup dialogs that used
// to say the same thing in two shapes (ToolchainDialog, GitMissingDialog).
//
// One row per tool: a state icon, what the tool is for, which package the install
// takes and what the user will see while it runs (UAC, a password dialog, Apple's
// installer), and the one action that fits the state. Everything installs through the
// system's package manager (winget, Homebrew / Apple's Command Line Tools, apt / dnf /
// pacman / zypper behind pkexec); the commands for doing it by hand are folded away.
//
// Package managers report no usable percentage, so a running install shows its
// phase (Preparing, Package manager, Verifying), an indeterminate bar and the time
// it has been running; the raw output is one fold away under "Install log".
//
// The two installers it drives are the ones that already exist and are unchanged:
// AppContext::startToolchainInstall (HcCodegen::installToolchain) and
// startGitInstall / startGitLfsInstall (HE::Sc::runGitInstall). They have their own
// worker, log and flags, so this dialog runs one install at a time and queues the
// rest ("Install all missing").
namespace ToolSetupDialog
{
	enum class Tool { None, Cmake, Compiler, Git, GitLfs, GitIdentity };

	enum class RowState
	{
		Checking,    // the probe has not answered yet (or a recheck is running)
		Ok,          // found
		Missing,     // not found, and there is something to press
		Installing,  // its installer is running
		Failed,      // its installer ended and the tool is still not there
		Waiting,     // cannot be dealt with yet: Git LFS and the identity need git first
	};

	// Called every frame from EditorUI. Opens itself once when a probe finds
	// something missing (unless the user silenced it), and when requestShow() asks.
	void DrawToolSetupDialog(AppContext& ctx);

	// Opens the dialog on the next frame even when the startup warning was
	// silenced. `focus` flashes that tool's row, so a banner's "Set Up…" lands on
	// the line it is about.
	void requestShow(Tool focus = Tool::None);

	// The same, opened on behalf of something that waits for the tool: `purpose`
	// is one sentence shown above the rows (the export says it continues by
	// itself once the compiler is there). It stays until the dialog closes.
	void requestShowFor(Tool focus, const char* purpose);
	const char* purpose();   // "" when the dialog was not opened for anything

	// True while the dialog's popup was on screen in the last DrawToolSetupDialog.
	// A caller that parked work behind requestShowFor watches it go false again.
	bool isShowing();

	// The rows and the footer, inside whichever window is current. DrawToolSetupDialog
	// wraps this in its popup; the headless UI test hosts it in a plain window.
	// `closeRequested` is set when the user pressed Close (or everything is set up).
	void drawToolSetup(AppContext& ctx, bool* closeRequested = nullptr);

	// An inline "this is missing" notice with a "Set Up…" button that opens the
	// dialog on the tool's row. Draws nothing when the tool is found or the probe is
	// still running, so a caller can use it unconditionally where one tool matters.
	// While that tool installs it shows the running time instead of the button.
	void drawMissingBanner(AppContext& ctx, Tool tool, const char* message);

	RowState rowState(AppContext& ctx, Tool tool);

	// Forgets everything the dialog remembers between frames (cached probes, the
	// install queue, acknowledged failures). The dialog keeps its state in the file,
	// like the dialogs before it; the headless tests start each case from clean.
	void resetStateForTests();
}
