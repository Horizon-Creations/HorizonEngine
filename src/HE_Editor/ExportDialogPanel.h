#pragma once

struct AppContext;

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

	// Blocks until a project export running on the worker thread has finished.
	// Must be called on editor shutdown — destroying a joinable std::thread
	// terminates the process.
	void joinPendingExport();
}
