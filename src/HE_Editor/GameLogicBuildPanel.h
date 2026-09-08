#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct AppContext;

// ── Build ▸ Build and Reload Game Logic ──────────────────────────────────────
// A C++ project authors gameplay as a native GameLogic library (ProjectManager.h);
// this is the button that compiles it and swaps it into the running preview
// without the play session ending.
//
// Shape deliberately copied from ExportDialogPanel: the compile is blocking, so
// it runs on a worker thread and reports into the shared Build window
// (BuildProgressDialog, started with Kind::GameLogic so the two runs cannot take
// each other's buttons). All state is file-static in the .cpp.
//
// The division of labour between the two threads is the part that matters: the
// WORKER only compiles. The reload — onStop, dlopen, re-inject, onStart — runs
// in render(), on the UI thread, because it touches the world. A worker that
// swapped the module in itself would be mutating the ECS from under the frame
// being drawn.
namespace GameLogicBuildPanel
{
	// Where a project's module lives, what it is called and how it is built is
	// NOT here: it is HE::hccg::gameLogicSourceDir / gameLogicBuildDir /
	// builtGameLogic / gameLogicSpec, next to the cmake run that uses it. This
	// panel is the button, the worker and the window.

	// Whether the menu row does anything here: a C++ project with a
	// Source/CMakeLists.txt. False for every other language and for no project.
	bool available(const AppContext& ctx);

	// Menu action. Compiles the module and, if a play session is running, swaps
	// it in. Refuses (with a notification) while another run owns the Build
	// window.
	void start(AppContext& ctx);

	// Per-frame: reaps the finished worker, runs the reload and closes the run
	// out. Must be called every frame, not only while the window is open — a
	// finished compile has to be joined either way. Call it AFTER
	// ExportDialogPanel::render, so the two poll the Build window's action in a
	// defined order.
	void render(AppContext& ctx);

	// True while the compile worker is running.
	bool running();

	// Blocks until the worker has finished. Editor shutdown must call it —
	// destroying a joinable std::thread terminates the process.
	void joinPendingBuild();
}
