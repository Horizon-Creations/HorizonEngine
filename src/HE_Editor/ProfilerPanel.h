#pragma once

struct AppContext;

// ── Performance Profiler window (View ▸ Performance Profiler) ────────────────
// The editor front-end of the runtime EngineProfiler: the live HUD + frame-time
// graph, the benchmark/single-frame capture controls and the per-pass GPU +
// per-scope CPU breakdown of one captured frame.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace ProfilerPanel
{
	// `open` is the View-menu toggle; the window clears it when closed and turns
	// the profiler's live sampling off while it is not shown.
	void DrawProfilerWindow(AppContext& ctx, bool& open);
}
