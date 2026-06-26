#pragma once
#include <entt/entt.hpp>
#include <cstdint>

// Marks a runtime-generated terrain chunk entity. Chunk entities are children of
// a terrain entity, each carrying a MeshComponent + LODComponent so the existing
// LODSystem (distance → mesh swap) and frustum culler optimise distant/off-screen
// terrain automatically. They are NEVER serialized and HIDDEN from the Outliner —
// the TerrainSystem recreates them from the TerrainComponent (same as the mesh).
struct TerrainChunkComponent {
    entt::entity terrain = entt::null;  // owning terrain entity
    uint32_t     cx = 0, cz = 0;        // chunk grid coordinate
};
