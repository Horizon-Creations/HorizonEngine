#pragma once

class HorizonWorld;
class ContentManager;
class IRenderer;

namespace TerrainSystem
{
    // Iterates all entities with TerrainComponent; for those with dirty=true,
    // generates a new mesh, registers or replaces it in ContentManager, and
    // updates (or creates) the entity's MeshComponent.
    // Pass renderer != nullptr to call InvalidateMesh after replaceStaticMesh so
    // the GPU VBO cache is evicted and re-uploaded the same frame.
    void updateTerrains(HorizonWorld& world, ContentManager& cm,
                        IRenderer* renderer = nullptr);
}
