#pragma once
#include <Types/UUID.h>
#include <Math/Math.h>
#include <HorizonScene/FoliageStore.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ── A farther detail level of a foliage layer ───────────────────────────────
// The layer's own mesh and material are level 0 and draw from the camera out to
// the first level's `distance`; each level here takes over from its `distance`
// until the next one's, the last until the layer's drawDistance. A plant's level
// is chosen by its own ground-plane distance (FoliageExtract.cpp), so a level
// boundary through a bucket splits the bucket instead of popping it whole.
// The last level is typically the engine's cross card (kEngineCrossCardMeshId)
// with a material that carries a picture of the plant.
struct FoliageLod {
    // Null: the layer's own (a level that only thins). The material falls back
    // to the LAYER's material, not to the level mesh's own.
    HE::UUID meshAssetId;
    HE::UUID materialAssetId;
    float    distance = 50.0f;         // m, ground plane: where this level starts
    // Share of the layer's plants this level still draws, 0..1 (0: none). The plants kept
    // are the first of each bucket, which is an even thinning because the scatter
    // order is random (FoliageStore).
    float    density  = 1.0f;
};

// ── One kind of plant on a landscape ────────────────────────────────────────
// Grass, a bush, a tree, pebbles: each is a layer with its own mesh, material,
// density, seed, mask and draw settings, scattered on the terrain of the entity
// that carries the FoliageComponent. Layers are independent: painting one mask,
// changing one density re-scatters that layer only.
struct FoliageLayer {
    // Shown in the inspector and the Landscape panel; empty = "Layer N".
    std::string name;
    // The layer's own switch, beside the component's (zone hiding). The extractor
    // skips a hidden layer; its scatter stays, so showing it again is instant.
    bool     visible = true;
    HE::UUID meshAssetId;
    HE::UUID materialAssetId;
    float    density      = 0.1f;    // instances per unit area
    int      seed         = 42;
    float    minScale     = 0.8f;
    float    maxScale     = 1.2f;
    float    drawDistance = 80.f;    // instances beyond this distance are not submitted
    bool     dirty        = true;    // set true to regenerate this layer's cachedInstances

    // ── Painted density mask ────────────────────────────────────────────────
    // One byte per texel, row-major over the terrain's 0..1 UV range like the
    // terrain's layerWeights: 255 = the full `density`, 0 = nothing grows here
    // (an exclusion area — a road, a yard, a lake), anything between thins the
    // scatter proportionally. EMPTY = unpainted, the layer stays uniform over
    // the whole landscape exactly as before the mask existed, so old scenes
    // and layers nobody painted look the same. FoliagePaint edits it, the
    // Landscape panel's Foliage brush drives that, and FoliageSystem samples it
    // when it scatters. Serialised inline (base64) with the component.
    uint32_t             maskRes = 128;   // mask side length in texels
    std::vector<uint8_t> densityMask;     // maskRes² bytes, or empty

    // What the extraction reads besides the scatter. Every default is "as before
    // the layer list existed"; the serializer writes a field only when it differs.
    float bucketSize     = 32.0f;   // m, the grid the store is sorted into (a divisor of the 256 m cells)
    bool  castsShadow    = true;    // false: the layer is never submitted to a shadow map
    bool  contributesAO  = true;    // false: the layer stays out of the SSAO pre-pass (grass)
    float shadowDistance = 0.0f;    // m; beyond it a bucket stops casting shadows. 0 = drawDistance

    // Farther detail levels, ordered by distance (the loader sorts them). Empty =
    // the layer's mesh all the way to drawDistance, as before levels existed.
    static constexpr size_t kMaxLods = 7;
    std::vector<FoliageLod> lods;
    // m: a plant that has switched level keeps it until it is this far past the
    // boundary, so a camera that wobbles on a boundary does not flip plants every
    // frame. Not applied at drawDistance, which stays the hard edge it always was.
    float lodHysteresis = 2.0f;

    // ── Placement rules (a layer's mesh grows only where the ground fits) ───
    // The landscape material shows grass, dirt, rock and snow by slope, height
    // and noise in its pixel shader; a pebble, a rock or a tuft of grass has to
    // stand where that surface is visible, so the CPU scatter judges the same
    // things. Every default is "no rule" (the layer scatters as it did before
    // the rules existed); the serializer writes a field only when it differs.
    // A rule only REJECTS candidates, the survivors keep the pose they have in
    // the unruled layout (like the density mask), and changing one re-scatters
    // this layer alone.
    //
    // Steepness of the ground, 1 - N.y (the landscape material's own measure):
    // 0 flat, ~0.29 a 45 degree hillside, 1 a wall. A candidate stays when
    // slopeMin <= steepness <= slopeMax.
    float slopeMin = 0.0f;
    float slopeMax = 1.0f;
    // World-space height of the ground (the terrain entity's own height added),
    // in metres: heightMin <= y <= heightMax.
    static constexpr float kNoHeightLimit = 10000.0f;
    float heightMin = -kNoHeightLimit;
    float heightMax =  kNoHeightLimit;
    // The paint layer whose weight gates the scatter: 0..3 are the channels of
    // TerrainComponent::layerWeights, 4..7 of layerWeights2; -1 = ignore the
    // paint. A candidate is kept with the probability the layer's weight gives
    // at that spot (255 = always, 0 = never), so a rock layer painted onto
    // "Rock" grows rocks on the rock and thins out at its edge. An unpainted
    // landscape (no weightmap) counts as layer 0 fully, like the shader does.
    int   weightLayer = -1;

    // ── Runtime (not serialised) ────────────────────────────────────────────
    // Per-instance model matrices as the scatter produced them: WORLD space at
    // scatter time (origin = the terrain entity's position), in generation order.
    // The editor's read-outs and the scatter tests read this; the RENDERER does
    // not — it reads `store` below, which also follows a terrain that is rotated,
    // scaled, parented or moved after the scatter.
    std::vector<glm::mat4> cachedInstances;

    // The scatter again, terrain-local and sorted into buckets. Rebuilt with
    // cachedInstances, shared with every RenderWorld extracted from it.
    std::shared_ptr<const FoliageStore> store;
};

// Scatters foliage mesh instances across the terrain of the same entity, one
// scatter per layer. FoliageSystem::update() computes each layer's
// cachedInstances (and the bucketed `store` the renderer reads) when the layer,
// or the whole component, is dirty.
// RenderExtractor does NOT walk the instances: it classifies each layer's
// buckets against the camera and emits one cluster RenderObject per visible
// bucket (FoliageExtract.cpp). GeometryPass / RenderSorter::batchDepthRuns
// unfold a cluster into its instances when they form draws, so the GPU
// instancing the same-mesh batching always gave is unchanged.
struct FoliageComponent {
    // More than this many layers on one landscape is a scene nobody authored by
    // hand; the editor stops adding there, the loader drops the rest, and the
    // collab mask sync (TerrainSync, one grid id per layer) relies on the bound.
    static constexpr size_t kMaxLayers = 32;

    bool visible = true;    // extractor skips invisible (zone hiding)
    // Re-scatter EVERY layer: the ground under them moved (sculpt, Mountain,
    // heightmap import). A change to one layer sets that layer's own `dirty`.
    bool dirty   = true;

    // Never empty in a component the loader or the editor made: a fresh
    // component is one default layer, as the component was before layers existed.
    std::vector<FoliageLayer> layers = std::vector<FoliageLayer>(1);

    // ── Runtime (not serialised) ────────────────────────────────────────────
    // Bumped by FoliageSystem::update on every re-scatter of any layer and on every
    // change of a setting the extraction depends on (mesh, material, distances,
    // scale range, visibility, the flags, the layer count). A retained / persistent
    // RenderWorld can key on it to know whether the foliage it holds is stale
    // (same pattern as HorizonWorld::structureEpoch).
    uint32_t revision = 0;

    // FoliageSystem's memory of the settings the last `revision` bump saw.
    uint64_t settingsKey = 0;

    // The layer the editor works on: the inspector shows its fields and the
    // Landscape panel's Foliage brush paints its mask. Editor state, so it is
    // not saved; clamp it before use (an undo or a peer can remove layers).
    int activeLayer = 0;

    FoliageLayer*       active()       { return layers.empty() ? nullptr : &layers[activeIndex()]; }
    const FoliageLayer* active() const { return layers.empty() ? nullptr : &layers[activeIndex()]; }
    size_t activeIndex() const
    {
        if (layers.empty() || activeLayer < 0) return 0;
        return static_cast<size_t>(activeLayer) < layers.size()
             ? static_cast<size_t>(activeLayer) : layers.size() - 1;
    }

    // Appends an empty layer (no mesh yet) and makes it the active one; null when
    // kMaxLayers are there. Its seed is one no other layer uses, so two layers
    // given the same mesh do not grow in exactly the same spots.
    FoliageLayer* addLayer()
    {
        if (layers.size() >= kMaxLayers) return nullptr;
        FoliageLayer l;
        l.name = "Layer " + std::to_string(layers.size() + 1);
        for (bool taken = true; taken;)
        {
            taken = std::any_of(layers.begin(), layers.end(),
                                 [l](const FoliageLayer& o) { return o.seed == l.seed; });
            if (taken) l.seed += 7919;
        }
        layers.push_back(std::move(l));
        activeLayer = static_cast<int>(layers.size() - 1);
        return &layers.back();
    }
};

// What the editor calls layer `index`: its name, or "Layer N" while it has none.
inline std::string foliageLayerLabel(const FoliageLayer& l, size_t index)
{
    return l.name.empty() ? "Layer " + std::to_string(index + 1) : l.name;
}
