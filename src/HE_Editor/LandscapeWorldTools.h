#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity
#include <functional>
#include <string>

struct AppContext;

// ── The Details panel's "Tile Landscape" section (Thema 326, step 6) ───────────────────────────────────────
// What the editor offers for a world-scale landscape (LandscapeWorldComponent): the component's own settings, and the three ways to make
// the pack file it streams from (HE::LandscapePackBuild): a procedural world, the scene's Terrain migrated into tiles, and an import of
// 16-bit height maps (one picture, or a World Machine / Gaea tile set). A build runs on its own thread for as long as it takes (a 100 km
// world is minutes), shows a progress bar and can be cancelled; when it ends the component points at the new pack and the runtime starts
// again from it.
//
// Not here: painting by hand on tiles, and the water of a migrated terrain (see LandscapePackBuild.h).
namespace LandscapeWorldTools
{
	// Once a frame, from the editor's main loop (next to the landscape runtime's update): takes in a build that finished, points the
	// component at the pack and makes the runtime open it again. Needs no ImGui; safe to call with no build running.
	void pump(HorizonWorld& world);

	// The section's body for an entity that has a LandscapeWorldComponent. Drawn by InspectorPanel.
	// `afterEdit` is called right after each widget that edits the component (the Details panel's undo tracking hangs on the last item).
	void drawSection(AppContext& ctx, Entity entity, const std::function<void()>& afterEdit = {});

	// ── What the section's buttons do, without the widgets (the UI test and scripts drive these) ──
	struct GenerateSettings
	{
		int   tilesX = 8, tilesZ = 8;   // 512 m tiles: 196 x 196 is 100 km
		int   seed = 1337;
		int   octaves = 12;
		float heightRangeM = 3000.0f;
	};
	struct ImportSettings
	{
		float metersPerPixel = 2.0f;
		float heightMinM = 0.0f;
		float heightRangeM = 3000.0f;
		bool  flipFileRows = false;
		bool  paintFromShape = true;
	};
	// Each starts a build into `outPath` for `target` (an entity with a LandscapeWorldComponent) and returns "" or why it could not start
	// (another build is running, nothing to migrate, an empty path ...). The result arrives through pump().
	std::string startGenerate(HorizonWorld& world, Entity target, const std::string& outPath, const GenerateSettings& s);
	std::string startMigrate(HorizonWorld& world, Entity target, const std::string& outPath);
	std::string startImport(HorizonWorld& world, Entity target, const std::string& outPath, const std::string& fileOrFolder, const ImportSettings& s);
	bool        buildRunning();
	void        cancelBuild();
	// The last build's one-line outcome ("" before any), and whether it failed.
	const std::string& lastStatus();
	bool        lastStatusIsError();
	// The Terrain a migration would take: `near` if it has one, else the first in the scene; entt::null for none.
	Entity      migrationSource(HorizonWorld& world, Entity nearEntity);
	// Forgets the section's remembered state (tests; a project switch).
	void        resetState();
}
