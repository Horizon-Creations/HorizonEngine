#include "HorizonScene/FoliageSystem.h"
#include "HorizonScene/FoliagePaint.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include <Diagnostics/Log.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
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

    // ── FoliageComponent::revision ──────────────────────────────────────────
    // A fingerprint of every setting the extraction reads that is NOT a scatter
    // input (those set `dirty`, which re-scatters and bumps the revision itself).
    uint64_t hashBytes(uint64_t h, const void* data, size_t n)
    {
        const auto* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
        return h;
    }
    template <class T> uint64_t mix(uint64_t h, const T& v) { return hashBytes(h, &v, sizeof(T)); }

    uint64_t settingsKeyOf(const FoliageComponent& f)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        h = mix(h, f.visible);
        h = mix(h, f.meshAssetId.hi);     h = mix(h, f.meshAssetId.lo);
        h = mix(h, f.materialAssetId.hi); h = mix(h, f.materialAssetId.lo);
        h = mix(h, f.minScale);           h = mix(h, f.maxScale);
        h = mix(h, f.drawDistance);
        h = mix(h, f.bucketSize);
        h = mix(h, f.castsShadow);        h = mix(h, f.contributesAO);
        h = mix(h, f.shadowDistance);
        return h;
    }

    // The bucket edge actually used: a layer asking for 0 (or less) gets the default,
    // and a terrain so large that the grid would run past kMaxBuckets gets a coarser
    // grid (a bucket is a culling unit, not a promise of its exact size).
    constexpr int64_t kMaxBuckets = 4'000'000;

    float usableBucketSize(float asked)
    {
        return asked >= 1.0f ? asked : 32.0f;
    }

    // ── Bucket sort ─────────────────────────────────────────────────────────
    // `local` is the scatter in generation order, terrain-local. Counting sort by
    // grid cell, stable, so inside a bucket the scatter's own order survives. O(N)
    // time, one transient array of cell ids; the result is the only copy kept.
    std::shared_ptr<const FoliageStore> sortIntoBuckets(const std::vector<glm::mat4>& local,
                                                        float sizeX, float sizeZ, float bucketSize)
    {
        auto store = std::make_shared<FoliageStore>();
        float edge = usableBucketSize(bucketSize);
        int nx = 1, nz = 1;
        for (;;)
        {
            nx = std::max(1, static_cast<int>(std::ceil(sizeX / edge)));
            nz = std::max(1, static_cast<int>(std::ceil(sizeZ / edge)));
            if (static_cast<int64_t>(nx) * nz <= kMaxBuckets) break;
            edge *= 2.0f;
        }
        store->bucketSize = bucketSize;   // what was ASKED: that is what FoliageSystem compares against
        store->edge       = edge;
        store->gridX      = nx;
        store->gridZ      = nz;

        const size_t n          = local.size();
        const size_t cellCount  = static_cast<size_t>(nx) * static_cast<size_t>(nz);
        const float  halfX      = sizeX * 0.5f;
        const float  halfZ      = sizeZ * 0.5f;
        const float  inv        = 1.0f / edge;

        std::vector<uint32_t> cellOf(n);
        std::vector<uint32_t> start(cellCount + 1, 0u);
        for (size_t i = 0; i < n; ++i)
        {
            const int ix = std::clamp(static_cast<int>((local[i][3].x + halfX) * inv), 0, nx - 1);
            const int iz = std::clamp(static_cast<int>((local[i][3].z + halfZ) * inv), 0, nz - 1);
            cellOf[i] = static_cast<uint32_t>(iz) * static_cast<uint32_t>(nx) + static_cast<uint32_t>(ix);
            ++start[cellOf[i] + 1];
        }
        for (size_t c = 0; c < cellCount; ++c) start[c + 1] += start[c];

        store->local.resize(n);
        std::vector<uint32_t> cursor(start.begin(), start.end() - 1);
        for (size_t i = 0; i < n; ++i)
            store->local[cursor[cellOf[i]]++] = local[i];

        for (size_t c = 0; c < cellCount; ++c)
        {
            if (start[c + 1] == start[c]) continue;
            FoliageBucket b;
            b.first = start[c];
            b.count = start[c + 1] - start[c];
            for (uint32_t k = b.first; k < b.first + b.count; ++k)
                b.localBounds.expand(glm::vec3(store->local[k][3]));
            store->buckets.push_back(b);
        }
        return store;
    }
}

void FoliageSystem::update(HorizonWorld& world)
{
    auto& registry = world.registry();

    auto view = registry.view<FoliageComponent, TerrainComponent>();
    for (auto [entity, foliage, terrain] : view.each())
    {
        // A setting the extraction reads changed (mesh, distance, flags …): not a
        // re-scatter, but whoever keeps a RenderWorld across frames must hear of it.
        const uint64_t key = settingsKeyOf(foliage);
        if (key != foliage.settingsKey) { foliage.settingsKey = key; ++foliage.revision; }

        // The store is sorted into a grid of the layer's bucket size: a different size
        // asks for a different grid, which only a re-scatter can produce.
        if (foliage.store && foliage.store->bucketSize != foliage.bucketSize)
            foliage.dirty = true;

        if (!foliage.dirty) continue;
        foliage.dirty = false;
        foliage.cachedInstances.clear();
        foliage.store.reset();
        ++foliage.revision;

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
        // The same instances, terrain-local, in generation order: the input of the
        // bucket sort below. Transient — only the sorted copy lives on in the store.
        std::vector<glm::mat4> local;
        local.reserve(static_cast<size_t>(count));

        const float halfX = sizeX * 0.5f;
        const float halfZ = sizeZ * 0.5f;

        // A painted mask thins the scatter by rejection: every candidate is
        // drawn exactly as for a uniform layer, then kept with the probability
        // the mask gives at that spot — 0 where the brush erased (an exclusion
        // area never gets an instance), 1 where nothing was painted. The
        // acceptance roll comes from its own per-candidate hash, NOT from the
        // shared stream, so the survivors keep exactly the position, rotation
        // and scale they have in the uniform layout: painting removes and
        // thins, it never reshuffles what stays.
        const bool     masked   = !foliage.densityMask.empty();
        const uint32_t keepSalt = static_cast<uint32_t>(foliage.seed) * 0x9E3779B9u;
        int placed = 0;

        uint32_t rngState = static_cast<uint32_t>(foliage.seed) ^ 0xABCD1234u;
        for (int i = 0; i < count; ++i)
        {
            const float lx = randFloat(rngState) * sizeX - halfX;
            const float lz = randFloat(rngState) * sizeZ - halfZ;
            const float rotY   = randFloat(rngState) * 6.2831853f; // [0, 2π]
            const float scale  = foliage.minScale
                               + randFloat(rngState) * (foliage.maxScale - foliage.minScale);
            if (masked)
            {
                const float keep = FoliagePaint::sample(foliage, terrain, lx, lz);
                uint32_t roll = wangHash(static_cast<uint32_t>(i) ^ keepSalt);
                const float u = randFloat(roll);   // [0, 1)
                // Strict at both ends: a mask of exactly 0 rejects every
                // candidate (u can be 0), a mask of 1 keeps every one (u < 1).
                if (keep <= 0.0f || u >= keep) continue;
            }
            const float ly = terrainHeightAt(terrain, lx, lz);

            // The terrain-local pose, built exactly as the world one always was:
            // translate, rotate about Y, scale. Rotating and scaling never touch the
            // translation column, so the world matrix is this one with its position
            // moved by `origin` — bit for bit what `translate(origin + local)` gave.
            glm::mat4 m = glm::translate(glm::mat4(1.f), glm::vec3(lx, ly, lz));
            m = glm::rotate(m, rotY, glm::vec3(0.f, 1.f, 0.f));
            m = glm::scale(m, glm::vec3(scale));
            local.push_back(m);

            const glm::vec3 worldPos = origin + glm::vec3(lx, ly, lz);
            m[3] = glm::vec4(worldPos, 1.0f);
            foliage.cachedInstances.push_back(m);
            ++placed;
        }

        foliage.store = sortIntoBuckets(local, sizeX, sizeZ, foliage.bucketSize);

        HE_LOG_DEBUG(Foliage, "Entity %u: scattered %d of %d foliage instance(s) over %.0fx%.0f%s "
                              "into %zu bucket(s)",
                     static_cast<uint32_t>(entity), placed, count, sizeX, sizeZ,
                     masked ? " (density mask)" : "", foliage.store->buckets.size());
    }
}
