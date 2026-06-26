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

        // Use WORLD position (worldMatrix translation), not local — terrain chunks
        // are children, so their local position is a per-chunk offset; only the world
        // position gives the right per-chunk camera distance. worldMatrix is computed
        // by the scene graph / extractor (1-frame lag at most, invisible for LOD).
        glm::vec3 pos(0.f);
        if (const auto* tf = registry.try_get<TransformComponent>(entity))
            pos = glm::vec3(tf->worldMatrix[3]);

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
