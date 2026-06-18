#pragma once
#include <glm/glm.hpp>

class HorizonWorld;

namespace LODSystem {
    // For every entity with LODComponent + MeshComponent:
    // selects the first LOD level whose maxDistance >= dist(entity, cameraPos)
    // and writes its meshId into MeshComponent::meshAssetId.
    void update(HorizonWorld& world, const glm::vec3& cameraPos);
}
