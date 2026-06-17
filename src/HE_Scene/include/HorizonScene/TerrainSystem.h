#pragma once

class HorizonWorld;
class ContentManager;

namespace TerrainSystem
{
    // Iterates all entities with TerrainComponent; for those with dirty=true,
    // generates a new mesh, registers or replaces it in ContentManager, and
    // updates (or creates) the entity's MeshComponent.
    void updateTerrains(HorizonWorld& world, ContentManager& cm);
}
