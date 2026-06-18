#include "HorizonScene/LODSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <glm/glm.hpp>

void LODSystem::update(HorizonWorld& world, const glm::vec3& cameraPos)
{
    auto& registry = world.registry();

    auto view = registry.view<LODComponent, MeshComponent>();
    for (auto [entity, lod, mesh] : view.each())
    {
        if (lod.levels.empty()) continue;

        // Use entity translation if available, otherwise treat as at origin.
        glm::vec3 pos(0.f);
        if (const auto* tf = registry.try_get<TransformComponent>(entity))
            pos = tf->position;

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
