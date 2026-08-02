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

	// Preferences ▸ C++ Toolchain ▸ Recheck: force the dialog open on the next
	// completed probe even when the user permanently suppressed it — an explicit
	// user request should always show the result.
	void requestShow();
}
