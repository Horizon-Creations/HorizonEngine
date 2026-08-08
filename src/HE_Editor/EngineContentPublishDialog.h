#pragma once

struct AppContext;

// ─── "Content ▸ Publish Engine Content to Server…" ────────────────────────────
// Dev-only tool (see ContentManager::isEngineContentDevMode) that runs
// HE::Cs::publishEngineContentBlocking on a worker thread and shows its log
// live. All state is file-static in the .cpp, same arrangement as
// ToolchainDialog — this dialog opens itself once requested, no AppContext
// field needed to track "is it open".
namespace EngineContentPublishDialog
{
	// Opens the dialog and starts the publish. No-op if a publish is already running.
	void open(AppContext& ctx);

	// Draws the dialog if open. Call once per frame from the Editor's popup pass.
	void Draw(AppContext& ctx);
}
