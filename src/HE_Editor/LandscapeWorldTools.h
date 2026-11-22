#pragma once
#include <HorizonScene/HorizonWorld.h>   // Entity
#include <functional>
#include <string>

struct AppContext;
struct ImVec2;
class  RenderWorld;
namespace HE { class LandscapeRuntime; }

// ── The Details panel's "Tile Landscape" section (Thema 326, step 6) ───────────────────────────────────────
// What the editor offers for a world-scale landscape (LandscapeWorldComponent): the component's own settings, and the three ways to make
// the pack file it streams from (HE::LandscapePackBuild): a procedural world, the scene's Terrain migrated into tiles, and an import of
// 16-bit height maps (one picture, or a World Machine / Gaea tile set). A build runs on its own thread for as long as it takes (a 100 km
// world is minutes), shows a progress bar and can be cancelled; when it ends the component points at the new pack and the runtime starts
// again from it.
//
// Sculpting and painting by hand: "Edit Tiles" arms a brush in the Scene viewport that works on the tiles under it and writes them back
// (HE::LandscapeEditSession); the edits are a file beside the pack and are baked into the shipped pack by the export.
// Not here: the water of a migrated terrain (see LandscapePackBuild.h), and the Landscape mode's other tools (foliage, water, mountain).
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

	// ── Sculpt and paint on the tiles (HE::LandscapeEditSession) ──
	// The runtime the brush edits: the editor's own, handed in once (the world only holds it const). Null takes it away.
	void        setRuntime(HE::LandscapeRuntime* runtime);
	// Drawn inside the Scene viewport next to TerrainTools::sculptInViewport: the brush cursor on the tile landscape and the stroke under the
	// left button, while "Edit Tiles" is on in the section. A stroke is one undo step of the section's own Undo Stroke / Redo Stroke.
	void        brushInViewport(AppContext& ctx, const RenderWorld& sceneSnapshot, const ImVec2& rectMin, const ImVec2& rectMax,
	                            bool navigating, bool viewportHovered, float dt);
	// Writes the edits beside the pack ("<pack>.edits"); "" or why it failed. Called by the section's button and when the scene is saved.
	std::string saveEdits();
	// One stroke of the Water tool at an absolute place, without a viewport: a lake `radius` wide, 4 m above the ground there, with a
	// bed dug under it. What the headless dump paints (HE_DUMP_TILEWATER); false when no editing region holds it.
	bool paintLake(double absX, double absZ, float radius);
	// True while there are edits no file holds yet.
	bool        editsUnsaved();
}
