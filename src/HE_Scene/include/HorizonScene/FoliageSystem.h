#pragma once

class HorizonWorld;

namespace FoliageSystem {
    // For every entity with FoliageComponent + TerrainComponent where dirty=true:
    // scatter instances on the terrain surface and cache the transforms.
    void update(HorizonWorld& world);
}
