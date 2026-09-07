#pragma once

struct AppContext;

// ── Startup toolchain check (cmake + a working C++ compiler) ─────────────────
// Reacts to the background probe (EditorApplication::startToolchainProbe) and
// offers the Homebrew/winget auto-install path. Self-triggering — no `open`
// flag like the other dialogs; see the comment on DrawToolchainDialog.
// Split out of EditorUI.cpp; all of its state is file-static in the .cpp.
namespace ToolchainDialog
{
	void DrawToolchainDialog(AppContext& ctx);

	// Preferences ▸ Editor ▸ Tool Status ▸ "Fix" on cmake / C++ compiler: force the
	// dialog open even when the user permanently suppressed it — an explicit
	// user request should always show the result.
	void requestShow();
}
