#pragma once

class HorizonWorld;

namespace FoliageSystem {
    // For every entity with FoliageComponent + TerrainComponent where dirty=true:
    // scatter instances on the terrain surface and cache the transforms. A
    // painted densityMask (see FoliagePaint) thins the scatter per spot and
    // keeps erased areas empty; without one the layer is uniform.
    //
    // The scatter also lands, sorted into a grid of buckets and relative to the
    // terrain, in FoliageComponent::store — what the renderer reads. This is the
    // only writer of cachedInstances, store and revision: the revision counts every
    // re-scatter and every change of a setting the extraction depends on.
    void update(HorizonWorld& world);
}
