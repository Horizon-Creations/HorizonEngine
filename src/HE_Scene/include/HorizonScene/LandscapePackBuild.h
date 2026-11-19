#pragma once
#include "HorizonScene/LandscapeSource.h"
#include "HorizonScene/LandscapeTile.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct TerrainComponent;

namespace HE
{

// ── Making a tile landscape: the editor's three producers (Thema 326, step 6) ────────────────────────────
// A pack (LandscapeSource.h) is written once and read many times, so "authoring" a world-scale landscape is making the pack:
//   * a procedural world (the fBm of ProceduralLandscapeSource, any size: a 100 km test world or a generated starting point),
//   * a migrated TerrainComponent (the single landscape of today, resampled onto the tile grid),
//   * an import of height maps: a World Machine / Gaea tile set (one file per tile, `name_x<X>_y<Y>.png|r16|raw|pgm`) or one picture.
// All three go through LandscapePackWriter, tile by tile, so a 4 GB world needs no 4 GB of memory, and several tiles are made at the same
// time on plain threads (the job pool belongs to the frame). No ImGui, no scene, no renderer in here: the editor's Landscape World section
// (LandscapeWorldTools.h) drives these from a LandscapeBuildJob.
//
// Not covered here, on purpose: painting by hand on tiles (a pack appends its tiles back to back behind a fixed index, so a tile that
// changes size cannot be rewritten in place; that needs a writable store and is the next editor step) and the water of a migrated
// TerrainComponent (its water::Field is not cut into tiles; the migrated pack has no water table).

// Called after each batch with (tiles written, tiles in all); return false to cancel (the pack file is removed).
using LandscapeBuildProgress = std::function<bool(size_t done, size_t total)>;

struct LandscapeBuildResult
{
	bool        ok = false;
	bool        cancelled = false;
	std::string error;                 // in user words when !ok (and not cancelled)
	int         tilesX = 0, tilesZ = 0;
	size_t      tilesWritten = 0;
	uint64_t    packBytes = 0;
	// What the pack's stored heights mean: metres = heightMinM + v / 65535 * heightScaleM. These are the numbers a LandscapeWorldComponent has to carry
	// (the pack's header has them as well and the runtime reads them from there; the component's are only a fallback).
	float       heightMinM = 0.0f, heightScaleM = 0.0f;
	double      seconds = 0.0;
	std::string summary() const;       // one line for a status text
};

struct LandscapeBuildOptions
{
	LandscapeCodec codec = LandscapeCodec::best();
	unsigned       threads = 0;        // 0 = all hardware threads but one
	LandscapeBuildProgress progress;
};

// ── Procedural ───────────────────────────────────────────────────────────────
// `params.geometry` / tiles / seed as for the test world. Heights span 0 .. params.heightScale metres.
LandscapeBuildResult buildProceduralPack(const std::string& path, const ProceduralLandscapeParams& params, const LandscapeBuildOptions& options = {});

// ── Migration of a TerrainComponent ──────────────────────────────────────────
// The terrain's south-west corner (local -sizeX/2, -sizeZ/2) lands at (originX, originZ) of the pack's world; the tile grid covers the
// terrain with whole tiles and the last row/column of tiles repeats the terrain's edge height outside it. Heights are the terrain's own field
// (sculptHeights, else the noise: computeTerrainHeightField) sampled bilinearly at the tiles' raster; with a terrain whose cell is the tile's
// raster (1025 samples over 2048 m against a 2 m tile) that is an exact copy. The three weight pages become the tile's 12 channels (a terrain
// with no paint is layer 0 everywhere). `heightScaleM <= 0` fits the range of the terrain: min = floor(lowest), scale = the span above it.
struct LandscapeMigrateParams
{
	LandscapeGeometry geometry;
	float originX = 0.0f, originZ = 0.0f;
	float heightMinM = 0.0f, heightScaleM = 0.0f;
	float yOffsetM = 0.0f;   // the terrain entity's height in the world: the field is local to it
};
LandscapeBuildResult migrateTerrainToPack(const TerrainComponent& terrain, const std::string& path, const LandscapeMigrateParams& params,
                                          const LandscapeBuildOptions& options = {});

// ── Height map import ────────────────────────────────────────────────────────
// One grid of equally sized files (a single picture is a grid of one). Pixel (px, pz) of the whole grid is the sample at world
// (px * metersPerPixel, pz * metersPerPixel): the samples are points, so N pixels span (N - 1) * metersPerPixel, as a tool exports them
// with `overlap` off. Image row 0 is the -Z edge, column 0 the -X edge, like TerrainHeightmap; `flipY` reverses the ORDER OF THE FILE ROWS
// only (y0 = the southern-most file, World Machine's numbering), the pictures themselves stay top-down. A stored height is the picture's 16-bit value itself
// (8-bit pictures are widened by 257), so `heightMinM` / `heightScaleM` say what 0 and 65535 mean in metres and are only written down.
struct LandscapeHeightmapFiles
{
	std::vector<std::string> files;     // row-major, filesX * filesZ entries; empty string = a hole (reads as the lowest height)
	int                      filesX = 0, filesZ = 0;
	std::string              error;     // set when nothing usable was found
};
// Every `*_x<N>_y<M>.<ext>` of a folder (png, r16, raw, pgm), sorted into the grid; a hole in the grid is a hole.
LandscapeHeightmapFiles findHeightmapTileFiles(const std::string& dir);
// A single picture as a grid of one.
LandscapeHeightmapFiles singleHeightmapFile(const std::string& path);

struct LandscapeHeightmapImportParams
{
	LandscapeGeometry geometry;
	float  metersPerPixel = 2.0f;
	float  heightMinM = 0.0f, heightScaleM = 3000.0f;
	bool   flipY = false;
	// Paint the 12 layers from the shape (flat ground / steep rock / high ground, see the cpp) instead of layer 0 everywhere.
	bool   autoWeights = true;
	// A world larger or smaller than the pictures: tiles beyond them read the edge. 0 = just cover them.
	int    tilesX = 0, tilesZ = 0;
};
LandscapeBuildResult importHeightmapToPack(const LandscapeHeightmapFiles& files, const std::string& path, const LandscapeHeightmapImportParams& params,
                                           const LandscapeBuildOptions& options = {});

// ── Running one on a thread ──────────────────────────────────────────────────
// The editor must not freeze for the minutes a 100 km world takes. start() runs `work` on its own thread with progress counters the UI
// reads every frame; cancel() makes the next progress call return false. poll() hands out the result once, on the main thread.
class LandscapeBuildJob
{
public:
	using Work = std::function<LandscapeBuildResult(const LandscapeBuildOptions&)>;
	LandscapeBuildJob() = default;
	~LandscapeBuildJob();
	LandscapeBuildJob(const LandscapeBuildJob&) = delete;
	LandscapeBuildJob& operator=(const LandscapeBuildJob&) = delete;

	bool start(std::string label, Work work);   // false while one is running
	void cancel() { m_cancel = true; }
	bool running() const { return m_running; }
	size_t done() const { return m_done; }
	size_t total() const { return m_total; }
	const std::string& label() const { return m_label; }
	// True once when the job finished since the last poll; `out` is its result.
	bool poll(LandscapeBuildResult& out);

private:
	std::thread               m_thread;
	std::atomic<bool>         m_running{ false };
	std::atomic<bool>         m_cancel{ false };
	std::atomic<bool>         m_finished{ false };
	std::atomic<size_t>       m_done{ 0 }, m_total{ 0 };
	LandscapeBuildResult      m_result;
	std::string               m_label;
};

} // namespace HE
