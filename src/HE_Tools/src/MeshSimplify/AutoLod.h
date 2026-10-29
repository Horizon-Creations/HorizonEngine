#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <Types/UUID.h>
#include "ContentManager/Assets.h"
#include "MeshSimplifier.h"

// Auto-LOD chains (Thema 331, step 3): turns one StaticMeshAsset into a row of
// simplified LOD meshes written as ordinary sidecar assets, the way the engine's
// own RockA / RockALod1 / RockALod2 sit next to each other (mesh_gen) — except
// these are user content, so their UUIDs are random-but-stable instead of fixed.
//
// The link between LOD0 and its sidecars is the NAME, `<stem>_LOD<n>.hasset` in
// LOD0's folder; no chunk on LOD0 records them. What consumes a chain (a
// FoliageLayer's lods, a LODComponent) stores the sidecars' UUIDs itself, so a
// scene never depends on the convention — it only serves regeneration: running
// the generator again finds the existing files (writeAsset keeps their UUID, so
// every reference survives) and a Reimport of LOD0 knows whether to refresh them.
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
};

// `<stem>_LOD<n>.hasset` beside `meshPath` (content-relative, forward slashes).
std::string lodPath(const std::string& meshPath, int n);

// The sidecars that exist for `meshPath`, LOD1 first, stopping at the first gap.
std::vector<std::string> findLodSidecars(const std::filesystem::path& contentRoot,
                                         const std::string& meshPath);

// True for a path that IS a LOD sidecar (`…_LOD<n>.hasset`); the editor keeps a
// sidecar from being run through the generator itself.
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

// build() + write every generated level next to `lod0` (its `path` must be set)
// as `<stem>_LOD<n>.hasset`. Existing sidecars of the same name are overwritten
// in place and keep their UUID.
Chain generate(const StaticMeshAsset& lod0, const std::filesystem::path& contentRoot,
               const Settings& settings);

// Ratios for "N levels, each keeping `step` of the one before, the last one
// `floor`" — the shape the editor's dialog exposes. levels 1..8.
std::vector<float> ratiosFor(int levels, float step, float floor);

} // namespace AutoLod
