#pragma once
#include <imgui.h>

struct AppContext;

// ── Project Settings tab ─────────────────────────────────────────────────────
// Everything that belongs to the PROJECT rather than to the editor on this
// machine: what it is called, how its shadows and physics behave, what the
// packaged build boots with, which scripts its text needs, what its scripts may
// reach, which collision channels touch. A full editor tab (Edit ▸ Project
// Settings…), laid out like Preferences — a category rail on the
// left, the selected page on the right — because the two are the same kind of
// surface and should be operated the same way. The difference is WHERE a value
// goes: Preferences writes config.json on this machine, this tab writes the
// project's .heproj and its Config/ProjectSettings.json, both of which travel
// with the project and into the build it exports.
//
// The four pages that used to sit under "Project" in Preferences (Application,
// Permissions, Fonts, Collision Layers) live here now, unchanged; their help
// scopes ("Application/…", "Permissions/…") are theirs and moved with them.
namespace ProjectSettingsPanel
{
	// Sentinel "asset path" identifying the tab (no backing .hasset).
	constexpr const char* kTabPath = "::ProjectSettings::";

	// Sub-pages, in the order the rail lists them.
	enum class Page
	{
		// Game — what the project IS
		General, Application, Permissions, Fonts,
		// Rendering — how it draws
		RenderDefaults, Shadows,
		// Physics — how it simulates
		Simulation, CollisionLayers,
		// Audio — the mixer's buses live in their own window; the page points
		// there rather than duplicating the strip.
		AudioBuses,
	};

	// Fill the given tab rect with the tab (rail + content).
	void render(AppContext& ctx, const ImVec2& pos, const ImVec2& size);

	// Ask EditorUI to open (or focus) the tab — usable from panels drawn
	// outside renderEditor. The overload with a Page also switches to that
	// page. Consumed once per frame by the tab strip.
	void requestOpen();
	void requestOpen(Page page);
	bool takeOpenRequest();

	// The Audio ▸ Buses page's "Open Audio Mixer": the mixer's open flag lives
	// in EditorUI with the other tool windows, so the tab raises a request and
	// EditorUI consumes it once per frame.
	bool takeAudioMixerRequest();
}
