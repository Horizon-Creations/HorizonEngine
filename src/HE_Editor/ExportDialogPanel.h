#pragma once

#include <string>

struct AppContext;
struct ExportProfile;

// ── Build ▸ Export Project ────────────────────────────────────────────────────
// The export dialog: profile picker, the editable mirror of the selected
// ExportProfile, the build-step/log view, and the worker thread that packs the
// project (HorizonCode → C++ codegen, scene serialisation, ProjectExporter).
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp and
// reached only through these three entry points.
namespace ExportDialogPanel
{
	// Menu action (Build ▸ Export Project…): fill the dialog fields from the
	// project's active profile, collect the startup-scene choices, raise the modal.
	void open(AppContext& ctx);

	// Per-frame: reaps a finished worker and draws the modal. Must be called every
	// frame from the editor UI, not only while the popup is open — a completed
	// export has to be joined even if the user closed the popup.
	void render(AppContext& ctx);

	// Run an export from this profile WITHOUT the modal — the door an MCP client
	// comes through (McpToolsBuild.cpp, `project_package`).
	//
	// The profile is taken BY VALUE on purpose: a client may override any of its
	// fields for one run, and it patches a copy. Nothing here writes a profile
	// back into the project or calls saveProject — that is what the dialog's own
	// Save button does, with a human looking at it.
	//
	// Does exactly what pressing Export in the dialog does otherwise: it fills
	// the same dialog mirror (so the settings a human opens next are the ones
	// that just ran), reloads the window/backend rows from the editor config the
	// same way open() does, and hands the job to the same worker reporting into
	// the same BuildProgressDialog.
	//
	// Returns false with `outError` only for the two things that stop a run from
	// STARTING: no project (or no content manager) and a run already going.
	// Everything that can go wrong once it has started — no runtime bundle for
	// the target, a startup scene that no longer loads, a failing toolchain — is
	// a FAILED run in the Build window, exactly as it is when a human pressed the
	// button, and is read back from there.
	bool startFromProfile(AppContext& ctx, const ExportProfile& profile,
	                      std::string* outError = nullptr);

	// True while the modal was on screen during the last render() call. Only a
	// status read — the interactive tutorial watches it to notice that the user
	// opened Build ▸ Export Project.
	bool isOpen();

	// Blocks until a project export running on the worker thread has finished.
	// Must be called on editor shutdown — destroying a joinable std::thread
	// terminates the process.
	void joinPendingExport();
}
