#include "HorizonScene/TerrainSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/TerrainChunkComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Renderer/IRenderer.h>
#include <entt/entt.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    uint32_t nextPow2(uint32_t v)
    {
        uint32_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    struct ChunkGrid
    {
        uint32_t chunksPerSide = 1;  // C  (C×C chunks)
        uint32_t lod0Cells     = 64; // cells per chunk side at LOD0 (power of two)
        uint32_t numLODs       = 1;
    };

    // Derive a clean power-of-two chunk grid from any source resolution. The chunk
    // grid is decoupled from the source heightfield (chunks SAMPLE the field), so
    // odd resolutions like 512 (=511 cells) snap to 512 cells = 8×64, strides clean.
    ChunkGrid computeGrid(uint32_t res)
    {
        const uint32_t cells   = (res >= 2) ? (res - 1) : 1;
        const uint32_t snapped = std::max(64u, nextPow2(cells));   // total cells per side (pow2)
        uint32_t cps = std::clamp(snapped / 64u, 1u, 32u);
        uint32_t cpsPow2 = 1; while (cpsPow2 * 2u <= cps) cpsPow2 <<= 1; // floor to pow2 divisor
        cps = std::max(1u, cpsPow2);
        const uint32_t chunkCells = snapped / cps;                 // power of two (e.g. 64)
        uint32_t L = 1, c = chunkCells;
        while (c >= 8u && L < 4u) { c >>= 1; ++L; }                // 64→32→16→8 ⇒ 4 LODs
        return { cps, chunkCells, L };
    }

    bool rectsOverlap(float aMinX, float aMinZ, float aMaxX, float aMaxZ,
                      float bMinX, float bMinZ, float bMaxX, float bMaxZ)
    {
        return aMinX <= bMaxX && aMaxX >= bMinX && aMinZ <= bMaxZ && aMaxZ >= bMinZ;
    }
}

namespace TerrainSystem
{
    // Build (or replace, reusing UUIDs) the L LOD meshes for one chunk and wire up
    // its LODComponent + MeshComponent. Returns nothing; mutates the chunk entity.
    static void buildChunk(entt::registry& reg, ContentManager& cm, IRenderer* renderer,
                           entt::entity chunkEnt, const std::vector<float>& field,
                           uint32_t res, const TerrainComponent& tc, const ChunkGrid& g,
                           uint32_t cx, uint32_t cz)
    {
        const float u0 = static_cast<float>(cx)     / static_cast<float>(g.chunksPerSide);
        const float u1 = static_cast<float>(cx + 1) / static_cast<float>(g.chunksPerSide);
        const float v0 = static_cast<float>(cz)     / static_cast<float>(g.chunksPerSide);
        const float v1 = static_cast<float>(cz + 1) / static_cast<float>(g.chunksPerSide);

        auto* lod = reg.try_get<LODComponent>(chunkEnt);
        const bool haveLevels = lod && lod->levels.size() == g.numLODs;

        LODComponent newLod;
        const float chunkWorld = tc.sizeX / static_cast<float>(g.chunksPerSide);
        for (uint32_t k = 0; k < g.numLODs; ++k)
        {
            const uint32_t verts = (g.lod0Cells >> k) + 1u;   // 65, 33, 17, 9, …
            StaticMeshAsset m = generateTerrainChunkMesh(field, res, tc.sizeX, tc.sizeZ,
                                                         u0, v0, u1, v1, verts);
            HE::UUID id;
            if (haveLevels && cm.getStaticMesh(lod->levels[k].meshId) != nullptr)
            {
                id = lod->levels[k].meshId;            // reuse UUID → cheap re-upload
                cm.replaceStaticMesh(id, std::move(m));
                if (renderer) renderer->InvalidateMesh(id);
            }
            else
            {
                id = cm.registerStaticMesh(std::move(m));
            }
            // Generous distance LOD: near terrain stays full-resolution and only
            // genuinely distant chunks decimate (geometric growth). Tunable per
            // terrain via lodDistanceScale. base = 6 chunk-widths at LOD0 → for a
            // typical terrain the whole near/mid field is full detail.
            const float scale = std::max(0.1f, tc.lodDistanceScale);
            const float base  = chunkWorld * 6.0f * scale;
            const float maxDist = (k + 1 == g.numLODs)
                ? std::numeric_limits<float>::max()
                : base * static_cast<float>(1u << k);   // base, 2·base, 4·base
            newLod.levels.push_back({ id, maxDist });
        }
        reg.emplace_or_replace<LODComponent>(chunkEnt, newLod);

        // Drive the mesh from LOD0 initially; LODSystem swaps it per-frame by distance.
        // Terrain casts shadows again (self-shadowing) — the shadow pass renders each
        // chunk at its current (distance-LOD) mesh, so near chunks self-shadow in
        // detail while distant chunks are cheap/coarse automatically. (Earlier this
        // was disabled to dodge a Shadow cost that turned out to be a profiler
        // single-frame artifact — median shadow time is ~1ms, which is fine.)
        MeshComponent mc;
        mc.meshAssetId = newLod.levels.empty() ? HE::UUID{} : newLod.levels.front().meshId;
        mc.dirty       = true;
        reg.emplace_or_replace<MeshComponent>(chunkEnt, mc);
    }

    void updateTerrains(HorizonWorld& world, ContentManager& cm, IRenderer* renderer)
    {
        auto& reg = world.registry();

        // Collect terrain entities first — we create/destroy entities below, which
        // must not happen while iterating the view.
        std::vector<entt::entity> terrains;
        for (auto e : reg.view<TerrainComponent>()) terrains.push_back(e);

        for (entt::entity te : terrains)
        {
            auto& tc = reg.get<TerrainComponent>(te);
            if (!tc.dirty && !tc.regionDirty) continue;

            // Snap to a 2ⁿ+1 resolution ONCE so chunk LOD0 vertices land EXACTLY on
            // source grid points (otherwise LOD0 bilinearly resamples the master and
            // smears sculpted detail — the "lost detail up close" regression). For a
            // sculpted terrain this resamples sculptHeights one time (near-lossless,
            // e.g. 512→513); for noise it just bumps the resolution. Idempotent.
            {
                const uint32_t r0 = std::clamp(tc.resolution, 2u, 1024u);
                uint32_t cells = r0 - 1, p = 1; while (p < cells) p <<= 1;
                const uint32_t snappedRes = p + 1;
                if (snappedRes != r0)
                {
                    if (tc.sculptHeights.size() == static_cast<size_t>(r0) * r0)
                        tc.sculptHeights = resampleHeightField(tc.sculptHeights, r0, snappedRes);
                    tc.resolution = snappedRes;
                    tc.dirty = true; // full rebuild at the new resolution
                }
            }

            const uint32_t res = std::clamp(tc.resolution, 2u, 1024u);
            const ChunkGrid g  = computeGrid(res);
            const std::vector<float> field = computeTerrainHeightField(tc);

            const bool gridChanged = (tc.builtRes != res || tc.builtChunksPerSide != g.chunksPerSide);
            const bool rebuildAll  = tc.dirty || gridChanged;

            // Ensure the terrain entity has the terrain material but NOT its own
            // renderable mesh — the chunks render now (avoids drawing it twice).
            if (reg.all_of<MeshComponent>(te)) reg.remove<MeshComponent>(te);
            auto* matComp = reg.try_get<MaterialComponent>(te);
            if (!matComp || matComp->materialAssetId == HE::kDefaultMaterialId)
            {
                MaterialComponent mat; mat.materialAssetId = HE::kDefaultTerrainMaterialId;
                reg.emplace_or_replace<MaterialComponent>(te, mat);
            }

            // Index existing chunk entities of this terrain by grid coord.
            std::vector<entt::entity> toDestroy;
            std::vector<std::vector<entt::entity>> byCoord(
                static_cast<size_t>(g.chunksPerSide) * g.chunksPerSide);
            for (auto [ce, cc] : reg.view<TerrainChunkComponent>().each())
            {
                if (cc.terrain != te) continue;
                if (gridChanged || cc.cx >= g.chunksPerSide || cc.cz >= g.chunksPerSide)
                { toDestroy.push_back(ce); continue; }
                byCoord[static_cast<size_t>(cc.cz) * g.chunksPerSide + cc.cx].push_back(ce);
            }
            for (entt::entity d : toDestroy) world.destroyEntity(d);

            const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
            const float chunkX = tc.sizeX / static_cast<float>(g.chunksPerSide);
            const float chunkZ = tc.sizeZ / static_cast<float>(g.chunksPerSide);

            for (uint32_t cz = 0; cz < g.chunksPerSide; ++cz)
                for (uint32_t cx = 0; cx < g.chunksPerSide; ++cx)
                {
                    // Region-skip: on a sculpt region update, only touch chunks whose
                    // world-XZ rect overlaps the brush-dirtied rect.
                    const float minX = -halfX + static_cast<float>(cx) * chunkX;
                    const float minZ = -halfZ + static_cast<float>(cz) * chunkZ;
                    const float maxX = minX + chunkX, maxZ = minZ + chunkZ;
                    if (!rebuildAll && tc.regionDirty &&
                        !rectsOverlap(minX, minZ, maxX, maxZ,
                                      tc.dirtyMinX, tc.dirtyMinZ, tc.dirtyMaxX, tc.dirtyMaxZ))
                        continue;

                    const size_t key = static_cast<size_t>(cz) * g.chunksPerSide + cx;
                    entt::entity chunkEnt = byCoord[key].empty() ? entt::null : byCoord[key].front();

                    if (chunkEnt == entt::null)
                    {
                        chunkEnt = world.createEntity("TerrainChunk");
                        world.reparentEntity(chunkEnt, te);
                        TerrainChunkComponent tcc; tcc.terrain = te; tcc.cx = cx; tcc.cz = cz;
                        reg.emplace<TerrainChunkComponent>(chunkEnt, tcc);
                        // Position at the chunk centre (terrain-local) so per-chunk
                        // distance-LOD + frustum culling work; mesh verts are centred.
                        TransformComponent tf;
                        tf.position = glm::vec3(minX + chunkX * 0.5f, 0.0f, minZ + chunkZ * 0.5f);
                        tf.dirty    = true;
                        reg.emplace_or_replace<TransformComponent>(chunkEnt, tf);
                        MaterialComponent mat; mat.materialAssetId = HE::kDefaultTerrainMaterialId;
                        reg.emplace_or_replace<MaterialComponent>(chunkEnt, mat);
                    }

                    buildChunk(reg, cm, renderer, chunkEnt, field, res, tc, g, cx, cz);
                }

            tc.builtRes           = res;
            tc.builtChunksPerSide = g.chunksPerSide;
            tc.dirty              = false;
            tc.regionDirty        = false;
        }
    }
}
