#pragma once
#include <Types/UUID.h>
#include <Math/Math.h>
#include <HorizonScene/FoliageStore.h>
#include <cstdint>
#include <memory>
#include <vector>

// Scatters foliage mesh instances across the terrain of the same entity.
// FoliageSystem::update() computes cachedInstances (and the bucketed `store`
// the renderer reads) when dirty is true.
// RenderExtractor does NOT walk the instances: it classifies the store's
// buckets against the camera and emits one cluster RenderObject per visible
// bucket (FoliageExtract.cpp). GeometryPass / RenderSorter::batchDepthRuns
// unfold a cluster into its instances when they form draws, so the GPU
// instancing the same-mesh batching always gave is unchanged.
struct FoliageComponent {
    bool     visible = true;    // extractor skips invisible (zone hiding)
    HE::UUID meshAssetId;
    HE::UUID materialAssetId;
    float    density      = 0.1f;    // instances per unit area
    int      seed         = 42;
    float    minScale     = 0.8f;
    float    maxScale     = 1.2f;
    float    drawDistance = 80.f;    // instances beyond this distance are not submitted
    bool     dirty        = true;    // set true to regenerate cachedInstances

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

    // Per-instance model matrices as the scatter produced them: WORLD space at
    // scatter time (origin = the terrain entity's position), in generation order.
    // The editor's read-outs and the scatter tests read this; the RENDERER does
    // not — it reads `store` below, which also follows a terrain that is rotated,
    // scaled, parented or moved after the scatter.
    std::vector<glm::mat4> cachedInstances;

    // ── Render side (runtime only, not serialised) ──────────────────────────
    // The scatter again, terrain-local and sorted into buckets. Rebuilt with
    // cachedInstances, shared with every RenderWorld extracted from it.
    std::shared_ptr<const FoliageStore> store;

    // Bumped by FoliageSystem::update on every re-scatter and on every change of
    // a setting the extraction depends on (mesh, material, distances, scale range,
    // visibility, the flags below). A retained / persistent RenderWorld can key on
    // it to know whether the foliage it holds is stale (same pattern as
    // HorizonWorld::structureEpoch).
    uint32_t revision = 0;

    // What the extraction reads besides the scatter. One layer per terrain for now,
    // so these live here with their defaults (every one is "as before"); the layer
    // list that makes them per-layer settings is a later step.
    float bucketSize     = 32.0f;   // m, the grid the store is sorted into (a divisor of the 256 m cells)
    bool  castsShadow    = true;    // false: the layer is never submitted to a shadow map
    bool  contributesAO  = true;    // false: the layer stays out of the SSAO pre-pass (grass)
    float shadowDistance = 0.0f;    // m; beyond it a bucket stops casting shadows. 0 = drawDistance

    // FoliageSystem's memory of the settings the last `revision` bump saw.
    uint64_t settingsKey = 0;
};
