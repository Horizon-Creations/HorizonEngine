#pragma once
#include <Types/UUID.h>
#include <vector>
#include <limits>

struct LODLevel {
    HE::UUID meshId;
    float    maxDistance = std::numeric_limits<float>::max();
};

// Attaches LOD levels to a mesh entity. Levels must be sorted by maxDistance
// (nearest first). LODSystem::update() picks the first level whose maxDistance
// >= camera distance and writes its meshId into MeshComponent::meshAssetId.
struct LODComponent {
    std::vector<LODLevel> levels;
    uint8_t               current = 0; // index of currently active level
};
