#pragma once
#include "HorizonScene/CellStreamer.h"
#include "HorizonScene/LandscapeCellHlod.h"

#include <cstddef>
#include <cstdint>
#include <string>

class ContentManager;

namespace HE
{

// ── Baking the cell HLOD proxies from the cell files (Thema 326, step 6) ─────────────────────────────────────────────────────────────────
// For every cell of a manifest: load the cell file into a scratch world, take its STATIC meshes (a MeshComponent that is visible, with no
// skeleton, no animator and no moving rigid body), put their triangles in absolute world metres (HE::worldMatrixOf, the cell's entities already
// carry absolute positions) and merge them with bakeCellHlod into the one mesh LandscapeHlodProxies draws.
//
// A cell is only so big: `maxTrianglesPerCell` bounds what one proxy costs by taking the instances in order of their bounding size (the
// big ones, buildings and rocks, first; the small props are what a distant eye misses) until the budget is spent. Instances are not
// decimated, and they carry no material: the proxy draws with the engine's default material, a silhouette in the right place at the right size.
//
// `reader` is CellStreamer's (project-relative cell path -> a function that fills the bytes, or an empty function for a missing file). Runs on the
// calling thread and loads mesh assets through `content` as it goes (ensureResident); the caller releases nothing, the export owns the manager.
struct CellHlodBakeOptions
{
	size_t maxTrianglesPerCell = 20000;
	size_t maxInstancesPerCell = 4000;   // a cell of ten thousand twigs is not a building: past this many the smallest are dropped first
};

struct CellHlodBakeStats
{
	size_t cells = 0;            // cells in the manifest
	size_t baked = 0;            // cells that came out with a proxy
	size_t empty = 0;            // cells with no static mesh at all
	size_t failed = 0;           // cell files missing or unreadable
	size_t instances = 0;        // static mesh instances merged
	size_t instancesDropped = 0; // left out for the budgets
	size_t meshesMissing = 0;    // mesh assets that did not load
	size_t triangles = 0;
	size_t bytes = 0;
};

LandscapeHlodTable bakeCellProxies(const CellManifest& manifest, const CellStreamer::Reader& reader, ContentManager& content,
                                   const CellHlodBakeOptions& options = {}, CellHlodBakeStats* stats = nullptr);

// Appends `table` to an existing pack as its HLOD section (LandscapeSource.h, "Pack file"), or replaces the one it has. The tiles, the coarse
// world and the water table are not touched. False with the reason in `error` for a file that is not a pack.
bool landscapePackSetHlod(const std::string& packPath, const LandscapeHlodTable& table, std::string* error = nullptr);

} // namespace HE
