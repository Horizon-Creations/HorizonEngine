#include "HorizonScene/WaterSurface.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/WaterMesh.h"
#include "HorizonScene/WaterField.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/WaterSurfaceComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <Diagnostics/Log.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Renderer/IRenderer.h>
#include <entt/entt.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace HE::water;

    WaterSurface::Stats g_stats;

    // Surface entity → the mesh it owns, per world (entity ids are per registry).
    // The component carries the same UUID; this copy is what lets a mesh be given
    // back after its entity is gone, and then the component is no longer there to
    // ask. Entt recycles handles, so the key alone is not proof — the UUID must
    // match too (the RopeTrailSystem / terrain tessellation bookkeeping).
    std::unordered_map<const HorizonWorld*, std::unordered_map<uint32_t, HE::UUID>> g_owned;

    bool terrainHasArea(const TerrainComponent& tc)
    {
        return std::isfinite(tc.sizeX) && std::isfinite(tc.sizeZ) && tc.sizeX > 0.0f && tc.sizeZ > 0.0f;
    }

    bool overlap(const CellRect& a, const CellRect& b)
    {
        return a.valid() && b.valid() &&
               a.x0 <= b.x1 && a.x1 >= b.x0 && a.z0 <= b.z1 && a.z1 >= b.z0;
    }

    uint64_t mix(uint64_t h, const void* data, size_t bytes)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
    template <typename T> uint64_t mix(uint64_t h, const T& v) { return mix(h, &v, sizeof(T)); }

    // Everything that is the same for all bodies of a landscape and moves their
    // mesh without moving a cell (the shore clipping settings included).
    uint64_t paramsKeyOf(const TerrainComponent& tc, const glm::vec2& uvOrigin)
    {
        uint64_t h = 1469598103934665603ull;
        h = mix(h, tc.sizeX); h = mix(h, tc.sizeZ); h = mix(h, tc.water.res);
        h = mix(h, uvOrigin.x); h = mix(h, uvOrigin.y);
        h = mix(h, WaterSurface::kUvMetersPerTile);
        h = mix(h, tc.water.clipToGround); h = mix(h, tc.water.shoreOvershoot);
        return h;
    }

    // The mesh asset behind a surface goes with it: out of the renderer's cache,
    // out of the content manager.
    void giveBackMesh(ContentManager& cm, IRenderer* renderer, const HE::UUID& id)
    {
        if (id == HE::UUID{}) return;
        if (renderer) renderer->InvalidateMesh(id);
        if (!cm.unloadAsset(id))
            HE_LOG_DEBUG(Asset, "Water surface mesh %016llx%016llx stayed loaded"
                         " — something still holds a handle on it",
                         static_cast<unsigned long long>(id.hi),
                         static_cast<unsigned long long>(id.lo));
    }

    void removeSurface(HorizonWorld& world, ContentManager& cm, IRenderer* renderer, entt::entity e)
    {
        auto& reg = world.registry();
        HE::UUID mesh{};
        if (const auto* ws = reg.try_get<WaterSurfaceComponent>(e)) mesh = ws->meshId;
        if (auto w = g_owned.find(&world); w != g_owned.end())
            w->second.erase(static_cast<uint32_t>(e));
        world.destroyEntity(e);
        giveBackMesh(cm, renderer, mesh);
    }

    // Material first: with it not resident the renderer would draw the surface
    // with the plain fallback, which is a grey slab and looks like a bug in the
    // mesh. By UUID when the engine folder has been scanned (an editor session, a
    // pak), by path when it has not — a headless dump renders before the scan, and
    // there the UUID is unknown while the path resolves. A missing file (a unit
    // test without EngineContent, a broken install) is said once.
    void ensureWaterMaterial(ContentManager& cm)
    {
        if (cm.ensureResident(HE::kEngineWaterMaterialId)) return;
        if (cm.loadAsset(WaterSurface::kMaterialPath) == HE::kEngineWaterMaterialId) return;
        static bool said = false;
        if (said) return;
        said = true;
        HE_LOG_WARN(Asset, "Water surface: the engine water material (%s) is not available — "
                           "water will draw with the fallback material", WaterSurface::kMaterialPath);
    }

    // Per landscape.
    struct Pass
    {
        HorizonWorld&   world;
        ContentManager& cm;
        IRenderer*      renderer;

        entt::registry& reg() { return world.registry(); }

        void apply(entt::entity te, uint16_t bodyId, entt::entity existing,
                   Surface& s, uint64_t paramsKey, WaterSurface::Stats& stats)
        {
            auto& r = reg();
            HE::UUID meshId{};
            bool reuseMesh = false;
            if (existing != entt::null)
                if (const auto* ws = r.try_get<WaterSurfaceComponent>(existing);
                    ws && ws->meshId != HE::UUID{} && cm.getStaticMesh(ws->meshId) != nullptr)
                {
                    meshId = ws->meshId;
                    reuseMesh = true;
                }

            if (reuseMesh)
            {
                cm.replaceStaticMesh(meshId, std::move(s.mesh));
                if (renderer) renderer->InvalidateMesh(meshId);
            }
            else
                meshId = cm.registerStaticMesh(std::move(s.mesh));

            entt::entity e = existing;
            if (e == entt::null)
            {
                ensureWaterMaterial(cm);
                e = world.createEntity("WaterSurface");
                world.reparentEntity(e, te);
                MaterialComponent mat;
                mat.materialAssetId = HE::kEngineWaterMaterialId;
                r.emplace_or_replace<MaterialComponent>(e, mat);
                ++stats.created;
            }

            WaterSurfaceComponent ws;
            ws.terrain   = te;
            ws.body      = bodyId;
            ws.meshId    = meshId;
            ws.hash      = s.hash;
            ws.paramsKey = paramsKey;
            ws.level     = s.level;
            ws.cellX0 = s.cells.x0; ws.cellZ0 = s.cells.z0;
            ws.cellX1 = s.cells.x1; ws.cellZ1 = s.cells.z1;
            ws.triangles = s.triangles;
            r.emplace_or_replace<WaterSurfaceComponent>(e, ws);

            // The entity stands at the middle of the body's box, at the surface's
            // height; the vertices are relative to that point.
            TransformComponent tf;
            if (const auto* old = r.try_get<TransformComponent>(e)) tf = *old;
            tf.position = glm::vec3(s.center.x, s.level, s.center.y);
            tf.dirty = true;
            r.emplace_or_replace<TransformComponent>(e, tf);

            MeshComponent mc;
            mc.meshAssetId    = meshId;
            mc.castsShadow    = false;     // a translucent sheet; its shadow would be a hard grey slab
            mc.dirty          = true;
            r.emplace_or_replace<MeshComponent>(e, mc);

            g_owned[&world][static_cast<uint32_t>(e)] = meshId;
            ++stats.rebuilt;
            stats.triangles += s.triangles;
        }

        void terrain(entt::entity te, TerrainComponent& tc, std::vector<entt::entity>& surfaces,
                     WaterSurface::Stats& stats)
        {
            Field& f = tc.water;

            // One surface per body. A second one for the same body (a copied
            // entity) would draw the lake twice and fight over the mesh: keep
            // the first.
            std::unordered_map<uint16_t, entt::entity> byBody;
            for (entt::entity e : surfaces)
            {
                const uint16_t id = reg().get<WaterSurfaceComponent>(e).body;
                if (byBody.count(id)) { removeSurface(world, cm, renderer, e); ++stats.removed; continue; }
                byBody[id] = e;
            }

            const bool usable = f.allocated() && terrainHasArea(tc) && !f.bodies.empty();
            if (!usable)
            {
                for (auto& kv : byBody) { removeSurface(world, cm, renderer, kv.second); ++stats.removed; }
                f.dirty = false;
                return;
            }
            ++stats.terrains;

            const glm::vec3 wp = HE::worldPositionOf(world, te);
            SurfaceOptions opt;
            opt.uvOrigin        = glm::vec2(wp.x, wp.z);
            opt.uvMetersPerTile = WaterSurface::kUvMetersPerTile;
            opt.clipToGround    = f.clipToGround;
            opt.shoreOvershoot  = f.shoreOvershoot;
            const uint64_t paramsKey = paramsKeyOf(tc, opt.uvOrigin);

            // Steady state: nothing was edited, every surface still belongs to a
            // body, was built with today's parameters and stands at its body's
            // level. That is nearly every tick, and it costs no more than this.
            bool steady = !f.dirty;
            if (steady)
                for (const auto& kv : byBody)
                {
                    const auto& ws = reg().get<WaterSurfaceComponent>(kv.second);
                    const Body* b = f.findBody(kv.first);
                    if (!b || ws.paramsKey != paramsKey || ws.level != b->level) { steady = false; break; }
                }
            if (steady) return;

            const std::vector<CellRect> extents = allBodyCells(tc);
            // The edited rectangle in cells, one cell wider all round: a contour
            // reads the cells around a corner, so a change in the cell next door
            // moves it.
            CellRect dirtyCells;
            if (f.dirty)
            {
                const glm::vec2 cs = cellSize(tc);
                dirtyCells = cellRect(tc, f.dirtyMinX - cs.x, f.dirtyMinZ - cs.y,
                                          f.dirtyMaxX + cs.x, f.dirtyMaxZ + cs.y);
            }

            for (size_t i = 0; i < f.bodies.size(); ++i)
            {
                const Body& body = f.bodies[i];
                const CellRect& ext = extents[i];
                // Taken out of the map as it is looked at: what is still in it after
                // the loop belongs to a body that no longer exists.
                entt::entity existing = entt::null;
                if (const auto it = byBody.find(body.id); it != byBody.end())
                {
                    existing = it->second;
                    byBody.erase(it);
                }

                if (!ext.valid())                       // a body with no water draws nothing
                {
                    if (existing != entt::null) { removeSurface(world, cm, renderer, existing); ++stats.removed; }
                    continue;
                }

                bool candidate = existing == entt::null;
                if (!candidate)
                {
                    const auto& ws = reg().get<WaterSurfaceComponent>(existing);
                    CellRect built; built.x0 = ws.cellX0; built.z0 = ws.cellZ0; built.x1 = ws.cellX1; built.z1 = ws.cellZ1;
                    candidate = ws.paramsKey != paramsKey || ws.level != body.level ||
                                overlap(dirtyCells, ext) || overlap(dirtyCells, built);
                }
                if (!candidate) { ++stats.kept; continue; }

                Lattice lattice;
                if (!buildLattice(tc, body.id, ext, lattice, opt.clipFor(body.level))) continue;
                const uint64_t hash = surfaceHash(lattice, body.level, opt);
                if (existing != entt::null)
                {
                    auto& ws = reg().get<WaterSurfaceComponent>(existing);
                    if (ws.hash == hash)                // an edit nearby that did not reach this lake
                    {
                        ws.cellX0 = ext.x0; ws.cellZ0 = ext.z0; ws.cellX1 = ext.x1; ws.cellZ1 = ext.z1;
                        ++stats.kept;
                        continue;
                    }
                }

                Surface s;
                if (!buildSurface(tc, lattice, ext, body.level, opt, s)) continue;
                if (s.empty())                          // wet cells but no area worth a polygon
                {
                    if (existing != entt::null) { removeSurface(world, cm, renderer, existing); ++stats.removed; }
                    continue;
                }
                apply(te, body.id, existing, s, paramsKey, stats);
            }

            // What is left in the map belongs to a body that no longer exists.
            for (auto& kv : byBody) { removeSurface(world, cm, renderer, kv.second); ++stats.removed; }
            f.dirty = false;
        }
    };
}

namespace WaterSurface
{
    void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer)
    {
        auto& reg = world.registry();
        Stats stats;

        // ── Give back the meshes of surfaces that are gone ───────────────────
        // An undo rebuilds every generated entity; a deleted landscape takes its
        // children with it. Neither tells anyone.
        if (auto w = g_owned.find(&world); w != g_owned.end())
        {
            auto& owned = w->second;
            for (auto it = owned.begin(); it != owned.end(); )
            {
                const entt::entity e = static_cast<entt::entity>(it->first);
                const auto* ws = reg.valid(e) ? reg.try_get<WaterSurfaceComponent>(e) : nullptr;
                if (ws && ws->meshId == it->second) { ++it; continue; }
                giveBackMesh(cm, renderer, it->second);
                it = owned.erase(it);
            }
            if (owned.empty()) g_owned.erase(w);
        }

        // ── Surfaces by landscape ────────────────────────────────────────────
        std::unordered_map<uint32_t, std::vector<entt::entity>> byTerrain;
        std::vector<entt::entity> orphans;
        for (auto [e, ws] : reg.view<WaterSurfaceComponent>().each())
        {
            if (ws.terrain != entt::null && reg.valid(ws.terrain) && reg.all_of<TerrainComponent>(ws.terrain))
                byTerrain[static_cast<uint32_t>(ws.terrain)].push_back(e);
            else
                orphans.push_back(e);
        }
        for (entt::entity e : orphans) { removeSurface(world, cm, renderer, e); ++stats.removed; }

        // ── Each landscape ───────────────────────────────────────────────────
        Pass pass{ world, cm, renderer };
        std::vector<entt::entity> terrains;
        for (auto e : reg.view<TerrainComponent>()) terrains.push_back(e);   // entities are created below
        for (entt::entity te : terrains)
        {
            auto& tc = reg.get<TerrainComponent>(te);
            std::vector<entt::entity> mine;
            if (auto found = byTerrain.find(static_cast<uint32_t>(te)); found != byTerrain.end())
                mine = std::move(found->second);
            // A landscape that never had water and has none now has nothing to look at.
            if (mine.empty() && tc.water.bodies.empty()) { tc.water.dirty = false; continue; }
            pass.terrain(te, tc, mine, stats);
        }

        g_stats = stats;
    }

    const Stats& lastStats() { return g_stats; }
}
