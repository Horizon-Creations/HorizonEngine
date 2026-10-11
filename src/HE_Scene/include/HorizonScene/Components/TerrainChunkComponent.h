#pragma once
#include <entt/entt.hpp>
#include <Types/UUID.h>
#include "WaterSurfaceComponent.h"
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
    // tessActive = the mesh is filled and hooked in as LODComponent::refinedMeshId.
    HE::UUID     tessMeshId{};
    bool         tessActive = false;
};

namespace HE
{
    // Everything a landscape generates for itself: its chunk entities and its water
    // surfaces. None of them is saved, listed, replicated or picked on its own —
    // the TerrainComponent (heights, paint, water field) is the source of truth and
    // TerrainSystem recreates the rest. One predicate so that a new kind of
    // generated child is added HERE and not forgotten at the ten places that have
    // to skip it (serializer, outliner, replication, picking, …).
    inline bool isTerrainGenerated(const entt::registry& reg, entt::entity e)
    {
        return reg.all_of<TerrainChunkComponent>(e) || reg.all_of<WaterSurfaceComponent>(e);
    }

    // The landscape a generated child belongs to, or entt::null.
    inline entt::entity terrainOwnerOf(const entt::registry& reg, entt::entity e)
    {
        if (const auto* c = reg.try_get<TerrainChunkComponent>(e)) return c->terrain;
        if (const auto* w = reg.try_get<WaterSurfaceComponent>(e)) return w->terrain;
        return entt::null;
    }
}
