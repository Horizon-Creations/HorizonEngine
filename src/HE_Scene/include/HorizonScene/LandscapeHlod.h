#pragma once
// The baked far-landscape material of one terrain chunk (Thema 228, docs/landscape-hlod-baked-material-2026-11-02.md):
// the three pictures HlodBaker makes, as engine assets (an sRGB albedo texture, a normal texture and a surface
// texture, each with its mip chain, and ONE small lit material that reads them by world position), and the ids and
// paths they are packed under.
//
// The ids are a pure function of the terrain entity's id and the chunk coordinate, so the export (which writes the
// assets into the pak) and the runtime (which looks them up when it builds the chunk's LOD levels) agree without a
// table, and an unchanged landscape packs the same bytes under the same ids on the next export.
#include "HorizonScene/LandscapeHlodBaker.h"
#include <Types/UUID.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ContentManager;
class HorizonWorld;
class IRenderer;

namespace HE::landscape
{
    enum class HlodAsset : uint32_t { Material = 0, Albedo = 1, Normal = 2, Surface = 3 };

    HE::UUID    hlodAssetId(const HE::UUID& terrainEntityId, uint32_t cx, uint32_t cz, HlodAsset what);
    std::string hlodAssetPath(const HE::UUID& terrainEntityId, uint32_t cx, uint32_t cz, HlodAsset what);
    // Every baked asset lives under this folder: the export keeps it as a reach root (nothing in a scene names it).
    inline constexpr char kHlodAssetFolder[] = "Baked/Landscape/";

    // One packed asset: its bytes are a complete .hasset.
    struct HlodAssetBlob
    {
        HE::UUID             id;
        std::string          relPath;
        uint16_t             type = 0;      // HE::AssetType
        std::vector<uint8_t> bytes;
    };

    // The textures and the material of one chunk as .hasset blobs (material last). `rect` is the chunk's world XZ
    // rectangle the pictures cover. False with the reason in `why`.
    bool buildHlodAssets(const HlodChunkImages& images, const HlodChunkRect& rect, const HE::UUID& terrainEntityId,
                         uint32_t cx, uint32_t cz, std::vector<HlodAssetBlob>& out, std::string* why = nullptr);

    // What the export gives the bake.
    struct HlodExportOptions
    {
        uint64_t budgetBytes  = 512ull << 20;   // all baked tiles of the export, cooked (ASTC / BC7) with mips; 0 = no limit
        int      maxTexels    = 256;            // the largest side of a tile; coarser when the screen would not need more anyway
        int      minTexels    = 16;
        int      supersample  = 2;
        float    fovYDeg      = 60.0f;          // the reference camera the planner sizes the pictures for
        int      screenHeightPx = 1080;
    };
    struct HlodExportStats
    {
        uint32_t terrains = 0;          // landscapes baked
        uint32_t chunks = 0;            // chunks baked
        uint32_t skipped = 0;           // landscapes left alone (not an Auto Landscape material, rotated, ...)
        uint32_t downscaled = 0;        // landscapes baked below what the screen would want (cap or budget)
        uint64_t plannedBytes = 0;      // what the planner expects the cooked textures to cost
        double   ms = 0.0;
    };
    // Bakes every landscape in `source` that has an Auto Landscape material (the scene an export loaded, or the open
    // one) and appends the assets of each chunk to `out`. `seen` carries the terrain ids already baked by an earlier
    // scene so one landscape that several scenes share is baked once. `restoreTo` is the world the renderer draws
    // again afterwards. Main thread. `log` gets one line per landscape, `cancelled` is polled between chunks.
    HlodExportStats bakeWorldLandscapes(HorizonWorld& source, HorizonWorld* restoreTo, ContentManager& cm, IRenderer& renderer,
                                        const HlodExportOptions& options, std::vector<HlodAssetBlob>& out,
                                        std::vector<HE::UUID>& seen,
                                        const std::function<void(const std::string&)>& log = {},
                                        const std::function<bool()>& cancelled = {});

    // Puts the blobs into `cm` as resident assets (what a pak mount does at runtime), for a witness or a test that
    // wants to draw the baked far landscape without packing. Returns how many loaded.
    size_t loadHlodAssets(ContentManager& cm, const std::vector<HlodAssetBlob>& blobs);
}
