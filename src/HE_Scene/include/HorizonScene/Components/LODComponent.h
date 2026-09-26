#pragma once
#include <cstdint>
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

    // Optional level FINER than levels[0], chosen first when the camera is
    // within refinedMaxDistance (terrain tessellation). Kept OUT of `levels` on
    // purpose: navigation, physics, the extractor and the editor read
    // levels[0] as "the full-detail mesh", and that has to stay LOD0 rather
    // than a displaced, many-times-denser runtime mesh. Runtime only, never
    // serialised. current == kRefined while it is the drawn one.
    static constexpr uint8_t kRefined = 0xFF;
    HE::UUID              refinedMeshId{};
    float                 refinedMaxDistance = 0.0f;
};
