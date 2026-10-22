#pragma once
#include <HorizonScene/Components/FoliageComponent.h>
#include <ContentManager/DefaultAssets.h>
#include <cstdint>
#include <string>

// ── What a landscape layer scatters ─────────────────────────────────────────
// The engine's layer meshes (DefaultAssets.h, written by mesh_gen) as ready-made
// FoliageLayer settings: the mesh and its detail levels, a density and scale range
// that suit its size, the shadow / AO flags the foliage budget wants for it, and
// the placement rules that put it where the Auto Landscape material shows the
// matching surface (its defaults: rock past a steepness of 0.12, snow above 60 m).
// A preset is a starting point: the author changes any field afterwards, and a
// layer keeps exactly what is saved in it (nothing here is read again later).
//
// One mesh kind is one FoliageLayer; a second boulder shape is a second layer
// (FoliageComponent::addLayer hands each a seed of its own), so two shapes never
// grow in the same spots.
namespace HE
{
    enum class LayerMeshKind : int { Tuft, Pebble, RockA, RockB, RockC, SnowDrift, Count };
    constexpr int kLayerMeshKindCount = static_cast<int>(LayerMeshKind::Count);

    inline const char* layerMeshKindName(LayerMeshKind k)
    {
        static const char* const names[] = { "Grass Tuft", "Pebble", "Rock A", "Rock B", "Rock C", "Snow Drift" };
        return names[static_cast<int>(k)];
    }

    // The file name stem the kind's meshes carry ("Engine/Meshes/<stem>.hasset"; level
    // n >= 1 adds "Lod<n>").
    inline const char* layerMeshStem(LayerMeshKind k)
    {
        static const char* const stems[] = { "Tuft", "Pebble", "RockA", "RockB", "RockC", "SnowDrift" };
        return stems[static_cast<int>(k)];
    }

    inline UUID layerMeshLevel0(LayerMeshKind k)
    {
        switch (k)
        {
            case LayerMeshKind::Tuft:      return kEngineTuftMeshId;
            case LayerMeshKind::Pebble:    return kEnginePebbleMeshId;
            case LayerMeshKind::RockA:     return kEngineRockAMeshId;
            case LayerMeshKind::RockB:     return kEngineRockBMeshId;
            case LayerMeshKind::RockC:     return kEngineRockCMeshId;
            case LayerMeshKind::SnowDrift: return kEngineSnowDriftMeshId;
            default:                       return UUID{};
        }
    }

    // Detail levels the kind has, level 0 included.
    inline int layerMeshLevelCount(LayerMeshKind k)
    {
        switch (k)
        {
            case LayerMeshKind::Tuft: case LayerMeshKind::RockA:
            case LayerMeshKind::RockB: case LayerMeshKind::RockC: return 3;
            default:                                              return 2;
        }
    }

    inline UUID layerMeshLevelId(LayerMeshKind k, int level)
    {
        UUID id = layerMeshLevel0(k);
        id.hi += static_cast<uint64_t>(level);
        return id;
    }

    // Content path of one level, for loading the file by path.
    inline std::string layerMeshLevelPath(LayerMeshKind k, int level)
    {
        return std::string("Engine/Meshes/") + layerMeshStem(k) + (level > 0 ? "Lod" + std::to_string(level) : "") + ".hasset";
    }

    // Fills `l` with the kind's settings. The layer's seed stays (it keeps the layer's
    // own random stream), its mask too; the scatter is marked dirty.
    inline void applyLayerMeshPreset(FoliageLayer& l, LayerMeshKind k)
    {
        l.name            = layerMeshKindName(k);
        l.meshAssetId     = layerMeshLevel0(k);
        l.materialAssetId = UUID{};       // the mesh's own material; a surface material is a later step
        l.lods.clear();
        l.slopeMin = 0.0f; l.slopeMax = 1.0f;
        l.heightMin = -FoliageLayer::kNoHeightLimit; l.heightMax = FoliageLayer::kNoHeightLimit;
        l.weightLayer = -1;
        l.castsShadow = false; l.contributesAO = false; l.shadowDistance = 0.0f;
        l.bucketSize = 32.0f;

        // {distance, density} of levels 1.. (a level thins the layer from its distance out).
        struct Level { float distance, density; };
        Level levels[2] = {};
        switch (k)
        {
            case LayerMeshKind::Tuft:
                l.density = 0.30f; l.minScale = 0.7f; l.maxScale = 1.5f; l.drawDistance = 70.0f;
                l.slopeMax = 0.12f;                                   // below where the material turns to rock
                levels[0] = { 15.0f, 0.7f }; levels[1] = { 35.0f, 0.4f };
                break;
            case LayerMeshKind::Pebble:
                l.density = 0.20f; l.minScale = 0.5f; l.maxScale = 1.6f; l.drawDistance = 45.0f;
                l.slopeMax = 0.25f;                                   // dirt and the foot of a slope
                levels[0] = { 15.0f, 0.6f };
                break;
            case LayerMeshKind::RockA: case LayerMeshKind::RockB: case LayerMeshKind::RockC:
                l.density = 0.004f; l.minScale = 0.6f; l.maxScale = 2.4f; l.drawDistance = 160.0f;
                l.slopeMin = 0.15f;                                   // where the material shows rock
                l.castsShadow = true; l.shadowDistance = 60.0f; l.contributesAO = true;
                levels[0] = { 30.0f, 1.0f }; levels[1] = { 75.0f, 0.7f };
                break;
            case LayerMeshKind::SnowDrift:
                l.density = 0.01f; l.minScale = 0.8f; l.maxScale = 2.0f; l.drawDistance = 110.0f;
                l.heightMin = 66.0f;                                  // snow height 60 m + its blend
                l.slopeMax = 0.35f;                                   // snow does not hold on the steep
                levels[0] = { 40.0f, 0.8f };
                break;
            default: break;
        }
        for (int n = 1; n < layerMeshLevelCount(k) && n <= 2; ++n)
        {
            FoliageLod d;
            d.meshAssetId = layerMeshLevelId(k, n);
            d.distance    = levels[n - 1].distance;
            d.density     = levels[n - 1].density;
            l.lods.push_back(d);
        }
        l.dirty = true;
    }
}
