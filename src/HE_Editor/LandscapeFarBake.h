#pragma once
#include <string>

struct AppContext;

// The baked far landscape in the editor. Until now only the export baked it
// (ExportDialogPanel, HE::landscape::bakeWorldLandscapes): what a landscape's
// distant chunks would draw in the game was never seen while working on it.
//
// A bake covers every Auto Landscape of the open scene. Its tiles are written to
// <project>/Saved/FarLandscape (outside git and team sync, like the rest of
// Saved/), made resident, and the landscape's chunks rebuild to draw them on
// their far levels. Each landscape's tiles carry a key of what they were baked
// from (heights, layer weights, material, size, place): a changed landscape
// puts its stale tiles away (its far chunks draw the live material again) until
// the next bake. When the editor bakes is the Preferences setting
// EditorConfig::LandscapeFarBake: Off, Manual (the Bake button in Landscape ▸
// Setup), Automatic (a few seconds after the last change).
namespace LandscapeFarBake
{
	enum class State { NoLandscape, UpToDate, Stale, NotBaked };

	// Bakes the open scene's landscapes now. `summary` gets one line for the panel.
	bool bakeNow(AppContext& ctx, std::string* summary = nullptr);

	// What the open scene's landscapes are, measured against their last bake.
	State state(AppContext& ctx);
	const std::string& lastSummary();

	// Once per editor frame. Picks up the saved tiles of a scene that is opened,
	// puts stale ones away, and in Automatic mode bakes once nothing was changed
	// for a few seconds and `userBusy` (a mouse button held, play mode) is false.
	void tick(AppContext& ctx, bool userBusy);

	// The project or scene changed: what was loaded and measured is forgotten.
	void reset();
}
