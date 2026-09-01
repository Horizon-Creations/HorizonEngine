#include "HorizonScene/TerrainSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/TerrainChunkComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include "HorizonScene/PhysicsWorld.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Renderer/IRenderer.h>
#include <entt/entt.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
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

    // Unpainted terrain: the shader binds the 1×1 (1,0,0,0) default weightmap, so
    // the flat approximation has to agree — layer 0 alone, not an even split.
    void setUnpaintedLayerAverage(TerrainComponent& tc)
    {
        tc.avgLayerWeights[0] = 1.0f;
        tc.avgLayerWeights[1] = tc.avgLayerWeights[2] = tc.avgLayerWeights[3] = 0.0f;
    }

    // Mean painted weight per layer over the whole terrain, normalised to Σ = 1 —
    // one scan per PAINT (not per frame), which is what makes it affordable for
    // the flat-shaded consumers (GI hits) to reproduce the terrain's colour.
    // Normalising per texel first matches the shader, which divides each texel's
    // blend by that texel's weight sum, so half-painted texels don't count less.
    void computeAverageLayerWeights(TerrainComponent& tc)
    {
        double acc[4] = { 0.0, 0.0, 0.0, 0.0 };
        size_t texels = 0;
        for (size_t i = 0; i + 3 < tc.layerWeights.size(); i += 4)
        {
            const double w[4] = { tc.layerWeights[i + 0] / 255.0, tc.layerWeights[i + 1] / 255.0,
                                  tc.layerWeights[i + 2] / 255.0, tc.layerWeights[i + 3] / 255.0 };
            const double s = w[0] + w[1] + w[2] + w[3];
            if (s <= 1e-4) continue;                       // blank texel: the shader's
            for (int k = 0; k < 4; ++k) acc[k] += w[k] / s; // 1e-4 floor, same skip
            ++texels;
        }
        if (texels == 0) { setUnpaintedLayerAverage(tc); return; }
        for (int k = 0; k < 4; ++k)
            tc.avgLayerWeights[k] = static_cast<float>(acc[k] / static_cast<double>(texels));
    }

    // ── Terrains whose collider still has to catch up ────────────────────────
    // Entity id → "it was edited again this tick". updateTerrains marks a
    // terrain here every time it regenerates its meshes and rebuilds the
    // physics height field on the first tick that leaves it alone. Why not
    // straight away: see the flush at the end of updateTerrains.
    //
    // Keyed by WORLD as well, because entity ids are per-registry — terrain #7
    // of a second world must not be flushed against the first one's. A world's
    // entry is erased as soon as nothing of it is pending. Only a play session
    // that STOPS mid-stroke leaves one behind (the flush needs a physics world
    // to flush into): the next session flushes it on its first tick, where the
    // rebuild is redundant but harmless, since initialize() has built that
    // collider already and addEntity replaces rather than duplicates.
    //
    // File-static rather than a TerrainComponent field: this is bookkeeping
    // ABOUT the component, and the component is serialised. Main-thread only,
    // like the rest of the systems tick.
    std::unordered_map<const HorizonWorld*, std::unordered_map<uint32_t, bool>>
        g_pendingTerrainColliders;
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
                                                         u0, v0, u1, v1, verts, tc.uvTiling);
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

    // The physics-aware tick. `physics` is nullable and null is the normal state
    // outside play — the height field only has to exist while the simulation
    // runs, so an editor that is not playing simply passes nothing.
    //
    // A landscape's collider is a single static height field on the TERRAIN
    // entity (PhysicsWorld::buildTerrainBodyFor), NOT one per chunk: it is built
    // from the same computeTerrainHeightField() array the chunk meshes are
    // sampled out of, at the full snapped resolution, so it never follows the
    // distance-LOD the way a collider made from the displayed chunk meshes
    // would. What it does not get on its own is a SECOND look after a sculpt
    // stroke, and that is what this overload adds.
    void updateTerrains(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                        PhysicsWorld* physics)
    {
        auto& reg = world.registry();

        // Collect terrain entities first — we create/destroy entities below, which
        // must not happen while iterating the view.
        std::vector<entt::entity> terrains;
        for (auto e : reg.view<TerrainComponent>()) terrains.push_back(e);

        // ── Layer weightmap → GPU texture ────────────────────────────────────
        // The painted per-texel layer weights (RGBA8, one channel per layer) become
        // a texture the chunks' draw calls carry, so a material's Landscape Layer
        // Blend node can sample them. Registered once and REPLACED in place on
        // later paints, so the UUID stays stable for anything already holding it.
        for (entt::entity te : terrains)
        {
            auto& tc = reg.get<TerrainComponent>(te);
            if (tc.layerWeights.empty())
            {
                tc.weightmapTextureId = HE::UUID{};   // unpainted → shader uses layer 0
                tc.weightsDirty = false;
                setUnpaintedLayerAverage(tc);
                continue;
            }
            const uint32_t wr = std::max(1u, tc.weightRes);
            if (tc.layerWeights.size() != static_cast<size_t>(wr) * wr * 4)
            {
                tc.layerWeights.clear();              // defensive: never upload a short blob
                tc.weightmapTextureId = HE::UUID{};
                tc.weightsDirty = false;
                setUnpaintedLayerAverage(tc);
                continue;
            }
            const bool have = tc.weightmapTextureId != HE::UUID{}
                           && cm.getTexture(tc.weightmapTextureId) != nullptr;
            if (have && !tc.weightsDirty) continue;
            computeAverageLayerWeights(tc);           // paint changed → refresh the mean

            TextureAsset tex;
            tex.type     = HE::AssetType::Texture;
            tex.name     = "terrain_weightmap";
            tex.path     = "mem://terrain_weightmap";
            tex.width    = static_cast<int>(wr);
            tex.height   = static_cast<int>(wr);
            tex.channels = 4;
            tex.data     = tc.layerWeights;
            if (have)
            {
                cm.replaceTexture(tc.weightmapTextureId, std::move(tex));
                if (renderer) renderer->InvalidateTexture(tc.weightmapTextureId);
            }
            else
                tc.weightmapTextureId = cm.registerTexture(std::move(tex));
            tc.weightsDirty = false;
        }

        // ── Chunk material follows the Landscape entity's material ───────────
        // The chunks are what actually render; the Landscape entity itself has no
        // mesh. Their MaterialComponent used to be pinned to the built-in terrain
        // material at CREATION time, so assigning a material to the Landscape — or
        // recolouring the one it has — never reached the screen. Re-synced every
        // tick (NOT gated on tc.dirty: a material swap doesn't dirty the heightfield)
        // so the parent's material and its per-entity param overrides propagate
        // without rebuilding the terrain.
        for (entt::entity te : terrains)
        {
            if (auto* have = reg.try_get<MaterialComponent>(te);
                !have || have->materialAssetId == HE::UUID{} ||
                have->materialAssetId == HE::kDefaultMaterialId)
            {
                MaterialComponent mat; mat.materialAssetId = HE::kDefaultTerrainMaterialId;
                reg.emplace_or_replace<MaterialComponent>(te, mat);
            }
            // Copied out by value: emplacing below can reallocate the pool, which
            // would dangle a pointer into the parent's component.
            const MaterialComponent src = reg.get<MaterialComponent>(te);
            auto same = [&src](const MaterialComponent& d)
            {
                if (d.materialAssetId != src.materialAssetId) return false;
                if (d.paramOverrides.size() != src.paramOverrides.size()) return false;
                for (size_t i = 0; i < d.paramOverrides.size(); ++i)
                {
                    const auto& x = d.paramOverrides[i]; const auto& y = src.paramOverrides[i];
                    if (x.name != y.name) return false;
                    for (int k = 0; k < 4; ++k) if (x.value[k] != y.value[k]) return false;
                }
                return true;
            };
            std::vector<entt::entity> needMat; // chunks with no MaterialComponent yet
            for (auto [ce, cc] : reg.view<TerrainChunkComponent>().each())
            {
                if (cc.terrain != te) continue;
                auto* dst = reg.try_get<MaterialComponent>(ce);
                if (!dst) { needMat.push_back(ce); continue; }
                if (same(*dst)) continue;
                dst->materialAssetId = src.materialAssetId;
                dst->paramOverrides  = src.paramOverrides;
                dst->dirty           = true;
            }
            // Deferred: emplace adds storage, which must not happen mid-view.
            for (entt::entity ce : needMat)
                reg.emplace_or_replace<MaterialComponent>(ce, src);
        }

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
                        // Seed from the Landscape's own material (the sync pass at
                        // the top of the tick keeps it aligned from here on).
                        if (const auto* parentMat = reg.try_get<MaterialComponent>(te))
                            reg.emplace_or_replace<MaterialComponent>(chunkEnt, *parentMat);
                        else
                        {
                            MaterialComponent mat; mat.materialAssetId = HE::kDefaultTerrainMaterialId;
                            reg.emplace_or_replace<MaterialComponent>(chunkEnt, mat);
                        }
                    }

                    buildChunk(reg, cm, renderer, chunkEnt, field, res, tc, g, cx, cz);
                }

            tc.builtRes           = res;
            tc.builtChunksPerSide = g.chunksPerSide;
            tc.dirty              = false;
            tc.regionDirty        = false;

            // The ground the player stands on has to follow the ground they see —
            // but NOT from here. Reached only from inside the dirty/regionDirty
            // gate, which sounds like "once per edit" and is not: a brush drag
            // sets regionDirty on EVERY frame it is held, and so does a drag on
            // a noise slider with tc.dirty. Queue instead; the flush below
            // rebuilds once the terrain is left alone.
            if (physics)
                g_pendingTerrainColliders[&world][static_cast<uint32_t>(te)] = true;
        }

        // ── The collider catches up when the edit stops ──────────────────────
        // A height-field rebuild is not in the same price class as the mesh
        // regeneration above it. addEntity → buildHeightFieldShape evaluates the
        // WHOLE field a second time (the chunk meshes only resample the copy
        // this tick already made) and then hands Jolt every sample to quantise
        // and to compute active edges for — about a million of them on a
        // 1025² landscape, whereas a brush frame touches a couple of chunks.
        // Paying that per brush FRAME is what makes a stroke stutter, and the
        // stutter is worst on exactly the landscapes people sculpt.
        //
        // So the trade is: the collision surface is stale while the brush is
        // down, and correct one tick after it lifts. Nobody can walk onto the
        // metre of hill they are pushing up inside the same stroke — the camera
        // is on the brush — and every other consumer (spawning, raycasts, an AI
        // path) is asking about ground the stroke is not touching. One rebuild
        // per stroke instead of sixty per second buys that outright.
        //
        // addEntity is idempotent — it tears the old height field down first —
        // and it is deliberately NOT guarded by hasPhysics(), so a terrain whose
        // body failed to build at scene start gets another attempt here.
        //
        // The heights are re-read from the TerrainComponent rather than patched
        // region-wise on purpose: Jolt's HeightFieldShape::SetHeights silently
        // clamps to the min/max the shape was CREATED with, which would let a
        // sculpted mountain grow on screen while its collision stayed flat.
        //
        // Nothing to do for the chunk entities destroyed above: the collider
        // lives on the terrain entity, never on a chunk. A destroyed TERRAIN
        // entity is reaped by PhysicsWorld::step(), and the re-check below keeps
        // a terrain that died mid-stroke from being rebuilt on its way out.
        //
        // find, not operator[]: the overwhelmingly common tick has nothing
        // pending, and [] would default-construct and then erase an inner map on
        // every one of them — a per-frame allocation inside the fix that exists
        // to remove a per-frame rebuild.
        const auto w = physics ? g_pendingTerrainColliders.find(&world)
                               : g_pendingTerrainColliders.end();
        if (w != g_pendingTerrainColliders.end())
        {
            auto& pending = w->second;
            for (auto it = pending.begin(); it != pending.end(); )
            {
                if (it->second)          // edited again this tick — still under the brush
                {
                    it->second = false;
                    ++it;
                    continue;
                }
                const entt::entity te = static_cast<entt::entity>(it->first);
                if (reg.valid(te) && reg.all_of<TerrainComponent>(te))
                    physics->addEntity(world, it->first);
                it = pending.erase(it);
            }
            if (pending.empty())
                g_pendingTerrainColliders.erase(w);
        }
    }

    void updateTerrains(HorizonWorld& world, ContentManager& cm, IRenderer* renderer)
    {
        updateTerrains(world, cm, renderer, nullptr);
    }
}
