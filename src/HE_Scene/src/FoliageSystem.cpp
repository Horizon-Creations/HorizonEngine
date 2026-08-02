#include "HorizonScene/FoliageSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include <Diagnostics/Log.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdint>

namespace
{
    // Wang hash for deterministic per-instance pseudo-random values
    uint32_t wangHash(uint32_t n)
    {
        n = (n ^ 61u) ^ (n >> 16u);
        n += n << 3u;
        n ^= n >> 4u;
        n *= 0x27D4EB2Du;
        n ^= n >> 15u;
        return n;
    }

    float randFloat(uint32_t& state)
    {
        state = wangHash(state);
        return static_cast<float>(state & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }
}

void FoliageSystem::update(HorizonWorld& world)
{
    auto& registry = world.registry();

    auto view = registry.view<FoliageComponent, TerrainComponent>();
    for (auto [entity, foliage, terrain] : view.each())
    {
        if (!foliage.dirty) continue;
        foliage.dirty = false;
        foliage.cachedInstances.clear();

        if (foliage.meshAssetId == HE::UUID{})
        {
            HE_LOG_WARN(Foliage, "Entity %u: foliage layer has no mesh assigned — "
                                 "nothing will be scattered", static_cast<uint32_t>(entity));
            continue;
        }

        // Terrain world-space origin from TransformComponent if present
        glm::vec3 origin(0.f);
        if (const auto* tf = registry.try_get<TransformComponent>(entity))
            origin = tf->position;

        const float sizeX  = terrain.sizeX;
        const float sizeZ  = terrain.sizeZ;
        const float area   = sizeX * sizeZ;
        const int   count  = static_cast<int>(area * foliage.density);
        if (count <= 0)
        {
            HE_LOG_WARN(Foliage, "Entity %u: foliage density %.4f over a %.0fx%.0f terrain "
                                 "yields 0 instances", static_cast<uint32_t>(entity),
                        foliage.density, sizeX, sizeZ);
            continue;
        }
        // Scattering is O(count) and rebuilt whenever the layer is dirtied; a
        // density typo (0.5/m² over a 1 km terrain) is otherwise felt only as a
        // mysterious multi-second editor freeze.
        if (count > 200000)
            HE_LOG_WARN(Foliage, "Entity %u: scattering %d foliage instances "
                                 "(density %.4f over %.0fx%.0f) — this is very expensive",
                        static_cast<uint32_t>(entity), count, foliage.density, sizeX, sizeZ);

        foliage.cachedInstances.reserve(static_cast<size_t>(count));

        const float halfX = sizeX * 0.5f;
        const float halfZ = sizeZ * 0.5f;

        uint32_t rngState = static_cast<uint32_t>(foliage.seed) ^ 0xABCD1234u;
        for (int i = 0; i < count; ++i)
        {
            const float lx = randFloat(rngState) * sizeX - halfX;
            const float lz = randFloat(rngState) * sizeZ - halfZ;
            const float ly = terrainHeightAt(terrain, lx, lz);
            const float rotY   = randFloat(rngState) * 6.2831853f; // [0, 2π]
            const float scale  = foliage.minScale
                               + randFloat(rngState) * (foliage.maxScale - foliage.minScale);

            const glm::vec3 worldPos = origin + glm::vec3(lx, ly, lz);
            glm::mat4 m = glm::translate(glm::mat4(1.f), worldPos);
            m = glm::rotate(m, rotY, glm::vec3(0.f, 1.f, 0.f));
            m = glm::scale(m, glm::vec3(scale));
            foliage.cachedInstances.push_back(m);
        }

        HE_LOG_DEBUG(Foliage, "Entity %u: scattered %d foliage instance(s) over %.0fx%.0f",
                     static_cast<uint32_t>(entity), count, sizeX, sizeZ);
    }
}
