#pragma once

struct AppContext;

// ── Help ▸ Check for Updates… ────────────────────────────────────────────────
// The window around EditorSelfUpdate::Session (see EditorSelfUpdate.h for the
// order of events and who decides what). Opening it starts a check; the user
// then downloads, and "Restart to Update" closes the window and leaves a
// one-slot request that EditorUI turns into requestGuarded(GuardedAction::
// InstallUpdate) — the same unsaved-changes prompt as Quit. Only after the
// prompt has been answered with Save or Don't Save does beginInstall() run.
namespace SelfUpdateDialog
{
	// False in a build without libssh2 (no HorizonSelfUpdate): the menu rows are
	// left out and everything below is a no-op.
	bool available();

	// Open the dialog and start a check (Help ▸ Check for Updates…, both menu bars).
	void open();
	bool isOpen();

	// Draw. Safe to call every frame; draws nothing while closed.
	void Draw(AppContext& ctx);

	// "Restart to Update" was pressed: true once, then false until pressed again.
	bool takeInstallRequest();

	// The guarded action's body: start the takeover (HorizonUpdater with the open
	// project as the relaunch argument) on the session's worker. The dialog stays
	// open with "Installing" until it has an outcome; nothing quits here.
	void beginInstall(AppContext& ctx);

	// Every frame, wherever beginInstall can have been called from: true once the
	// takeover has started HorizonUpdater, and the caller must quit now. A failed
	// takeover leaves the install untouched and the dialog open with the reason.
	bool finishInstall();

	// Editor start: delete what earlier updates left beside the install, keeping
	// the one previous install the running build was swapped in from
	// (Takeover.h, pruneTakeoverLeftovers). On a worker — it can be a few
	// hundred megabytes — joined by joinPendingWork().
	void startHousekeeping();

	// Editor shutdown: a check, a download or the housekeeping may still be running.
	void joinPendingWork();

	// ── Acceptance driver (scripts/selfupdate_editor_e2e.py) ─────────────────
	// HE_SELFUPDATE_E2E=save|discard|cancel with HE_SELFUPDATE_E2E_TRIGGER=<file>:
	// once that file appears the dialog opens and presses its own Download and
	// Restart to Update buttons, and the unsaved-changes prompt that follows is
	// answered with the named button — the same code paths a click takes, on a
	// machine where nobody may click (a CI runner, a hidden editor). Never set for
	// a user; without both variables nothing here does anything.
	enum class PromptAnswer { None, Save, Discard, Cancel };
	// The answer for the prompt that guards InstallUpdate: once, then None.
	PromptAnswer takeE2EPromptAnswer();
}
