#pragma once
#include <imgui.h>

struct AppContext;

// ── Preferences tab + engine-settings catalog ────────────────────────────────
// The Preferences UI is a full editor tab (Edit ▸ Preferences, Ctrl+,): a
// category navigation on the left (General / Rendering / Source Control /
// Tools, each with sub-pages) and the selected page's settings on the right.
// It replaced the old modal popup.
//
// The engine-settings catalog is shared with the Quick Settings dock panel:
// every setting row carries a "pin" toggle in the Preferences tab, and Quick
// Settings shows only the pinned rows. Favourites are a comma-separated list
// of stable keys in EditorConfig::QuickSettingsFavorites (persisted to
// config.json). (Scene environment settings are NOT here — those live on the
// World entity.)
namespace EditorSettingsPanel
{
	enum class SettingsMode { Preferences, QuickSettings };

	// Sub-pages of the Preferences tab (the left-hand navigation buttons).
	enum class Page
	{
		// General
		Appearance, Viewport, ContentBrowser,
		// Rendering
		Display, PostProcessing, GlobalIllumination, Effects,
		// Collaboration
		CollabGeneral,
		// Source Control (git install status and the repository share one page)
		Repository,
		// Tools
		Status,
	};

	// Sentinel "asset path" identifying the Preferences tab (no backing .hasset).
	constexpr const char* kTabPath = "::Preferences::";

	// Renders the engine-settings catalog. Each `row(key, category, widget)` is a
	// logical setting group; `widget` draws its control(s). `categoryFilter`
	// limits the output to one category (used by the Preferences pages); null
	// draws everything (Quick Settings).
	void DrawEngineSettings(AppContext& ctx, SettingsMode mode,
	                        const char* categoryFilter = nullptr);

	// Fill the given tab rect with the Preferences tab (nav + content).
	void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size);

	// Ask EditorUI to open (or focus) the Preferences tab — usable from panels
	// drawn outside renderEditor (e.g. the Source Control window's "set up the
	// remote in Preferences" pointer). The overload with a Page also switches to
	// that page. Consumed once per frame by the tab strip.
	void requestOpen();
	void requestOpen(Page page);
	bool takeOpenRequest();

	// True while a Source Control page was drawn this frame — the Source Control
	// window ORs this into GitController::setPanelVisible so the fast status
	// poll also runs while the user is looking at the Preferences pages.
	bool sourceControlPageActive();
}
