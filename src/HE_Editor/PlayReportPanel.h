#pragma once

struct AppContext;

// ── Post-PIE report ("Play Session Report") ──────────────────────────────────
// Opens automatically when a play session ends with captured warnings/errors
// (EditorApplication installs a Logger sink for the duration of play).
// Split out of EditorUI.cpp.
namespace PlayReportPanel
{
	void drawPlayReport(AppContext& ctx);
}
