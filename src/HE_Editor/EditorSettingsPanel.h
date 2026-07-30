#pragma once

struct AppContext;

// ── Engine settings catalog + Preferences window (Edit ▸ Preferences) ────────
// One catalog of pinnable engine settings, rendered in two modes: Preferences
// shows every setting with a "pin" toggle; Quick Settings shows only the pinned
// ones. Favourites are a comma-separated list of stable keys in
// EditorConfig::QuickSettingsFavorites (persisted to config.json). (Scene
// environment settings are NOT here — those live on the World entity.)
// Split out of EditorUI.cpp; the Quick-Settings dock panel calls
// DrawEngineSettings directly, which is why both entry points are public.
namespace EditorSettingsPanel
{
	enum class SettingsMode { Preferences, QuickSettings };

	// Renders the engine-settings catalog. Each `row(key, category, widget)` is a
	// logical setting group; `widget` draws its control(s).
	void DrawEngineSettings(AppContext& ctx, SettingsMode mode);

	// `open` is a one-shot request raised by the Edit menu / Ctrl+, shortcut;
	// the window consumes it and turns it into a modal popup.
	void DrawPreferencesWindow(AppContext& ctx, bool& open);
}
