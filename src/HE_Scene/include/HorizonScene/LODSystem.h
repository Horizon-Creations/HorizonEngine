#pragma once
#include <glm/glm.hpp>

class HorizonWorld;
class ContentManager;

namespace LODSystem {
    // Gives every mesh entity whose static mesh carries detail levels in its own
    // asset (StaticMeshAsset::lods) a LODComponent mirroring them (fromAsset),
    // with distances from each level's error at the default screen tolerance and
    // the entity's scale — so a mesh with levels switches them wherever it is
    // used, with nothing set up per entity. Keeps those components in step with
    // the asset (regenerated levels, or none) and leaves hand-made ones alone.
    // Levels that are not resident yet (a pak) are asked for first and bound once
    // they are. Cheap: it runs in full only when content or the mesh entity count
    // changed, or every few dozen calls.
    void bindAssetLevels(HorizonWorld& world, ContentManager& cm);

    // For every entity with LODComponent + MeshComponent:
    // selects the first LOD level whose maxDistance >= dist(entity, cameraPos)
    // and writes its meshId into MeshComponent::meshAssetId.
    void update(HorizonWorld& world, const glm::vec3& cameraPos);
}
