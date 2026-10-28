#pragma once
#include <imgui.h>

struct AppContext;

// ── Project pages of the Preferences tab ─────────────────────────────────────
// Everything that belongs to the PROJECT rather than to the editor on this
// machine: what it is called, how its shadows and physics behave, what the
// packaged build boots with, which scripts its text needs, what its scripts may
// reach, which collision channels touch. These used to be a tab of their own
// (Edit ▸ Project Settings); they are pages of Preferences now, in its Project /
// World / Online groups, so every setting has ONE place. The difference is
// still WHERE a value goes: the editor pages write config.json on this machine,
// these write the project's .heproj and its Config/ProjectSettings.json, both of
// which travel with the project and into the build it exports.
//
// The page bodies live here (the Preferences tab only lists and calls them), so
// the headless UI test can draw one without the whole Preferences tab.
namespace ProjectSettingsPanel
{
	// Sub-pages. EditorSettingsPanel's rail lists them in three groups (Project,
	// World, Online); the enum is only an identity. The graphics rows and the
	// gamepad deadzones are not pages here: they are drawn once, on Preferences ▸
	// Graphics and Input, bound to the project's values when it sets its own.
	enum class Page
	{
		// Project — what it IS. (Its repository is on Accounts & Connections ▸
		// Source Control, next to git itself and the GitHub account.)
		General, Application, RenderDefaults, Fonts, Permissions,
		// World — how it simulates and sounds.
		Shadows, Simulation, CollisionLayers, AudioBuses,
		// Online — what a session of it is: AntiCheat is what its host refuses to
		// believe from a client ("anticheat"), Multiplayer the port, seats and
		// tick rate ("multiplayer").
		AntiCheat, Multiplayer,
	};

	// The rail label of a page ("Game Window" for RenderDefaults).
	const char* pageTitle(Page page);

	// Draw one page's body into the current window (no heading, no rail).
	void drawPage(AppContext& ctx, Page page);

	// Preferences ▸ Graphics: the rows named by `keys`, bound to the project's
	// own graphics when it sets them (the checkbox at the top switches that),
	// else to the editor's values. One set of rows, not one per owner.
	void drawGraphics(AppContext& ctx, const char* const* keys, int keyCount);

	// Preferences ▸ Input: the gamepad deadzone rows, bound the same way.
	void drawGamepad(AppContext& ctx);

	// Ask for the Preferences tab to open on a project page (usable from panels
	// drawn outside renderEditor). EditorUI consumes it once per frame and
	// forwards the page to EditorSettingsPanel.
	void requestOpen();
	void requestOpen(Page page);
	bool takeOpenRequest();
	Page requestedPage();
}
