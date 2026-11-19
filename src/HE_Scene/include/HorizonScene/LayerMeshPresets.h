#pragma once
#include <HorizonScene/Components/FoliageComponent.h>
#include <ContentManager/DefaultAssets.h>
#include <MaterialGraph/AutoLandscapeMaterial.h>
#include <cctype>
#include <cmath>
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
// The kinds are a set per texture group (Thema 233): Mountains has the six of Thema 180,
// Forest, Grassland and Desert their own (layerMeshKindsOfGroup). The rules that put a rock or a snow
// drift where the material shows rock or snow read the group's row (AutoLandscapeGroup::rockSlope,
// snowHeight), so a group that turns to rock earlier also grows its rocks earlier. A kind is not
// bound to one group: Grassland scatters Pebble and Rock A with its own numbers.
//
// One mesh kind is one FoliageLayer; a second boulder shape is a second layer
// (FoliageComponent::addLayer hands each a seed of its own), so two shapes never
// grow in the same spots.
namespace HE
{
    // Append only (a kind's number is not saved, but tests and the witness list them in order).
    enum class LayerMeshKind : int { Tuft, Pebble, RockA, RockB, RockC, SnowDrift,
                                     Fern, ForestStump, FallenLog, MossRock,
                                     TallGrass, FlowerTuft, ScreeChip,
                                     DuneGrass, Cactus, Sandstone, Count };
    constexpr int kLayerMeshKindCount = static_cast<int>(LayerMeshKind::Count);

    inline const char* layerMeshKindName(LayerMeshKind k)
    {
        static const char* const names[] = { "Grass Tuft", "Pebble", "Rock A", "Rock B", "Rock C", "Snow Drift",
                                              "Fern", "Forest Stump", "Fallen Log", "Mossy Rock",
                                              "Tall Grass", "Flower Tuft", "Scree Chip",
                                              "Dune Grass", "Cactus", "Sandstone" };
        return names[static_cast<int>(k)];
    }

    // The file name stem the kind's meshes carry ("Engine/Meshes/<stem>.hasset"; level
    // n >= 1 adds "Lod<n>").
    inline const char* layerMeshStem(LayerMeshKind k)
    {
        static const char* const stems[] = { "Tuft", "Pebble", "RockA", "RockB", "RockC", "SnowDrift",
                                              "Fern", "ForestStump", "FallenLog", "MossRock",
                                              "TallGrass", "FlowerTuft", "ScreeChip",
                                              "DuneGrass", "Cactus", "Sandstone" };
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
            case LayerMeshKind::Fern:        return kEngineFernMeshId;
            case LayerMeshKind::ForestStump: return kEngineForestStumpMeshId;
            case LayerMeshKind::FallenLog:   return kEngineFallenLogMeshId;
            case LayerMeshKind::MossRock:    return kEngineMossRockMeshId;
            case LayerMeshKind::TallGrass:   return kEngineTallGrassMeshId;
            case LayerMeshKind::FlowerTuft:  return kEngineFlowerTuftMeshId;
            case LayerMeshKind::ScreeChip:   return kEngineScreeChipMeshId;
            case LayerMeshKind::DuneGrass:   return kEngineDuneGrassMeshId;
            case LayerMeshKind::Cactus:      return kEngineCactusMeshId;
            case LayerMeshKind::Sandstone:   return kEngineSandstoneMeshId;
            default:                       return UUID{};
        }
    }

    // Detail levels the kind has, level 0 included.
    inline int layerMeshLevelCount(LayerMeshKind k)
    {
        switch (k)
        {
            case LayerMeshKind::Tuft: case LayerMeshKind::RockA:
            case LayerMeshKind::RockB: case LayerMeshKind::RockC:
            case LayerMeshKind::Fern: case LayerMeshKind::MossRock:
            case LayerMeshKind::TallGrass: case LayerMeshKind::FlowerTuft:
            case LayerMeshKind::DuneGrass: case LayerMeshKind::Sandstone: return 3;
            default:                                                       return 2;
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

    // The kinds a texture group offers, in the order the picker lists them. Mountains is the set of
    // Thema 180, exactly; a scene saved before groups (and a terrain whose material is no group's)
    // is Mountains.
    struct LayerMeshSet { const LayerMeshKind* kinds; int count; };
    inline LayerMeshSet layerMeshKindsOfGroup(AutoLandscapeGroupId g)
    {
        using K = LayerMeshKind;
        static constexpr K mountains[] = { K::Tuft, K::Pebble, K::RockA, K::RockB, K::RockC, K::SnowDrift };
        static constexpr K forest[]    = { K::Fern, K::ForestStump, K::FallenLog, K::MossRock };
        static constexpr K grassland[] = { K::TallGrass, K::FlowerTuft, K::Pebble, K::RockA, K::ScreeChip };
        static constexpr K desert[]    = { K::DuneGrass, K::Cactus, K::Sandstone, K::ScreeChip };
        switch (g)
        {
            case AutoLandscapeGroupId::Forest:    return { forest,    static_cast<int>(sizeof(forest)    / sizeof(K)) };
            case AutoLandscapeGroupId::Grassland: return { grassland, static_cast<int>(sizeof(grassland) / sizeof(K)) };
            case AutoLandscapeGroupId::Desert:    return { desert,    static_cast<int>(sizeof(desert)    / sizeof(K)) };
            default:                              return { mountains, static_cast<int>(sizeof(mountains) / sizeof(K)) };
        }
    }

    // The group of a terrain's material id; Mountains when it is none of the groups' (see above).
    inline AutoLandscapeGroupId layerMeshGroupOfMaterial(const UUID& materialId)
    {
        const int g = autoLandscapeGroupOfMaterial(materialId);
        return g < 0 ? AutoLandscapeGroupId::Mountains : static_cast<AutoLandscapeGroupId>(g);
    }

    // The kind whose file stem is `stem` (case-insensitive), or Count.
    inline LayerMeshKind layerMeshKindOfStem(const std::string& stem)
    {
        for (int k = 0; k < kLayerMeshKindCount; ++k)
        {
            const std::string s = layerMeshStem(static_cast<LayerMeshKind>(k));
            if (s.size() != stem.size()) continue;
            bool same = true;
            for (size_t i = 0; i < s.size() && same; ++i)
                same = std::tolower(static_cast<unsigned char>(s[i])) == std::tolower(static_cast<unsigned char>(stem[i]));
            if (same) return static_cast<LayerMeshKind>(k);
        }
        return LayerMeshKind::Count;
    }

    // Fills `l` with the kind's settings. The layer's seed stays (it keeps the layer's
    // own random stream), its mask too; the scatter is marked dirty. `group` is the texture
    // group of the terrain: the slope past which its material shows rock and the height of its
    // snow come from its table row (the Mountains row gives the numbers Thema 180 hard-coded).
    inline void applyLayerMeshPreset(FoliageLayer& l, LayerMeshKind k,
                                     AutoLandscapeGroupId group = AutoLandscapeGroupId::Mountains)
    {
        const AutoLandscapeGroup& row = autoLandscapeGroup(group);
        const float rockSlope = row.rockSlope;
        // A rock stands a little inside the rock texture's range (0.12 -> 0.15), to the hundredth.
        const float rockStart = std::floor((rockSlope + 0.03f) * 100.0f + 0.5f) / 100.0f;
        const float snowStart = row.snowHeight + 6.0f;                  // the snow line + its blend
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
                l.slopeMax = rockSlope;                               // below where the material turns to rock
                levels[0] = { 15.0f, 0.7f }; levels[1] = { 35.0f, 0.4f };
                break;
            case LayerMeshKind::Pebble:
                l.density = 0.20f; l.minScale = 0.5f; l.maxScale = 1.6f; l.drawDistance = 45.0f;
                l.slopeMax = 0.25f;                                   // dirt and the foot of a slope
                levels[0] = { 15.0f, 0.6f };
                break;
            case LayerMeshKind::RockA: case LayerMeshKind::RockB: case LayerMeshKind::RockC:
                l.density = 0.004f; l.minScale = 0.6f; l.maxScale = 2.4f; l.drawDistance = 160.0f;
                l.slopeMin = rockStart;                               // where the material shows rock
                l.castsShadow = true; l.shadowDistance = 60.0f; l.contributesAO = true;
                levels[0] = { 30.0f, 1.0f }; levels[1] = { 75.0f, 0.7f };
                break;
            case LayerMeshKind::SnowDrift:
                l.density = 0.01f; l.minScale = 0.8f; l.maxScale = 2.0f; l.drawDistance = 110.0f;
                l.heightMin = snowStart;                              // snow height 60 m + its blend
                l.slopeMax = 0.35f;                                   // snow does not hold on the steep
                levels[0] = { 40.0f, 0.8f };
                break;
            case LayerMeshKind::Fern:
                l.density = 0.10f; l.minScale = 0.7f; l.maxScale = 1.4f; l.drawDistance = 80.0f;
                l.slopeMax = rockSlope;                               // ferns take the soft ground, not the rocky
                levels[0] = { 18.0f, 0.7f }; levels[1] = { 40.0f, 0.4f };
                break;
            case LayerMeshKind::ForestStump:
                l.density = 0.0015f; l.minScale = 0.8f; l.maxScale = 1.4f; l.drawDistance = 140.0f;
                l.slopeMax = rockSlope;
                l.castsShadow = true; l.shadowDistance = 50.0f; l.contributesAO = true;
                levels[0] = { 35.0f, 0.8f };
                break;
            case LayerMeshKind::FallenLog:
                l.density = 0.001f; l.minScale = 0.8f; l.maxScale = 1.5f; l.drawDistance = 150.0f;
                l.slopeMax = rockSlope;                               // a log lying on a steep slope would slide
                l.castsShadow = true; l.shadowDistance = 50.0f; l.contributesAO = true;
                levels[0] = { 35.0f, 0.8f };
                break;
            case LayerMeshKind::MossRock:
                l.density = 0.006f; l.minScale = 0.5f; l.maxScale = 2.0f; l.drawDistance = 150.0f;
                l.slopeMin = rockStart;                               // where the material shows its mossy rock
                l.castsShadow = true; l.shadowDistance = 60.0f; l.contributesAO = true;
                levels[0] = { 30.0f, 1.0f }; levels[1] = { 70.0f, 0.7f };
                break;
            case LayerMeshKind::TallGrass:
                l.density = 0.40f; l.minScale = 0.8f; l.maxScale = 1.6f; l.drawDistance = 80.0f;
                l.slopeMax = rockSlope;
                levels[0] = { 18.0f, 0.7f }; levels[1] = { 40.0f, 0.4f };
                break;
            case LayerMeshKind::FlowerTuft:
                l.density = 0.06f; l.minScale = 0.7f; l.maxScale = 1.3f; l.drawDistance = 55.0f;
                l.slopeMax = rockSlope;
                levels[0] = { 15.0f, 0.7f }; levels[1] = { 30.0f, 0.4f };
                break;
            case LayerMeshKind::ScreeChip:
                l.density = 0.05f; l.minScale = 0.5f; l.maxScale = 1.4f; l.drawDistance = 70.0f;
                l.slopeMin = rockStart;                               // loose stones where the Stones texture shows
                levels[0] = { 20.0f, 0.6f };
                break;
            case LayerMeshKind::DuneGrass:
                l.density = 0.05f; l.minScale = 0.7f; l.maxScale = 1.5f; l.drawDistance = 75.0f;
                l.slopeMax = rockSlope;                               // sparse tufts on the sand, not on the cliffs
                levels[0] = { 18.0f, 0.7f }; levels[1] = { 40.0f, 0.4f };
                break;
            case LayerMeshKind::Cactus:
                l.density = 0.0015f; l.minScale = 0.8f; l.maxScale = 2.2f; l.drawDistance = 150.0f;
                l.slopeMax = rockSlope;
                l.castsShadow = true; l.shadowDistance = 50.0f; l.contributesAO = true;
                levels[0] = { 35.0f, 0.8f };
                break;
            case LayerMeshKind::Sandstone:
                l.density = 0.005f; l.minScale = 0.6f; l.maxScale = 2.2f; l.drawDistance = 160.0f;
                l.slopeMin = rockStart;                               // where the material shows its sandstone
                l.castsShadow = true; l.shadowDistance = 60.0f; l.contributesAO = true;
                levels[0] = { 30.0f, 1.0f }; levels[1] = { 75.0f, 0.7f };
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
