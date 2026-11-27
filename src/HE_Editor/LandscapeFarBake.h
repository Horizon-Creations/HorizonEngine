#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>

struct AppContext;
namespace HE { class LandscapeRuntime; }

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

	// ── The tile world (LandscapeRuntime, Thema 326 / 355) ───────────────────────
	// Its far image is the export's region bake (HorizonScene/LandscapeFarBake.h)
	// on a worker while the editor goes on, over the edit overlay; only the
	// regions whose tiles changed are painted again. The table goes to the
	// export's own cache (Saved/LandscapeFar/<pack>.far), so the next export
	// paints nothing that the editor already painted, and onto the overlay,
	// which draws it at once.
	//
	// What a region is painted from: the landscape MATERIAL, drawn once from
	// straight above (the ground image below) wherever the backend reads it back
	// and the material writes a virtual texture; else the layer weights times the
	// layer colours (the export's painter). The material needs the region's tiles
	// in the atlas: the main thread draws it when the worker asks. A manual bake
	// moves the streaming anchor from region to region for that (tileAnchor), an
	// automatic one paints only the regions loaded now and leaves the others as
	// they were (stale) until a manual bake.
	void setTileRuntime(HE::LandscapeRuntime* runtime);
	using GroundImage = std::function<bool(const glm::dvec2& minAbs, const glm::dvec2& maxAbs, uint32_t px, std::vector<uint8_t>& rgba)>;
	// The ground image (RvtLandscape::renderTileGroundImage) and the key of the material it draws (0 = none now), from the owner of the
	// RVT driver. Unset: every region is painted from the weights.
	void setGroundImage(GroundImage image, std::function<uint64_t()> materialKey);
	// The painter hash of a bake from the material as things stand (0 = there would be none): the export keeps those regions.
	uint64_t groundPainterHash();
	// Where the tile world streams around this frame: `eye`, or the region a manual bake from the material is waiting for.
	glm::dvec3 tileAnchor(const glm::dvec3& eye);
	enum class TileState { NoWorld, Idle, Baking, Stale, UpToDate };
	TileState tileState();
	// Starts a bake of the tile world (false: none to bake, or one is running). `walk`: a material bake moves the streaming anchor to
	// every region it has to paint (the manual bake); without, a region that is not loaded keeps its old image.
	bool bakeTileWorld(AppContext& ctx, bool walk = true);
	const std::string& tileSummary();
}
