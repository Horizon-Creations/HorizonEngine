#pragma once
#include <HorizonScene/Components/NavMeshComponent.h>

class HorizonWorld;

namespace NavigationSystem {
    // Build (or rebuild) the NavMesh stored in the given component.
    // Returns true on success. Must be called after geometry is provided.
    bool bake(NavMeshComponent& nmc);

    // Advance agent movement along their computed paths.  For each entity
    // with a NavAgentComponent and a TransformComponent, queries the NavMesh
    // held by the first NavMeshComponent found in the world, computes a path
    // if none exists, and moves along it according to speed*dt.
    void update(HorizonWorld& world, float dt);
}
