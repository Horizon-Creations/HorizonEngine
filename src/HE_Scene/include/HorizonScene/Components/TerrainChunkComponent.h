#pragma once
#include <entt/entt.hpp>
#include <Types/UUID.h>
#include <cstdint>

// Marks a runtime-generated terrain chunk entity. Chunk entities are children of
// a terrain entity, each carrying a MeshComponent + LODComponent so the existing
// LODSystem (distance → mesh swap) and frustum culler optimise distant/off-screen
// terrain automatically. They are NEVER serialized and HIDDEN from the Outliner —
// the TerrainSystem recreates them from the TerrainComponent (same as the mesh).
struct TerrainChunkComponent {
    entt::entity terrain = entt::null;  // owning terrain entity
    uint32_t     cx = 0, cz = 0;        // chunk grid coordinate

    // Tessellated level (TerrainComponent::tessellationFactor). The mesh is
    // registered ONCE per chunk and afterwards only replaced — emptied when the
    // camera leaves, refilled when it comes back — because every registration
    // can move the content manager's pool under other holders' pointers.
    // tessActive = the mesh is filled and sits in front of LODComponent::levels.
    HE::UUID     tessMeshId{};
    bool         tessActive = false;
};
