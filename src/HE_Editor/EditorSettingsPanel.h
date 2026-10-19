#pragma once
#include <imgui.h>
#include "ProjectSettingsPanel.h"

struct AppContext;
struct EditorConfig;

// ── Preferences tab + engine-settings catalog ────────────────────────────────
// The Preferences UI is a full editor tab (Edit ▸ Preferences, Ctrl+,): a
// category navigation on the left (Editor / Project / World / Online / Preview /
// Accounts & Connections / Tools, each with pages) and the selected page's
// settings on the right. It replaced the old modal popup. The Editor, Preview,
// Accounts and Tools groups follow the EDITOR from project to project; the
// Project, World and Online groups edit the open PROJECT (their bodies are
// ProjectSettingsPanel's) and write into it, not into config.json.
//
// The engine-settings catalog is shared with the Quick Settings dock panel:
// every setting row carries a "pin" toggle in the Preferences tab, and Quick
// Settings shows only the pinned rows. Favourites are a comma-separated list
// of stable keys in EditorConfig::QuickSettingsFavorites (persisted to
// config.json). (Scene environment settings are NOT here — those live on the
// World entity.)
namespace EditorSettingsPanel
{
	// Project: the same rows drawn by a Project Settings page, bound to a
	// config the caller hands in (DrawOptions::config) — no pin gutter, no
	// category headings, never locked.
	enum class SettingsMode { Preferences, QuickSettings, Project };

	// Sub-pages of the Preferences tab (the left-hand navigation buttons), in the
	// order the rail lists them. The Project / World / Online pages at the end of
	// the enum belong to the open project (ProjectSettingsPanel.h); the rest are
	// about THIS editor and THIS user.
	enum class Page
	{
		// Editor — how the editor behaves and looks.
		// Appearance carries the graph editors' and the widget designer's own
		// switches under its rows (what used to be the Panels page). Input is the
		// pointer, the gamepad and the keyboard: every editor-wide chord,
		// rebindable. ProjectData is what the editor keeps about a project as it
		// works on it: the content browser's scan and cache, and the autosave.
		Appearance, Viewport, Input, ProjectData, Feedback,
		// Preview — what the viewport is drawn with on this machine. EditorRenderer
		// is the editor's own backend and frame pacing; ViewportQuality is the
		// user's own graphics values — what a project that sets none previews
		// with, and an override for one that does (a machine that cannot carry
		// the project's bloom and GI).
		EditorRenderer, ViewportQuality,
		// Accounts & Connections — who you are and what the editor talks to.
		// SourceControl is git on this machine and the GitHub account (the
		// repository of a PROJECT is on its Project Settings tab). Team is the
		// team server and the session settings. RemoteControl is the MCP bridge:
		// a program on this machine driving this editor, which is a different
		// question from a collaborator, who is a person who was invited.
		SourceControl, Team, RemoteControl,
		// Tools — the status of the outside tools the editor needs.
		Status,
		// Project / World / Online — one page per ProjectSettingsPanel::Page.
		ProjectGeneral, ProjectApplication, ProjectWindow, ProjectFonts,
		ProjectPermissions, ProjectRepository,
		WorldShadows, WorldSimulation, WorldCollisionLayers, WorldAudioBuses,
		OnlineAntiCheat, OnlineMultiplayer,
	};

	// Open the tab on the page that draws `page` (what a "set it up in the
	// project" button asks for; EditorUI forwards ProjectSettingsPanel's request).
	void requestOpenProjectPage(ProjectSettingsPanel::Page page);

	// Sentinel "asset path" identifying the Preferences tab (no backing .hasset).
	constexpr const char* kTabPath = "::Preferences::";

	// How the HorizonCode graph editor spells a variable in its variable list:
	// Detailed puts the name and the (coloured) type on two lines, Compact puts
	// both on one. Set on the Editor ▸ Panels page, under HorizonCode.
	enum class HcVariableStyle { Detailed = 0, Compact = 1 };

	// The setting, read back for whoever draws the list. An accessor rather than
	// a shared key so the graph editor never has to know where this is persisted,
	// and so an absent or damaged value has ONE place to fall back to Detailed.
	HcVariableStyle hcVariableStyle();

	// Whether the widget designer runs the widget's Pre Construct on its canvas
	// (sandboxed, never saved). On by default, like UMG. Set on the Editor ▸
	// Panels page, under Widgets; read by the designer every frame so the
	// checkbox and the canvas cannot disagree.
	bool widgetRunPreConstruct();

	// Renders the engine-settings catalog. Each `row(key, category, widget)` is a
	// logical setting group; `widget` draws its control(s). `categoryFilter`
	// limits the output to one category (used by the Preferences pages); null
	// draws everything (Quick Settings).
	void DrawEngineSettings(AppContext& ctx, SettingsMode mode,
	                        const char* categoryFilter = nullptr);

	// The general form. A page shows the rows of one category (`category`), or
	// exactly the rows named in `rows` whatever their category (the pages are not
	// the catalogue's categories any more: Viewport Quality draws rows from three
	// of them). `config` is the config the widgets edit — the editor's own by
	// default, a scratch copy on the Project pages, which write it back
	// into the project's block afterwards.
	struct DrawOptions
	{
		const char*        category = nullptr;
		const char* const* rows     = nullptr;
		int                rowCount = 0;
		EditorConfig*      config   = nullptr;
	};
	void DrawEngineSettings(AppContext& ctx, SettingsMode mode, const DrawOptions& options);

	// The repository half of source control (init, remote, GitHub setup, large
	// files): the repository belongs to the PROJECT, so it is drawn on the
	// Project ▸ Repository page and not on Accounts & Connections ▸ Source Control.
	void DrawRepositorySection(AppContext& ctx);

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
