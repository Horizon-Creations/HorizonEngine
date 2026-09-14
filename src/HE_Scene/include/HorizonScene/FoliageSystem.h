#pragma once

class HorizonWorld;

namespace FoliageSystem {
    // For every entity with FoliageComponent + TerrainComponent where dirty=true:
    // scatter instances on the terrain surface and cache the transforms. A
    // painted densityMask (see FoliagePaint) thins the scatter per spot and
    // keeps erased areas empty; without one the layer is uniform.
    void update(HorizonWorld& world);
}
