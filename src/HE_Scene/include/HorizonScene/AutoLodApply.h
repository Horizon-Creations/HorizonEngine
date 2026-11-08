#pragma once
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <ContentManager/DefaultAssets.h>
#include <Math/LodDistance.h>
#include <algorithm>
#include <vector>

// Fills the consumers of an Auto-LOD chain (Thema 331, step 3). The generator lives
// in the tool library (AutoLod.h, HorizonImporters), which the scene library does
// not link, so a chain arrives here as plain data: the sidecar meshes of LOD1.. in
// order, each with the object-space error it carries against LOD0.
//
// Nothing here is serialised in a new way: the results are ordinary FoliageLod
// entries and LODComponent levels, which the scene format already stores. A scene
// written before this existed therefore loads the same, and one written with a
// chain loads in an older build as a layer with levels it simply draws.
namespace HE::AutoLodApply
{

struct Level
{
    HE::UUID mesh;                 // LOD1, LOD2 … in order
    float    worldError = 0.0f;    // metres, object space, against LOD0
};

struct FoliageOptions
{
    LodScreenMetrics screen;
    // Mean plant scale: a plant drawn 1.5x as big shows its error 1.5x as big.
    float instanceScale = 1.0f;
    // End the chain on a cross card. With no impostor below it is the engine's own
    // card drawn with the LAYER's material (no picture of the plant), so it is off
    // unless asked for.
    bool  crossCard = false;
    // The baked impostor (Impostor.h): its card replaces the engine's, and its material
    // (a picture of this plant) replaces the layer's. Null = the engine card.
    HE::UUID impostorMesh;
    HE::UUID impostorMaterial;
    // Height of the baked plant in metres (card height x instanceScale), 0 = unknown.
    // The card does not take over before the plant is down to kImpostorPixels tall.
    float    impostorHeight = 0.0f;
};

// A card replaces geometry only once the whole plant is this many pixels tall or less
// (1080 p, 60 degrees): above it the card's flat, two-view look shows. The distance
// this gives is the one for a plant of the layer's mean scale; the extractor scales it
// per plant (FoliageExtract.cpp Levels::seen), so a bigger plant keeps its geometry
// farther and the card takes over at the same pixel height for every plant.
constexpr float kImpostorPixels = 128.0f;

inline float impostorStartDistance(float plantHeight)
{
    LodScreenMetrics m;
    m.tolerancePx = kImpostorPixels;
    return lodDistanceForError(plantHeight, m);
}

// Share of the layer's plants a level still draws: the first two extra levels draw
// all of them (the plant is still large), every one beyond thins by a quarter, to a
// floor of 30 %. A thinner far field is where the draw-count win of a chain is.
inline float lodDensityFor(size_t index)
{
    if (index < 2) return 1.0f;
    return std::max(0.3f, 1.0f - 0.25f * static_cast<float>(index - 1));
}

// Replaces layer.lods with the chain. The layer's own mesh stays LOD0. Returns the
// number of levels written (the layer holds FoliageLayer::kMaxLods at most).
inline size_t fillFoliageLayer(FoliageLayer& layer, const std::vector<Level>& chain, const FoliageOptions& opt)
{
    std::vector<float> errors;
    errors.reserve(chain.size());
    const float scale = std::max(opt.instanceScale, 0.01f);
    for (const Level& l : chain) errors.push_back(l.worldError * scale);
    const std::vector<float> dist = lodDistancesFromErrors(errors, opt.screen);

    // A level that would start beyond the layer's drawDistance never draws, and the
    // cutoff is the user's choice (it is what the layer costs): levels past it are
    // left out instead of pulling the cutoff outwards.
    layer.lods.clear();
    for (size_t i = 0; i < chain.size() && layer.lods.size() < FoliageLayer::kMaxLods; ++i)
    {
        if (dist[i] >= layer.drawDistance) break;
        FoliageLod d;
        d.meshAssetId = chain[i].mesh;
        d.distance    = dist[i];
        d.density     = lodDensityFor(i);
        layer.lods.push_back(d);
    }
    if (opt.crossCard && !layer.lods.empty() && layer.lods.size() < FoliageLayer::kMaxLods)
    {
        const float last = layer.lods.back().distance;
        if (last < layer.drawDistance)
        {
            FoliageLod d;
            d.meshAssetId     = opt.impostorMesh == HE::UUID{} ? HE::kEngineCrossCardMeshId : opt.impostorMesh;
            d.materialAssetId = opt.impostorMesh == HE::UUID{} ? layer.materialAssetId : opt.impostorMaterial;
            d.distance        = std::min(last * 1.8f, 0.5f * (last + layer.drawDistance));
            if (opt.impostorMesh != HE::UUID{} && opt.impostorHeight > 0.0f)
                d.distance = std::max(d.distance, impostorStartDistance(opt.impostorHeight));
            d.density         = lodDensityFor(layer.lods.size());
            if (d.distance < layer.drawDistance) layer.lods.push_back(d);
        }
    }

    // A level change is a swap of the plant's mesh: it must not flip back each frame
    // a camera wobbles on the boundary. A tenth of the tightest band, 1..10 m.
    float tightest = layer.lods.empty() ? 20.0f : layer.lods.front().distance;
    for (size_t i = 1; i < layer.lods.size(); ++i)
        tightest = std::min(tightest, layer.lods[i].distance - layer.lods[i - 1].distance);
    layer.lodHysteresis = std::clamp(tightest * 0.1f, 1.0f, 10.0f);

    layer.dirty = true;
    return layer.lods.size();
}

// Puts the baked impostor on the layer's FAR end without touching the chain before it.
// A last level that already is a cross card (the engine's, or `previousCard` — the card
// of an earlier bake) is replaced in place, keeping its distance and density, so baking
// again changes the picture and nothing else. Otherwise a level is appended: it starts
// where the nearest-but-one would (past the last level by 1.8x, or half-way to
// `drawDistance`), but never before the plant is kImpostorPixels tall (`plantHeight`, metres
// with the instance scale; 0 = unknown), and thins like any level of its index. Returns false when the layer
// has no room (kMaxLods) or the impostor starts past `drawDistance`.
inline bool applyImpostorLevel(FoliageLayer& layer, HE::UUID card, HE::UUID material, float plantHeight = 0.0f,
                               HE::UUID previousCard = {})
{
    if (!layer.lods.empty())
    {
        FoliageLod& last = layer.lods.back();
        const bool isCard = last.meshAssetId == HE::kEngineCrossCardMeshId
                         || (previousCard != HE::UUID{} && last.meshAssetId == previousCard)
                         || last.meshAssetId == card;
        if (isCard)
        {
            last.meshAssetId     = card;
            last.materialAssetId = material;
            layer.dirty = true;
            return true;
        }
    }
    if (layer.lods.size() >= FoliageLayer::kMaxLods) return false;
    FoliageLod d;
    d.meshAssetId     = card;
    d.materialAssetId = material;
    d.distance        = layer.lods.empty() ? 0.5f * layer.drawDistance
                                           : std::min(layer.lods.back().distance * 1.8f,
                                                      0.5f * (layer.lods.back().distance + layer.drawDistance));
    if (plantHeight > 0.0f) d.distance = std::max(d.distance, impostorStartDistance(plantHeight));
    if (d.distance >= layer.drawDistance) return false;
    d.density = lodDensityFor(layer.lods.size());
    layer.lods.push_back(d);
    layer.dirty = true;
    return true;
}

// Mesh entity: levels[0] = LOD0 (navigation, physics and the extractor read it as
// "the full-detail mesh"), each level up to the next one's start, the last open.
// `entityScale` is the entity's largest scale component.
inline void fillLodComponent(LODComponent& lod, HE::UUID lod0, const std::vector<Level>& chain,
                             const LodScreenMetrics& screen, float entityScale)
{
    std::vector<float> errors;
    const float scale = std::max(entityScale, 0.01f);
    for (const Level& l : chain) errors.push_back(l.worldError * scale);
    const std::vector<float> dist = lodDistancesFromErrors(errors, screen);

    lod.levels.clear();
    lod.current = 0;
    LODLevel first;
    first.meshId      = lod0;
    first.maxDistance = dist.empty() ? first.maxDistance : dist.front();
    lod.levels.push_back(first);
    for (size_t i = 0; i < chain.size(); ++i)
    {
        LODLevel lvl;
        lvl.meshId = chain[i].mesh;
        if (i + 1 < chain.size()) lvl.maxDistance = dist[i + 1];
        lod.levels.push_back(lvl);
    }
}

} // namespace HE::AutoLodApply
