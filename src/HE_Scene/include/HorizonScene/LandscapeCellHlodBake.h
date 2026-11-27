#pragma once
#include "HorizonScene/CellStreamer.h"
#include "HorizonScene/LandscapeCellHlod.h"

#include <JobSystem/JobSystem.h>
#include <glm/mat4x4.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
	size_t reused = 0;           // CellHlodBakeJob: cells whose file had not changed since the cache, left as they were
};

LandscapeHlodTable bakeCellProxies(const CellManifest& manifest, const CellStreamer::Reader& reader, ContentManager& content,
                                   const CellHlodBakeOptions& options = {}, CellHlodBakeStats* stats = nullptr);

// ── The same bake in steps, so the editor can keep the frame free ────────────────────────────────────────────────────────────────────────
// 1. cellStatics   the static meshes a parsed cell file names (parseSceneBytes runs on a worker; building the entities into the scratch world
//                  is the serializer's main-thread part)
// 2. resolveCellStatics  what needs the ContentManager (mesh triangles, the material's colour, the palette): main thread, never handed on as an
//                  asset pointer, only as copies
// 3. mergeCellInstances  transform, rank, budget, merge: no asset, no world, any thread
// bakeCellProxies is these three in a row.
struct CellStatic
{
	HE::UUID  mesh, material;     // material: the entity's override, null = the mesh's own first section
	glm::mat4 world{ 1.0f };
};
// False for a scene the serializer refused.
bool cellStatics(const nlohmann::json& parsedCell, std::vector<CellStatic>& out);

struct CellMeshLocal                       // one mesh asset's triangles, shared by every instance of it
{
	std::vector<glm::vec3> positions, normals;
	std::vector<uint32_t>  indices;
};
struct CellInstance
{
	std::shared_ptr<const CellMeshLocal> mesh;
	glm::mat4                            world{ 1.0f };
	uint32_t                             paletteEntry = 0;
};
// Appends colours to `palette` (RGB8 triplets, LandscapeHlodTable::palette); counts meshesMissing in `stats`.
std::vector<CellInstance> resolveCellStatics(const std::vector<CellStatic>& statics, ContentManager& content, std::vector<uint8_t>& palette,
                                             CellHlodBakeStats& stats);
// Empty mesh: nothing static in the cell (or nothing the budgets left). Adds instances / instancesDropped / triangles / bytes to `stats`.
LandscapeHlodMesh mergeCellInstances(std::vector<CellInstance>& instances, const CellHlodBakeOptions& options, CellHlodBakeStats& stats);

// ── Only what changed (the editor's bake) ────────────────────────────────────────────────────────────────────────────────────────────────
// False for a file that is not there. Runs on a worker.
using CellHlodStamper = std::function<bool(const std::string& path, CellHlodStamp& out)>;

// The loose cell files of a project folder (the manifest's paths are relative to it): the reader CellStreamer takes, and the stamp (size and
// modification time) of a file.
CellStreamer::Reader cellFileReader(const std::filesystem::path& projectRoot);
CellHlodStamper      cellFileStamper(const std::filesystem::path& projectRoot);

// The cache a bake leaves and the next one starts from (LandscapeCellHlod.h). `optionsKey`: other budgets, every cell bakes again.
uint64_t cellHlodOptionsKey(const CellHlodBakeOptions& options);

// Bakes the cells of a manifest whose stamp differs from the cache's, into the cache, without ever holding the main thread: the cell file is
// read and parsed on the job pool, the content work (step 2) runs in pump() under a time budget, the merge goes back to the pool. Cells that
// are gone from the manifest are dropped. A cell is entered into the cache when its own bake is through, so a cancelled bake keeps what it did
// and the next one does the rest.
//
// All of it is the main thread's but the jobs: start(), pump() and cancel() are called from one thread, which also owns `cache` and does not
// touch it from start() until finished() (or cancel()). The jobs only touch their own slot.
class CellHlodBakeJob
{
public:
	CellHlodBakeJob() = default;
	~CellHlodBakeJob() { cancel(); }
	CellHlodBakeJob(const CellHlodBakeJob&)            = delete;
	CellHlodBakeJob& operator=(const CellHlodBakeJob&) = delete;

	void start(const CellManifest& manifest, CellStreamer::Reader reader, CellHlodStamper stamper, const CellHlodBakeOptions& options,
	           CellHlodCache& cache);
	// Once a frame while active(). `budgetMs` bounds the content work (at least one cell when one is ready).
	void pump(ContentManager& content, double budgetMs);
	void cancel();

	bool active() const { return m_phase != Phase::Idle && m_phase != Phase::Done; }
	bool finished() const { return m_phase == Phase::Done; }
	const CellHlodBakeStats& stats() const { return m_stats; }
	size_t cellsLeft() const { return m_slots.size() - m_applied; }

private:
	enum class Phase { Idle, Scan, Cells, Done };
	struct Slot;
	struct Scan;

	Phase m_phase = Phase::Idle;
	CellManifest m_manifest;
	CellStreamer::Reader m_reader;
	CellHlodBakeOptions m_options;
	CellHlodCache* m_cache = nullptr;
	CancelToken m_cancel;
	std::shared_ptr<Scan> m_scan;
	JobHandle m_scanJob;   // cancel() waits for it: it reads the cache's stamps
	std::vector<std::shared_ptr<Slot>> m_slots;
	size_t m_nextParse = 0, m_nextResolve = 0, m_applied = 0;
	CellHlodBakeStats m_stats;
};

// Appends `table` to an existing pack as its HLOD section (LandscapeSource.h, "Pack file"), or replaces the one it has. The tiles, the coarse
// world and the water table are not touched. False with the reason in `error` for a file that is not a pack.
bool landscapePackSetHlod(const std::string& packPath, const LandscapeHlodTable& table, std::string* error = nullptr);

} // namespace HE
