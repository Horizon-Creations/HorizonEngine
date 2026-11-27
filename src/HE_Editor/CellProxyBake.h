#pragma once
#include <filesystem>
#include <string>

struct AppContext;
namespace HE { class LandscapeHlodTable; }

// The distant proxies of a split scene's streaming cells, baked in the editor (Thema 355 step 6). Until now only the export baked them
// (HE::bakeCellProxies); Play in the editor never showed what the game shows beyond a cell's load radius.
//
// For a scene with a cell list, every cell's static meshes become one merged proxy (LandscapeCellHlodBake.h, CellHlodBakeJob): the file is
// parsed and the proxy merged on the job pool, the mesh and material reads run on the main thread under a small time budget a frame, so the
// editor goes on. Only the cells whose file changed since the last bake are baked again. The result is kept in
// <project>/Saved/LandscapeHlod/<cell folder>.hlod (outside git and team sync, like the rest of Saved/), which is read again when the scene is
// opened. Play draws it for every cell the streamer does not hold (HE::LandscapeHlodProxies).
//
// When it runs is the same Preferences setting as the far landscape's, EditorConfig::LandscapeFarBake: Off (the editor bakes nothing and
// draws only what a pack of the scene brings), Manual (the Bake Cell Proxies button in the Streaming tab), Automatic (the cell files are
// checked every few seconds while the editor is idle; a changed cell is baked again by itself).
namespace CellProxyBake
{
	// Once per editor frame while editing (not while playing): reads the cache of a scene that is opened, runs the Automatic check, takes
	// a running bake a step further, writes the cache when one is through. `userBusy`: a mouse button is held.
	void tick(AppContext& ctx, bool userBusy);

	// The Manual button: bakes the cells that changed. False: Off, no cells, or a bake (or the cache's reading) is still going.
	bool bakeNow(AppContext& ctx);

	// What Play draws: the baked proxies. Null while there are none, or while the setting (EditorConfig::LandscapeFarBake) is Off (0).
	const HE::LandscapeHlodTable* table(int farBakeMode);

	// A headless witness that writes its cells elsewhere than the project (as PlayCellStreaming::begin's `root`): the cells are read from
	// `root` and the cache kept below it. Empty (the default): the open project's.
	void setWitnessRoot(const std::filesystem::path& root);

	bool baking();                      // a bake is running (cells left in `cellsLeft`)
	size_t cellsLeft();
	const std::string& summary();       // one line about the last bake, for the Streaming tab

	// The project or scene changed: what was read is forgotten, a running bake is cancelled.
	void reset();
}
