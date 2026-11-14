#pragma once
#include "HorizonScene/LandscapeCellHlod.h"
#include "HorizonScene/LandscapeCellHlodBake.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class HorizonWorld;
class ContentManager;

namespace HE
{

// ── A tile landscape in an export (Thema 326) ────────────────────────────────────────────────────────────────────────────────────────────
// The pack (LandscapeSource.h) is a file the scene names by path, not an asset in the pak: it can be gigabytes and is read by seeking.
// An export therefore ships it as a loose file in the game's data folder ("Landscape/<name>.pack") and the exported scenes name it by that
// relative path; the game resolves a relative path against its data folder (GameApplication, resolveLandscapePack).
//
// The HLOD proxies of the cells (LandscapeCellHlodBake.h) are baked here, on the calling thread (they load mesh assets through the editor's
// ContentManager), and kept as a table in the plan. They are written into the COPY of the pack the exporter makes, never into the project's
// own pack: an export must not change what the author owns.
struct LandscapeExportPack
{
	std::filesystem::path source;                   // the project's pack, found
	std::string dest;                               // relative to the game's data folder, forward slashes
	std::shared_ptr<LandscapeHlodTable> hlod;       // null: nothing baked (no cells, switched off, or nothing static in them)
	bool hlodDone = false;                          // a bake ran for this pack (a second scene with cells does not run another)
};

struct LandscapeExportPlan
{
	std::vector<LandscapeExportPack> packs;
	CellHlodBakeStats bake;                         // summed over the bakes that ran
};

struct LandscapeExportOptions
{
	bool bakeHlod = true;
	CellHlodBakeOptions bakeOptions;
};

using LandscapeExportLog = std::function<void(int level, const std::string& text)>;   // 0 info, 1 warning (Build::log)

// For every enabled LandscapeWorldComponent of `world` that names a pack: finds the file (absolute, else under `projectRoot`, else as given),
// adds it to `plan` (once per source, whatever the number of scenes) and rewrites the component's path to the destination so the scene that is
// serialized next names the shipped copy. With `options.bakeHlod` and a cell manifest in the world, bakes the cell proxies through the cell
// files under `projectRoot`. Components whose pack cannot be found are left as they were and a warning says so; nothing here fails the export.
// Returns the number of components rewritten.
size_t prepareLandscapePacksForExport(HorizonWorld& world, const std::filesystem::path& projectRoot, ContentManager& content,
                                      const LandscapeExportOptions& options, LandscapeExportPlan& plan, const LandscapeExportLog& log = {});

// The game's half: a relative pack path that names a file in `dataDir` becomes the absolute path of that file, so the runtime opens it whatever
// the working directory is. Paths that are absolute, or that name nothing in `dataDir`, are left alone (the editor's own scenes, a developer
// build run from the project). Returns the number of components changed; cheap enough for every frame (one stat per unresolved relative path).
size_t resolveLandscapePacks(HorizonWorld& world, const std::filesystem::path& dataDir);

// The step the exporter runs on the shipped copy: appends the baked table (a null or empty one writes nothing). Defined here because the pack
// format lives here and the exporter (HE_Core) cannot see it.
bool finishExportedPack(const LandscapeExportPack& pack, const std::filesystem::path& copied, std::string* error);

} // namespace HE
