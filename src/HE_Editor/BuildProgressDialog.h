#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct AppContext;

// The window an export is watched in: the steps as a row of rings across the
// top, each filling with its own progress, and below it the log of whichever
// step is selected. It replaces the step list and output box that used to sit
// at the bottom of the export settings dialog — those grew downwards while a
// build ran and pushed the buttons off the screen, and a compiler error was one
// undifferentiated stream with the pack log.
//
// Two halves, deliberately split: the `Build` namespace is the model, written
// from the export worker thread; the dialog itself only reads it. Everything
// here is thread-safe and file-static in the .cpp — one export runs at a time.
namespace BuildProgressDialog
{
	// ── Model: called from the export worker (any thread) ────────────────────
	namespace Build
	{
		// Start a run. Clears the previous run's steps and log. UI thread.
		void begin(const std::vector<std::string>& stepNames);

		// Move to a step; everything logged from here on belongs to it.
		void stepBegin(int index);

		// How far the current step is. `fraction` outside [0,1] means "running,
		// length unknown" — the ring spins instead of filling, which is what a
		// cmake configure phase or a codesign honestly is.
		void stepProgress(float fraction);
		void stepProgress(int done, int total);

		// A log line for the current step. severity: 0 info, 1 warning, 2 error.
		void log(int severity, const std::string& text);

		// What the current step is doing right now — the file being packed, the
		// class being translated. One line under the buttons, not the log.
		void setActivity(const std::string& what);

		// Mark a step failed without ending the run: HorizonCode codegen can fail
		// and the export still ships (interpreted), so the ring must be able to go
		// red while the build carries on.
		void stepFailed(int index);

		// End the run. A failure marks the running step failed and selects it.
		void finish(bool success, const std::string& message);

		// Where the finished export put the game, and whether this machine can
		// run it (false for a cross-platform target). Set before finish().
		void setLaunchTarget(const std::filesystem::path& executable, bool runnableHere);

		bool running();
	}

	// ── Dialog ───────────────────────────────────────────────────────────────

	// What the user asked for on the finished run. Polled once per frame by the
	// export panel, which owns the settings and the worker.
	enum class Action
	{
		None,
		Rebuild,    // run the same export again
		BackToSetup // reopen the export settings dialog
	};

	// Show the dialog on the next frame (the export panel calls this when it
	// starts a run, from outside its own popup — a popup opened inside another
	// popup becomes its child and dies with it).
	void requestOpen();

	// Draw it. Call once per frame at the top level, not inside another popup.
	void render(AppContext& ctx);

	// Take the pending action, if any (resets it).
	Action takeAction();

	// Whether the dialog drew this frame (the editor blocks input behind it).
	bool isOpen();
}
