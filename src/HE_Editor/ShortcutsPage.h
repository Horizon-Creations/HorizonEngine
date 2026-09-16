#pragma once

// ── Preferences ▸ Editor ▸ Shortcuts ─────────────────────────────────────────
// The keyboard, as a list: every editor-wide chord from EditorShortcuts, one
// row each, grouped the way the table groups them. The chord is a button;
// pressing it arms a capture, and the next key that goes down (with whatever
// modifiers are held) becomes the binding — Esc backs out, Backspace unbinds.
// Two actions that could fire on the same keystroke are painted red with the
// other's name, because a clash the page does not show is one the user meets
// in the viewport instead.
//
// A module of its own rather than a function inside EditorSettingsPanel.cpp:
// the settings tab pulls in the toolchain dialog, the router probe and the
// rest of the editor, and a page whose only inputs are ImGui and the shortcut
// table can be driven headless — clicked, typed into, screenshotted — when it
// does not carry that along (tests/test_shortcuts_ui.cpp).
//
// Written through GlobalState on every change, like the HorizonCode page:
// a binding is chosen once and should not be lost to a crash before exit.

#ifdef HE_IMGUI_ENABLED
namespace ShortcutsPage
{
	// The config.json key the overrides live under (one string, see
	// EditorShortcuts::encode). EditorApplication reads it at startup and
	// writes it again at exit; this page writes it on every change.
	inline constexpr const char* kConfigKey = "Keybindings";

	// Draws the page into the current window at the cursor.
	void draw();

	// The action a click armed for capture, "" when idle. Exposed for the
	// test; the page keeps it, EditorShortcuts does not know about it.
	const char* capturing();
}
#endif // HE_IMGUI_ENABLED
