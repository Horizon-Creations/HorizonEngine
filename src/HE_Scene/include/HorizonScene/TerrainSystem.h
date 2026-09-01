#pragma once

class HorizonWorld;
class ContentManager;
class IRenderer;
class PhysicsWorld;

namespace TerrainSystem
{
    // Iterates all entities with TerrainComponent; for those with dirty=true,
    // generates a new mesh, registers or replaces it in ContentManager, and
    // updates (or creates) the entity's MeshComponent.
    // Pass renderer != nullptr to call InvalidateMesh after replaceStaticMesh so
    // the GPU VBO cache is evicted and re-uploaded the same frame.
    void updateTerrains(HorizonWorld& world, ContentManager& cm,
                        IRenderer* renderer = nullptr);

    // Same, plus: a terrain whose heights changed has its collider rebuilt, so
    // sculpting while the game runs moves the collision with the ground instead
    // of leaving the player standing on the pre-stroke shape.
    //
    // `physics` is nullable and null is the normal state outside play. This is a
    // SEPARATE overload rather than a fourth defaulted parameter on the one
    // above ON PURPOSE: a default here would make every existing three-argument
    // call ambiguous against that overload and HE_Scene would stop compiling.
    void updateTerrains(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                        PhysicsWorld* physics);
}
