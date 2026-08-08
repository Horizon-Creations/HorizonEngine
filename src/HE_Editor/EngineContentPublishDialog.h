#pragma once

struct AppContext;

// ─── "Assets ▸ Publish…" / "Assets ▸ Rebuild Manifest from Server…" ───────────
// Dev-only tools (see ContentManager::isEngineContentDevMode) that run
// HE::Cs::publishEngineContentBlocking / rebuildManifestFromServerBlocking on a
// worker thread and show the log live. Both share one dialog (only one can
// meaningfully run at a time — they both write manifest.json). All state is
// file-static in the .cpp, same arrangement as ToolchainDialog — this dialog
// opens itself once requested, no AppContext field needed to track "is it open".
namespace EngineContentPublishDialog
{
	// Publishes the local EngineContent folder to the server. No-op if a run
	// (either kind) is already in progress.
	void open(AppContext& ctx);

	// Rebuilds manifest.json from what the server already has, via a remote
	// directory listing — for content that reached the server some other way
	// (see EngineContentPublish.h). No-op if a run is already in progress.
	void openRebuildFromServer(AppContext& ctx);

	// Draws the dialog if open. Call once per frame from the Editor's popup pass.
	void Draw(AppContext& ctx);

	// True exactly once after a run finished SUCCESSFULLY (the flag is cleared by
	// the call). Both operations rewrite the server's manifest.json, so whoever
	// drains this must re-fetch the manifest — otherwise the Editor keeps serving
	// the catalogue it read at startup and shows assets that are no longer there.
	bool takeRunSucceeded();

	// Joins the worker thread. MUST be called before the process tears down
	// statics: startRun only joins the previous run's thread when a NEW run
	// starts, so the last run of a session leaves a joinable std::thread whose
	// destructor would call std::terminate.
	void shutdown();
}
