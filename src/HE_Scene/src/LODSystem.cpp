#include "HorizonScene/LODSystem.h"
#include <cstdint>
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/TransformHierarchy.h"   // worldPositionOf — see update()
#include <glm/glm.hpp>

void LODSystem::update(HorizonWorld& world, const glm::vec3& cameraPos)
{
    auto& registry = world.registry();

    auto view = registry.view<LODComponent, MeshComponent>();
    for (auto [entity, lod, mesh] : view.each())
    {
        if (lod.levels.empty()) continue;

        // Use the WORLD position, not the local one — terrain chunks are children,
        // so their local position is a per-chunk offset; only the world position
        // gives the right per-chunk camera distance.
        //
        // Composed from the parent chain rather than read out of tc.worldMatrix.
        // That matrix is only as fresh as the last propagateTransforms, and NOTHING
        // in SceneSystems::tickWorld runs one: for an entity created this frame it
        // is still the identity. Terrain is exactly that case, and it is not a
        // corner one — TerrainSystem builds the chunk entities at the TOP of
        // tickWorld and LOD picks their level at the BOTTOM of the same tick, so a
        // fresh chunk grid would measure every chunk from the world origin and
        // settle on one level for all of them, then pop a frame later.
        //
        // Identity-safe: an entity without a TransformComponent answers (0,0,0),
        // the same fallback this had before.
        const glm::vec3 pos = HE::worldPositionOf(world, entity);

        const float dist = glm::distance(cameraPos, pos);

        uint8_t chosen = static_cast<uint8_t>(lod.levels.size() - 1);
        for (uint8_t i = 0; i < static_cast<uint8_t>(lod.levels.size()); ++i)
        {
            if (dist <= lod.levels[i].maxDistance)
            {
                chosen = i;
                break;
            }
        }

        if (lod.current != chosen || mesh.meshAssetId != lod.levels[chosen].meshId)
        {
            lod.current        = chosen;
            mesh.meshAssetId   = lod.levels[chosen].meshId;
            mesh.dirty         = true;
        }
    }
}
