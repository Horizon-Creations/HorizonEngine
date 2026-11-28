#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <Types/UUID.h>
#include "ContentManager/Assets.h"
#include "MeshSimplifier.h"

// Auto-LOD chains (Thema 331, step 3): turns one StaticMeshAsset into a row of
// simplified LOD meshes kept INSIDE the mesh's own file (HAsset::CHUNK_MLDI/MLOD).
// The Content Browser shows one asset; the Static Mesh tab switches between the
// levels; the runtime swaps them by distance.
//
// Every level keeps a UUID of its own (the ContentManager registers it under that
// id when the mesh loads), so what consumes a chain (a FoliageLayer's lods, a
// LODComponent, a cooked row) stores level ids exactly as it did when the levels
// were sidecar files `<stem>_LOD<n>.hasset` beside the mesh. Running the generator
// again keeps those ids; a mesh that still has such sidecars has them merged in,
// with their ids, and the files removed.
namespace AutoLod
{

struct Settings
{
    // Triangle share of each level after LOD0, nearest first, each in (0, 1).
    // {0.5, 0.25, 0.1} makes the 100 / 50 / 25 / 10 % chain.
    std::vector<float> ratios = { 0.5f, 0.25f, 0.1f };

    // Largest error a level may carry, relative to the section's extent
    // (MeshSimplifier::Settings::targetError). Generous on purpose: the ratio is
    // the budget, and this only stops a mesh that cannot be reduced that far
    // from being wrecked trying.
    float targetError = 0.15f;

    // Foliage: alpha-tested cards and thin, disconnected geometry. Whole pieces
    // may vanish, and (with an alpha mask) a level has to keep its leaf area.
    bool           foliage    = false;
    const uint8_t* alphaMask  = nullptr;   // 1 byte/texel, row-major, 255 = opaque
    int            alphaWidth  = 0;
    int            alphaHeight = 0;

    // A section is never reduced below this many triangles.
    size_t minSectionTriangles = 4;

    // A level that keeps more than this share of the PREVIOUS level's triangles
    // is dropped as a duplicate (the mesh could not be reduced any further).
    float minStepRatio = 0.9f;
};

struct Level
{
    std::string path;                // content-relative, forward slashes ("" for an in-memory build)
    HE::UUID    id;                  // the written asset's UUID (null for an in-memory build)
    uint32_t    triangles  = 0;
    float       worldError = 0.0f;   // metres (object space) this level is off LOD0 by; 0 for LOD0
};

struct Chain
{
    std::vector<Level> levels;       // levels[0] = LOD0 itself, then the generated ones
    std::string        error;        // non-empty: nothing was generated
    bool               writeFailed = false;   // error came from the disk, not from the mesh
};

// `<stem>_LOD<n>.hasset` beside `meshPath`: where a level lived before the levels
// moved into the mesh. Only migration still looks there.
std::string lodPath(const std::string& meshPath, int n);

// The old sidecars that exist for `meshPath`, LOD1 first, stopping at the first gap.
std::vector<std::string> findLodSidecars(const std::filesystem::path& contentRoot,
                                         const std::string& meshPath);

// The level index inside the mesh file (empty when it has none or cannot be read).
std::vector<StaticMeshAsset::Lod> levelsInFile(const std::filesystem::path& contentRoot,
                                               const std::string& meshPath);

// How many levels the mesh has: the ones inside it, else its old sidecars.
size_t levelCount(const std::filesystem::path& contentRoot, const std::string& meshPath);

// Drops the levels past `keep` — inside the mesh (the file is rewritten) and any
// old sidecars `<stem>_LOD<n>` with n > `keep` — and returns how many went. A
// regeneration that writes fewer levels than an earlier one otherwise leaves the
// old tail describing the old geometry. `keep` 0 clears the chain.
size_t removeStaleLevels(const std::filesystem::path& contentRoot,
                         const std::string& meshPath, int keep);

// True for a path that IS an old LOD sidecar (`…_LOD<n>.hasset`); the editor keeps
// one from being run through the generator itself.
bool isLodSidecarPath(const std::string& path);

// One simplified copy of `src` at `ratio`, section by section so the material
// slots (MSEC) survive: simplifying the whole index buffer as one would let
// triangles of different sections merge and break the contiguous-run contract.
// `worldError` receives the largest error over the sections, in object-space
// metres. Returns false for a mesh the tool cannot read (cooked GPU form, no
// triangles).
bool buildLevel(const StaticMeshAsset& src, float ratio, const Settings& settings,
                StaticMeshAsset& out, float& worldError);

// In-memory chain: LOD0's stats plus one entry per generated level. `meshes`
// receives the generated assets (paths/ids empty). Levels that did not shrink
// are dropped; `settings.ratios` is the request, not a promise.
Chain build(const StaticMeshAsset& lod0, const Settings& settings,
            std::vector<StaticMeshAsset>* meshes = nullptr);

// build() + rewrite `lod0`'s file (its `path` must be set) with the generated
// levels inside it. A level keeps the UUID of the level it replaces (from the
// file's level index, else from an old sidecar, which is then deleted); a new
// one gets a new UUID. `levels[i].path` is the mesh's own path.
Chain generate(const StaticMeshAsset& lod0, const std::filesystem::path& contentRoot,
               const Settings& settings);

// Ratios for "N levels, each keeping `step` of the one before, the last one
// `floor`" — the shape the editor's dialog exposes. levels 1..8.
std::vector<float> ratiosFor(int levels, float step, float floor);

} // namespace AutoLod
