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
#include <string>
#include <vector>

class ContentManager;

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

    // Puts the blobs into `cm` as resident assets (what a pak mount does at runtime), for a witness or a test that
    // wants to draw the baked far landscape without packing. Returns how many loaded.
    size_t loadHlodAssets(ContentManager& cm, const std::vector<HlodAssetBlob>& blobs);
}
