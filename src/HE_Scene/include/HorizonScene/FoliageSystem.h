#pragma once

#include <Math/Math.h>
#include <cstdint>

class HorizonWorld;
struct TerrainComponent;
struct FoliageLayer;

namespace FoliageSystem {
    // For every entity with FoliageComponent + TerrainComponent, every layer that
    // is dirty (or all of them, when the component is): scatter instances on the
    // terrain surface and cache the transforms. A layer's painted densityMask (see
    // FoliagePaint) thins its scatter per spot and keeps erased areas empty;
    // without one the layer is uniform. Layers scatter independently, each from
    // its own seed.
    //
    // The scatter also lands, sorted into a grid of buckets and relative to the
    // terrain, in FoliageLayer::store — what the renderer reads. This is the
    // only writer of cachedInstances, store and revision: the revision counts every
    // re-scatter and every change of a setting the extraction depends on.
    void update(HorizonWorld& world);

    // Scatters ONE layer over a terrain that is not an entity: clears the layer's cachedInstances and store and fills them. The same
    // layout the entity path gives for the same terrain, layer settings and `origin` (the terrain's world position); the instances
    // in `store` are terrain-local, centred on the terrain. Used per tile by the world-scale landscape. Logs nothing about density.
    void scatterLayer(const TerrainComponent& terrain, const glm::vec3& origin, FoliageLayer& layer, uint32_t logId = 0);
}
