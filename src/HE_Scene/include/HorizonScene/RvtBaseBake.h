#pragma once
// The baked base of an RVT landscape (RVT 2.0 step 6, docs/rvt-2-0-step6-export-baked-base-2026-11-08.md).
//
// A landscape whose material has an RVT Write / Read pair paints the part BEFORE the Write node (layers, groups, bombing) into
// pages at run time. For a landscape the game never edits, that work is the same on every run, so the export does it once:
// the three page layers (colour, normal, AO + roughness), seen from straight above over the whole landscape, are cooked into
// three textures (BC7 / ASTC by the packer, like every texture) and packed under ids that carry the page-state key. At run
// time the driver (RvtLandscape) finds them by that key and paints its pages from three tiny "baked" view materials that
// sample the textures by world position, instead of the material's own page graphs. Everything after the Read node (snow,
// sand, puddles, trails) is still the live shader.
//
// The key is the gate. It is the hash of everything the page subgraphs depend on (matRvtMaterialState: parameter values,
// switches, the live nodes of the page graphs) plus the landscape's size and place. A script that moved a parameter before the
// Write node, or a different material, gives another key, so the assets are not found and the pages are painted from the graph.
// A sculpt or paint stroke at run time is caught by the driver (it drops the baked pages for good, see RvtLandscape).
#include <HorizonScene/LandscapeHlod.h>
#include <MaterialGraph/RvtSplit.h>
#include <Types/UUID.h>
#include <entt/entt.hpp>
#include <glm/vec2.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ContentManager;
class HorizonWorld;
class IRenderer;
struct MaterialAsset;
struct MaterialComponent;

namespace HE::landscape
{
    enum class RvtBaseAsset : uint32_t { Albedo = 0, Normal = 1, Surface = 2 };

    HE::UUID    rvtBaseAssetId(const HE::UUID& terrainEntityId, uint64_t key, RvtBaseAsset what);
    std::string rvtBaseAssetPath(const HE::UUID& terrainEntityId, uint64_t key, RvtBaseAsset what);   // under kHlodAssetFolder: a reach root

    // The parameter values and switches a landscape's material resolves (the asset's slots, the entity's overrides on top), as
    // the page-state hash wants them. The driver and the export both build their environment here, so their keys agree.
    MatRvtEnv rvtMaterialEnv(const MaterialAsset& material, const MaterialComponent* entity);

    // The key of the pages of `terrain` (see the header comment); 0 when the landscape's material has no RVT Write / Read pair
    // (nothing to bake). Texture revisions are NOT part of it (the pak's cooked pixels differ from the editor's).
    uint64_t rvtBaseKey(ContentManager& cm, HorizonWorld& world, entt::entity terrain);

    // The three baked pictures of one landscape, `side` x `side` each, as texture blobs (a single-level RGBA8 .hasset each; the
    // packer builds the mips and block-compresses them).
    bool buildRvtBaseAssets(const HlodChunkImages& images, const HE::UUID& terrainEntityId, uint64_t key,
                            std::vector<HlodAssetBlob>& out, std::string* why = nullptr);

    // What the export gives the bake.
    struct RvtBaseOptions
    {
        uint64_t budgetBytes   = 256ull << 20;   // all baked landscapes of the export, cooked (BC7 / ASTC) with mips; 0 = no limit
        int      maxTexels     = 4096;           // largest side of a landscape's picture
        int      minTexels     = 128;
        float    metersPerTexel = 0.25f;         // what the live pages have at mip 0: the finest the picture needs
        int      tileTexels    = 1024;           // one render call covers this many texels per side
    };
    struct RvtBaseStats
    {
        uint32_t landscapes = 0;     // baked
        uint32_t skipped = 0;        // no RVT nodes, rotated, not square, ...
        uint32_t downscaled = 0;     // below the live page resolution (cap or budget)
        uint64_t plannedBytes = 0;
        double   ms = 0.0;
    };
    // The picture side for a landscape of `sizeM`: the live page resolution, a power of two, capped, halved until it fits.
    uint32_t rvtBaseSide(float sizeM, const RvtBaseOptions& o, uint64_t budgetLeft, bool* downscaled = nullptr);
    inline uint64_t rvtBaseCookedBytes(uint32_t side) { return static_cast<uint64_t>(side) * side * 3ull * 4ull / 3ull; }   // 3 layers x 1 byte x 4/3 for the mips

    // Bakes every landscape of `source` with an RVT-node material and appends its three textures to `out`. Same contract as
    // bakeWorldLandscapes (the scratch world of an HlodBaker, `restoreTo` drawn again afterwards, `seen` shared across scenes).
    RvtBaseStats bakeRvtBaseWorld(HorizonWorld& source, HorizonWorld* restoreTo, ContentManager& cm, IRenderer& renderer,
                                  const RvtBaseOptions& options, std::vector<HlodAssetBlob>& out, std::vector<HE::UUID>& seen,
                                  const std::function<void(const std::string&)>& log = {},
                                  const std::function<bool()>& cancelled = {});

    // The graph of one baked view: world position -> uv over the landscape's square -> the texture at `texturePath` -> an unlit
    // Output (what the page painter draws into the pool).
    MaterialGraph rvtBaseViewGraph(const std::string& texturePath, glm::vec2 rectMin, float sizeM);

    // Run time: when the pak carries the baked pictures of `terrain` for `key`, registers the three view materials that sample
    // them (BaseColor, Normal, Surface, the order HlodBaker draws them in) and returns true. False when they are not there.
    bool rvtBaseViewMaterials(ContentManager& cm, HorizonWorld& world, entt::entity terrain, uint64_t key, HE::UUID (&views)[3]);
}
