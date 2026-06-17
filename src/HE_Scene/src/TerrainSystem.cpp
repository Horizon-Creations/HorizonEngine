#include "HorizonScene/TerrainSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include <ContentManager/ContentManager.h>
#include <entt/entt.hpp>

namespace TerrainSystem
{
    void updateTerrains(HorizonWorld& world, ContentManager& cm)
    {
        auto& registry = world.registry();
        auto view = registry.view<TerrainComponent>();
        for (auto entity : view)
        {
            auto& tc = registry.get<TerrainComponent>(entity);
            if (!tc.dirty) continue;
            tc.dirty = false;

            StaticMeshAsset newMesh = generateTerrainMesh(tc);

            // Determine current registered mesh UUID (if any)
            HE::UUID existingId{};
            if (const auto* mc = registry.try_get<MeshComponent>(entity))
                existingId = mc->meshAssetId;

            HE::UUID meshId;
            if (cm.getStaticMesh(existingId) != nullptr)
            {
                // Keep the same UUID — swap the mesh data in-place
                cm.replaceStaticMesh(existingId, std::move(newMesh));
                meshId = existingId;
            }
            else
            {
                // First generation (or UUID orphaned after undo/redo)
                meshId = cm.registerStaticMesh(std::move(newMesh));
            }

            MeshComponent mc;
            mc.meshAssetId = meshId;
            registry.emplace_or_replace<MeshComponent>(entity, mc);
        }
    }
}
