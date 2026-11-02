#pragma once
#include <Types/UUID.h>
#include <glm/vec3.hpp>
#include <cstdint>

// Marks an entity LandscapeWorldProxies made for a tile landscape (Thema 326): one per (tile, foliage layer) and one per water sheet. Like a terrain
// chunk or a water surface it is NEVER serialized and HIDDEN from the Outliner (isTerrainGenerated, TerrainChunkComponent.h): the tile landscape
// (LandscapeRuntime) owns what it stands for and builds it again every time the camera moves on.
struct LandscapeProxyComponent {
    enum class Kind : uint8_t { Foliage, Water };
    Kind       kind = Kind::Foliage;
    // The absolute position of the entity's local origin. The entity stands at centre - world.origin(), refreshed every sync, so the
    // floating origin can move without anything being rebuilt.
    glm::dvec3 centre{ 0.0 };
    // Water: the sheet mesh this entity owns (registered once, given back with the entity).
    HE::UUID   meshId{};
    // Water: the region revision the mesh came from.
    uint64_t   revision = 0;
};
