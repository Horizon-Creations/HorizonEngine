#include <HorizonScene/NavigationSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/LODComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>

#include <ContentManager/ContentManager.h>

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>

#include <DebugDraw/DebugDraw.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>

// ── Static geometry collection ────────────────────────────────────────────────
namespace
{
    // Append one mesh's triangles in world space. A mirrored transform (negative
    // determinant) reverses the winding, and Recast decides walkability from the
    // triangle normal — so the winding is put back for those.
    void appendMeshGeometry(const StaticMeshAsset& mesh, const glm::mat4& xform,
                            NavMeshGeometry& out)
    {
        // The same two layouts the backends upload: cooked = interleaved
        // pos3+norm3+uv2, loose/editor = tightly packed positions.
        const float* pos    = nullptr;
        std::size_t  count  = 0;
        std::size_t  stride = 0;
        if (mesh.cooked && !mesh.interleaved.empty())
        {
            count  = mesh.vertexCount;
            stride = 8;
            if (mesh.interleaved.size() < count * stride) return;  // short cooked blob
            pos = mesh.interleaved.data();
        }
        else if (!mesh.vertices.empty())
        {
            pos    = mesh.vertices.data();
            count  = mesh.vertices.size() / 3;
            stride = 3;
        }
        if (!pos || count == 0 || mesh.indices.size() < 3) return;

        const int base = static_cast<int>(out.verts.size() / 3);
        out.verts.reserve(out.verts.size() + count * 3);
        for (std::size_t i = 0; i < count; ++i)
        {
            const glm::vec3 p = glm::vec3(xform * glm::vec4(pos[i * stride + 0],
                                                            pos[i * stride + 1],
                                                            pos[i * stride + 2], 1.0f));
            out.verts.push_back(p.x);
            out.verts.push_back(p.y);
            out.verts.push_back(p.z);
        }

        const bool flipped = glm::determinant(glm::mat3(xform)) < 0.0f;
        out.tris.reserve(out.tris.size() + mesh.indices.size());
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const std::uint32_t a = mesh.indices[i + 0];
            const std::uint32_t b = mesh.indices[i + 1];
            const std::uint32_t c = mesh.indices[i + 2];
            if (a >= count || b >= count || c >= count) continue;  // malformed asset
            out.tris.push_back(base + static_cast<int>(a));
            out.tris.push_back(base + static_cast<int>(flipped ? c : b));
            out.tris.push_back(base + static_cast<int>(flipped ? b : c));
        }
    }

    // A navmesh describes where an agent can walk when nothing has moved yet, so
    // anything that moves must stay out of it: a dynamic or kinematic body baked at
    // its authoring position leaves a permanent hole in the walkable surface once
    // it rolls away. The test walks up the parent chain because a crate parented to
    // a moving platform carries no body of its own.
    bool movesAtRuntime(const entt::registry& reg, entt::entity e)
    {
        for (entt::entity cur = e; cur != entt::null; )
        {
            if (const auto* rb = reg.try_get<RigidBodyComponent>(cur);
                rb && rb->type != HE::RigidBodyType::Static) return true;
            if (reg.try_get<CharacterControllerComponent>(cur)) return true;
            if (reg.try_get<NavAgentComponent>(cur)) return true;
            const auto* h = reg.try_get<HierarchyComponent>(cur);
            cur = h ? h->parent : entt::null;
        }
        return false;
    }
}

std::size_t NavigationSystem::collectStaticGeometry(HorizonWorld& world, ContentManager& content,
                                                    NavMeshGeometry& out)
{
    out.verts.clear();
    out.tris.clear();

    // worldMatrix is derived state that normally only the render extractor
    // refreshes. A bake runs from a panel, before extraction, so without this an
    // entity moved this frame would be collected at where it was last frame.
    HE::propagateTransforms(world);

    entt::registry& registry = world.registry();
    // Read-only handle for the per-entity probes below: registry::try_get never
    // creates a pool, but reaching for it through the const reference keeps that
    // guarantee visible — adding storage while a view is being iterated is the
    // classic way to invalidate it.
    const entt::registry& reg = registry;
    std::size_t meshes = 0;
    for (auto [e, mc, tc] : registry.view<MeshComponent, TransformComponent>().each())
    {
        // Hidden meshes are hidden for a reason (zone hiding) and triggers are
        // sensors the player is meant to walk through — neither is an obstacle.
        // Everything else visible stays in, including props with no physics at
        // all: they read as solid on screen, and Recast has no second source of
        // level geometry to fall back on.
        if (!mc.visible) continue;
        if (const auto* col = reg.try_get<ColliderComponent>(e); col && col->isTrigger) continue;
        if (movesAtRuntime(reg, e)) continue;

        // LOD0 rather than MeshComponent::meshAssetId: LODSystem overwrites that
        // every frame with whatever level suits the camera's current distance, so
        // baking from it would make the result depend on where the user was
        // standing. Terrain chunks are ordinary LOD entities here — that is how
        // the landscape gets in at all, the Landscape entity itself has no mesh.
        HE::UUID meshId = mc.meshAssetId;
        if (const auto* lod = reg.try_get<LODComponent>(e); lod && !lod->levels.empty())
            meshId = lod->levels.front().meshId;
        if (meshId == HE::UUID{}) continue;

        if (const StaticMeshAsset* mesh = content.getStaticMesh(meshId))
        {
            appendMeshGeometry(*mesh, tc.worldMatrix, out);
            ++meshes;
        }
    }

    const std::size_t tris = out.tris.size() / 3;
    HE_LOG_INFO(Nav, "NavMesh input collected: %zu triangle(s) from %zu mesh(es)", tris, meshes);
    // A full-resolution landscape alone is half a million triangles, and Recast
    // voxelises every one of them. Say so before the bake stalls the editor.
    if (tris > 250000)
        HE_LOG_WARN(Nav, "NavMesh input is %zu triangles — baking it will take a while. "
                         "Consider a smaller terrain resolution or a coarser Cell Size.", tris);
    return tris;
}

// ── NavMesh baking ────────────────────────────────────────────────────────────
bool NavigationSystem::bake(NavMeshComponent& nmc)
{
    nmc.navMesh  = nullptr;
    nmc.navQuery = nullptr;
    nmc.isDirty  = false;

    // Every failure below used to be a bare `return false`, and the only symptom
    // was "the agents don't move". Each stage now says which one gave up.
    HE_LOG_SLOW_SCOPE(Nav, 250.0, "NavMesh bake");

    const auto& geo = nmc.geometry;
    if (geo.verts.empty() || geo.tris.empty())
    {
        HE_LOG_WARN(Nav, "NavMesh bake skipped: no input geometry "
                         "(%zu vert floats, %zu tri indices)", geo.verts.size(), geo.tris.size());
        return false;
    }
    if (geo.verts.size() % 3 != 0 || geo.tris.size() % 3 != 0)
    {
        HE_LOG_ERROR(Nav, "NavMesh bake failed: malformed geometry — %zu vert floats and "
                          "%zu tri indices are not both multiples of 3",
                     geo.verts.size(), geo.tris.size());
        return false;
    }

    const int nverts = static_cast<int>(geo.verts.size() / 3);
    const int ntris  = static_cast<int>(geo.tris.size()  / 3);

    const NavMeshConfig& cfg = nmc.config;

    // ── Compute AABB ─────────────────────────────────────────────────────────
    float bmin[3] = { geo.verts[0], geo.verts[1], geo.verts[2] };
    float bmax[3] = { bmin[0], bmin[1], bmin[2] };
    for (int i = 1; i < nverts; ++i)
    {
        bmin[0] = std::min(bmin[0], geo.verts[i*3+0]);
        bmin[1] = std::min(bmin[1], geo.verts[i*3+1]);
        bmin[2] = std::min(bmin[2], geo.verts[i*3+2]);
        bmax[0] = std::max(bmax[0], geo.verts[i*3+0]);
        bmax[1] = std::max(bmax[1], geo.verts[i*3+1]);
        bmax[2] = std::max(bmax[2], geo.verts[i*3+2]);
    }

    // ── Recast configuration ─────────────────────────────────────────────────
    rcConfig rcCfg{};
    rcCfg.cs                   = cfg.cellSize;
    rcCfg.ch                   = cfg.cellHeight;
    rcCfg.walkableHeight       = static_cast<int>(std::ceil(cfg.walkableHeight  / cfg.cellHeight));
    rcCfg.walkableClimb        = static_cast<int>(std::floor(cfg.walkableClimb / cfg.cellHeight));
    rcCfg.walkableRadius       = static_cast<int>(std::ceil(cfg.walkableRadius  / cfg.cellSize));
    rcCfg.walkableSlopeAngle   = cfg.maxSlope;
    rcCfg.maxEdgeLen           = static_cast<int>(cfg.maxEdgeLen / cfg.cellSize);
    rcCfg.maxSimplificationError = cfg.maxSimplification;
    rcCfg.minRegionArea        = static_cast<int>(cfg.minRegionArea);
    rcCfg.mergeRegionArea      = static_cast<int>(cfg.mergeRegionArea);
    rcCfg.maxVertsPerPoly      = DT_VERTS_PER_POLYGON;
    rcCfg.detailSampleDist     = cfg.detailSampleDist < 0.9f ? 0.0f : cfg.cellSize * cfg.detailSampleDist;
    rcCfg.detailSampleMaxError = cfg.cellHeight * cfg.detailMaxError;
    // Ensure the Y range is large enough for the walkable height check.
    // The heightfield needs (walkableHeight + padding) voxels of free space above
    // the floor surface, so the bounding box must accommodate that.
    const float minYExtent = cfg.walkableHeight + 2.0f * cfg.cellHeight;
    if (bmax[1] - bmin[1] < minYExtent)
        bmax[1] = bmin[1] + minYExtent;

    rcCalcGridSize(bmin, bmax, cfg.cellSize, &rcCfg.width, &rcCfg.height);
    std::memcpy(rcCfg.bmin, bmin, sizeof(bmin));
    std::memcpy(rcCfg.bmax, bmax, sizeof(bmax));

    // rcCreateHeightfield allocates width*height span pointers up front. Now that a
    // whole landscape can reach this function, a kilometre of terrain at the default
    // 0.3 m cell asks for hundreds of megabytes and the editor simply stops
    // responding — refuse, and name the one setting that fixes it.
    constexpr long long kMaxGridCells = 4096ll * 4096ll;
    if (static_cast<long long>(rcCfg.width) * static_cast<long long>(rcCfg.height) > kMaxGridCells)
    {
        HE_LOG_ERROR(Nav, "NavMesh bake refused: a %dx%d cell grid at cellSize %.3f is beyond "
                          "the %lld-cell budget — raise Cell Size or bake a smaller area",
                     rcCfg.width, rcCfg.height, rcCfg.cs, kMaxGridCells);
        return false;
    }

    rcContext ctx;

    // Heightfield
    rcHeightfield* hf = rcAllocHeightfield();
    if (!hf || !rcCreateHeightfield(&ctx, *hf, rcCfg.width, rcCfg.height,
                                     rcCfg.bmin, rcCfg.bmax, rcCfg.cs, rcCfg.ch))
    {
        HE_LOG_ERROR(Nav, "NavMesh bake failed: heightfield allocation (%dx%d cells at "
                          "cellSize %.3f) — the bounds may be too large for the cell size",
                     rcCfg.width, rcCfg.height, rcCfg.cs);
        rcFreeHeightField(hf);
        return false;
    }

    std::vector<unsigned char> triAreas(ntris, 0);
    rcMarkWalkableTriangles(&ctx, rcCfg.walkableSlopeAngle,
        geo.verts.data(), nverts,
        geo.tris.data(), ntris,
        triAreas.data());
    rcRasterizeTriangles(&ctx,
        geo.verts.data(), nverts,
        geo.tris.data(), triAreas.data(), ntris,
        *hf, rcCfg.walkableClimb);

    rcFilterLowHangingWalkableObstacles(&ctx, rcCfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, rcCfg.walkableHeight, rcCfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, rcCfg.walkableHeight, *hf);

    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!chf || !rcBuildCompactHeightfield(&ctx, rcCfg.walkableHeight, rcCfg.walkableClimb, *hf, *chf))
    {
        HE_LOG_ERROR(Nav, "%s", "NavMesh bake failed: rcBuildCompactHeightfield");
        rcFreeHeightField(hf); rcFreeCompactHeightfield(chf);
        return false;
    }
    rcFreeHeightField(hf);

    rcErodeWalkableArea(&ctx, rcCfg.walkableRadius, *chf);
    rcBuildDistanceField(&ctx, *chf);
    rcBuildRegions(&ctx, *chf, 0, rcCfg.minRegionArea, rcCfg.mergeRegionArea);

    rcContourSet* cset = rcAllocContourSet();
    if (!cset || !rcBuildContours(&ctx, *chf, rcCfg.maxSimplificationError, rcCfg.maxEdgeLen, *cset))
    {
        HE_LOG_ERROR(Nav, "%s", "NavMesh bake failed: rcBuildContours");
        rcFreeCompactHeightfield(chf); rcFreeContourSet(cset);
        return false;
    }

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, rcCfg.maxVertsPerPoly, *pmesh))
    {
        HE_LOG_ERROR(Nav, "%s", "NavMesh bake failed: rcBuildPolyMesh");
        rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pmesh);
        return false;
    }
    if (pmesh->npolys == 0)
    {
        // Not a crash, just an empty result — usually walkableRadius/maxSlope
        // eroding the whole surface away, or geometry that is all too steep.
        HE_LOG_WARN(Nav, "NavMesh bake produced 0 polygons from %d triangle(s) — check "
                         "maxSlope (%.1f deg), walkableRadius (%.2f) and walkableHeight (%.2f) "
                         "against the geometry", ntris, cfg.maxSlope, cfg.walkableRadius,
                    cfg.walkableHeight);
        rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pmesh);
        return false;
    }

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, rcCfg.detailSampleDist,
                                          rcCfg.detailSampleMaxError, *dmesh))
    {
        HE_LOG_ERROR(Nav, "%s", "NavMesh bake failed: rcBuildPolyMeshDetail");
        rcFreeCompactHeightfield(chf); rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh); rcFreePolyMeshDetail(dmesh);
        return false;
    }
    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);

    // Mark all polys as walkable
    for (int i = 0; i < pmesh->npolys; ++i)
        pmesh->flags[i] = 1;

    // ── Detour NavMesh creation ───────────────────────────────────────────────
    dtNavMeshCreateParams params{};
    params.verts            = pmesh->verts;
    params.vertCount        = pmesh->nverts;
    params.polys            = pmesh->polys;
    params.polyAreas        = pmesh->areas;
    params.polyFlags        = pmesh->flags;
    params.polyCount        = pmesh->npolys;
    params.nvp              = pmesh->nvp;
    params.detailMeshes     = dmesh->meshes;
    params.detailVerts      = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris       = dmesh->tris;
    params.detailTriCount   = dmesh->ntris;
    params.walkableHeight   = cfg.walkableHeight;
    params.walkableRadius   = cfg.walkableRadius;
    params.walkableClimb    = cfg.walkableClimb;
    std::memcpy(params.bmin, pmesh->bmin, sizeof(params.bmin));
    std::memcpy(params.bmax, pmesh->bmax, sizeof(params.bmax));
    params.cs = rcCfg.cs;
    params.ch = rcCfg.ch;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
    {
        HE_LOG_ERROR(Nav, "NavMesh bake failed: dtCreateNavMeshData (%d polys, %d verts) — "
                          "the mesh most likely exceeds Detour's per-tile limits",
                     pmesh->npolys, pmesh->nverts);
        rcFreePolyMesh(pmesh); rcFreePolyMeshDetail(dmesh);
        return false;
    }

    const int polyCount = pmesh->npolys;
    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);

    dtNavMesh* rawMesh = dtAllocNavMesh();
    if (!rawMesh || rawMesh->init(navData, navDataSize, DT_TILE_FREE_DATA) != DT_SUCCESS)
    {
        HE_LOG_ERROR(Nav, "NavMesh bake failed: dtNavMesh::init (%d bytes of nav data)",
                     navDataSize);
        dtFree(navData);
        dtFreeNavMesh(rawMesh);
        return false;
    }

    nmc.navMesh.reset(rawMesh, [](dtNavMesh* m){ dtFreeNavMesh(m); });

    dtNavMeshQuery* rawQuery = dtAllocNavMeshQuery();
    if (!rawQuery || rawQuery->init(rawMesh, 2048) != DT_SUCCESS)
    {
        HE_LOG_ERROR(Nav, "%s", "NavMesh bake failed: dtNavMeshQuery::init");
        dtFreeNavMeshQuery(rawQuery);
        return false;
    }
    nmc.navQuery.reset(rawQuery, [](dtNavMeshQuery* q){ dtFreeNavMeshQuery(q); });

    HE_LOG_INFO(Nav, "NavMesh baked: %d polygon(s) from %d input triangle(s), "
                     "%d bytes of nav data, grid %dx%d",
                polyCount, ntris, navDataSize, rcCfg.width, rcCfg.height);
    return true;
}

// ── Agent update ──────────────────────────────────────────────────────────────
namespace
{
    // How far the target has to move before the current path is thrown away and
    // planned again. Zero would mean a full findPath every frame for an agent
    // chasing a player, and for every script that writes targetPos from a value
    // with float noise in it. A quarter of a metre is well inside the 0.6 m
    // radius a navmesh is baked for, so a target that moved less than this
    // cannot produce a meaningfully different route through it — while a chase
    // still re-plans about fifteen times a second at walking speed.
    constexpr float kRepathDistance = 0.25f;

    // Search a route from `worldPos` to agent.targetPos and store it on the
    // agent. Returns whether one was found; leaves the previous path alone when
    // it fails, so a passing failure does not strand a walking agent.
    //
    // Shared by the per-frame update and moveTo() on purpose: a script asking
    // "can this NPC get there" and the system's own re-planning must never
    // disagree about what is reachable.
    bool planPath(NavAgentComponent& agent, const glm::vec3& worldPos,
                  dtNavMeshQuery& query, std::uint32_t id)
    {
        const dtQueryFilter filter;
        // Search box around each end: an agent standing slightly above the floor
        // or a target clicked a metre off the mesh still finds its polygon.
        const float extents[3] = { 2.0f, 4.0f, 2.0f };

        const float startPos[3] = { worldPos.x, worldPos.y, worldPos.z };
        const float endPos[3]   = { agent.targetPos.x, agent.targetPos.y, agent.targetPos.z };

        dtPolyRef startRef, endRef;
        float nearestStart[3], nearestEnd[3];
        query.findNearestPoly(startPos, extents, &filter, &startRef, nearestStart);
        query.findNearestPoly(endPos,   extents, &filter, &endRef,   nearestEnd);

        if (!startRef || !endRef)
        {
            // The agent or its target is off the navmesh (spawned in the air,
            // target clicked outside the baked area). Silent until now.
            HE_LOG_THROTTLE(Nav, Warning, 5.0,
                            "Entity %u: no path — %s is not on the NavMesh "
                            "(agent at %.1f/%.1f/%.1f, target %.1f/%.1f/%.1f)",
                            id,
                            !startRef ? (!endRef ? "neither the agent nor the target"
                                                 : "the agent")
                                      : "the target",
                            worldPos.x, worldPos.y, worldPos.z,
                            agent.targetPos.x, agent.targetPos.y, agent.targetPos.z);
            return false;
        }

        dtPolyRef pathBuf[256];
        int pathLen = 0;
        query.findPath(startRef, endRef, nearestStart, nearestEnd, &filter, pathBuf, &pathLen, 256);
        if (pathLen == 0)
        {
            HE_LOG_THROTTLE(Nav, Warning, 5.0,
                            "Entity %u: findPath returned an empty path — target is "
                            "probably on a disconnected NavMesh island", id);
            return false;
        }
        if (pathLen >= 256)
            HE_LOG_THROTTLE(Nav, Warning, 10.0,
                            "Entity %u: path hit the 256-polygon buffer limit and is "
                            "truncated — the agent will stop short of its target", id);

        float straightPath[256 * 3];
        unsigned char flags[256];
        dtPolyRef   polys[256];
        int straightPathLen = 0;
        query.findStraightPath(nearestStart, nearestEnd,
            pathBuf, pathLen,
            straightPath, flags, polys,
            &straightPathLen, 256);

        agent.path.clear();
        for (int i = 0; i < straightPathLen; ++i)
            agent.path.push_back({ straightPath[i*3+0], straightPath[i*3+1], straightPath[i*3+2] });

        // Skip the first waypoint — it is the start position (nearestStart),
        // not a future goal. Start from index 1 if possible.
        agent.pathIdx = (agent.path.size() > 1) ? 1 : 0;
        agent.hasPath = !agent.path.empty();
        // Remember what this path was planned for. Without it the staleness test
        // in update() has nothing to compare against and never fires.
        agent.pathTarget = agent.targetPos;
        return agent.hasPath;
    }

    // The scene's NavMesh: the first one found, matching what update() has always
    // done. A second NavMeshComponent in a scene is ignored, not merged.
    NavMeshComponent* sceneNavMesh(entt::registry& reg)
    {
        for (auto [e, c] : reg.view<NavMeshComponent>().each())
            return &c;
        return nullptr;
    }
}

bool NavigationSystem::moveTo(HorizonWorld& world, entt::entity e, const glm::vec3& target)
{
    entt::registry& reg = world.registry();
    auto* agent = reg.try_get<NavAgentComponent>(e);
    if (!agent)
    {
        HE_LOG_THROTTLE(Nav, Warning, 5.0,
                        "Entity %u: nav.moveTo did nothing — the entity has no Nav Agent "
                        "component", static_cast<std::uint32_t>(e));
        return false;
    }

    // Written before the search, so the destination is on record even when it
    // turns out to be unreachable — that is what the Inspector then shows the
    // author, instead of the previous target with no sign anything was asked.
    agent->targetPos = target;

    NavMeshComponent* nmc = sceneNavMesh(reg);
    const bool planned = nmc && nmc->navQuery &&
                         planPath(*agent, HE::worldPositionOf(world, e), *nmc->navQuery,
                                  static_cast<std::uint32_t>(e));
    if (!planned)
    {
        if (!nmc || !nmc->navQuery)
            HE_LOG_THROTTLE(Nav, Warning, 5.0, "%s",
                            nmc ? "nav.moveTo failed: the scene's NavMesh is not baked"
                                : "nav.moveTo failed: the scene has no NavMesh entity");
        // Stopped rather than left walking: an agent told to go somewhere new
        // must not keep walking to the old place as if nothing was said. The
        // next update() releases the character's velocity.
        agent->moving  = false;
        agent->hasPath = false;
        return false;
    }

    agent->moving = true;
    return true;
}

float NavigationSystem::remainingDistance(HorizonWorld& world, entt::entity e)
{
    const auto* agent = world.registry().try_get<NavAgentComponent>(e);
    if (!agent || !agent->hasPath || agent->pathIdx >= agent->path.size())
        return -1.0f;

    // Along the path, not to the target: the two differ by exactly the detour
    // the navmesh found, which is the number a caller deciding "close enough to
    // attack" is actually asking about.
    glm::vec3 prev  = HE::worldPositionOf(world, e);
    float     total = 0.0f;
    for (std::size_t i = agent->pathIdx; i < agent->path.size(); ++i)
    {
        total += glm::length(agent->path[i] - prev);
        prev   = agent->path[i];
    }
    return total;
}

void NavigationSystem::update(HorizonWorld& world, float dt, PhysicsWorld* physics)
{
    auto& reg = world.registry();

    // ── autoStart ────────────────────────────────────────────────────────────
    // Before any early return below, so an agent that wants to walk is on record
    // by the time the "no navmesh" warning decides whether to fire.
    //
    // `physics` is the play gate (see the header): the editor hands one over in
    // play mode only, the packaged game always has one. Latched per agent rather
    // than re-read, or stopping an auto-started agent would restart it the next
    // frame. Runtime state, so leaving play mode restores it with the scene.
    if (physics)
    {
        for (auto [e, agent] : reg.view<NavAgentComponent>().each())
        {
            if (!agent.autoStart || agent.autoStarted) continue;
            agent.autoStarted = true;
            // Only if nothing has started it already: an onStart script that
            // called moveTo() before this first tick has a planned path, and
            // throwing it away would re-search the identical route.
            if (!agent.moving)
            {
                agent.moving  = true;
                agent.hasPath = false;
            }
        }
    }

    NavMeshComponent* nmc = sceneNavMesh(reg);
    if (!nmc || !nmc->navMesh || !nmc->navQuery)
    {
        // Agents standing still because there is no baked navmesh is the single
        // most common navigation complaint — say so, once every few seconds, and
        // only while something is actually trying to move.
        bool wantsToMove = false;
        for (auto [e, agent] : reg.view<NavAgentComponent>().each())
            if (agent.moving) { wantsToMove = true; break; }
        if (wantsToMove)
            HE_LOG_THROTTLE(Nav, Warning, 5.0, "%s",
                            nmc ? "Nav agents want to move but the NavMesh is not baked"
                                : "Nav agents want to move but the scene has no NavMesh entity");
        return;
    }

    dtNavMeshQuery* query = nmc->navQuery.get();

    // Read-only handle for the per-entity probe below, for the reason
    // collectStaticGeometry gives: try_get never creates a pool, and creating one
    // while a view is being iterated is the classic way to invalidate it.
    const entt::registry& creg = reg;

    for (auto [e, agent, tc] : reg.view<NavAgentComponent, TransformComponent>().each())
    {
        const uint32_t id = static_cast<uint32_t>(e);
        // What decides how this agent is moved at all: a character controller is
        // steered, anything else is not.
        const auto* cc = creg.try_get<CharacterControllerComponent>(e);

        // A character keeps the velocity it was last given, so an agent this
        // frame declines to steer would glide on at walking speed until
        // something else writes to it. One zero write on the frame the steering
        // ends, and only the horizontal part: the controller owns Y (gravity,
        // and whatever a jump left in flight), same rule MovementSystem follows.
        //
        // EVERY early exit below has to go through here, not just the "stopped
        // moving" one. The case that made this a lambda: a chaser writes the
        // player's position into targetPos every tick, the player steps off the
        // baked surface, the repath fails on every frame from then on — and the
        // NPC sails away in its last direction while the log calmly repeats that
        // the target is not on the NavMesh.
        const auto releaseCharacter = [&]()
        {
            if (agent.drivingCharacter && physics)
                physics->setCharacterVelocity(id, glm::vec3(0.0f, cc ? cc->velocity.y : 0.0f, 0.0f));
            agent.drivingCharacter = false;
        };

        if (!agent.moving) { releaseCharacter(); continue; }

        // A moved target invalidates the path. This is what NavAgentComponent's
        // header has always claimed and what the code never did: an agent handed
        // a new destination mid-walk kept following the old one to the end.
        if (agent.hasPath && glm::distance(agent.targetPos, agent.pathTarget) > kRepathDistance)
            agent.hasPath = false;

        // Everything from here is WORLD space. The navmesh is baked in world
        // space and its waypoints come out that way, while TransformComponent
        // holds a position that is local to the entity's parent — the two are
        // only the same for an unparented agent, and comparing them anyway is
        // how a parented NPC walks off by its parent's offset.
        const glm::vec3 worldPos = HE::worldPositionOf(world, e);

        // No path, or the one it had was just thrown away: find one. Same search
        // a script's moveTo() runs, so an agent that started walking by itself
        // and one that was told to cannot end up on different routes.
        if (!agent.hasPath && !planPath(agent, worldPos, *query, id))
        {
            releaseCharacter();
            continue;
        }

        if (agent.pathIdx >= agent.path.size()) { releaseCharacter(); continue; }

        // ── Follow the path with a character controller ──────────────────────
        // The way the player moves, and for the same reasons: an agent steered
        // by writing its transform walks through walls, ignores the ground it
        // was pathed over, and — since the physics step writes the pose of every
        // body back from Jolt — leaves its collider standing at the spawn point.
        if (physics && cc)
        {
            // The component is authoring data; the controller Jolt steers is
            // built by PhysicsWorld::initialize/addEntity. Without one,
            // setCharacterVelocity is a silent no-op and the agent stands there
            // for the rest of the session with nothing in the log.
            // hasCharacter, not hasPhysics: the latter is an OR, so an agent
            // whose character failed to build but whose kinematic proxy body did
            // would pass it, and setCharacterVelocity below would then be a
            // silent no-op for the rest of the session.
            if (!physics->hasCharacter(id))
            {
                HE_LOG_THROTTLE(Nav, Warning, 5.0,
                                "Entity %u: nav agent has a Character Controller but no "
                                "character was built for it in the physics world — it cannot "
                                "be moved",
                                id);
                releaseCharacter();
                continue;
            }

            // Horizontal distances only. The waypoints lie ON the navmesh while
            // a character's origin does not, so a 3D distance to the next
            // waypoint can never fall below stoppingDist and the agent would
            // circle it forever.
            auto flatDistTo = [&](const glm::vec3& p) {
                return glm::length(glm::vec2(p.x - worldPos.x, p.z - worldPos.z));
            };

            // Consume every waypoint already inside the stopping distance, so a
            // corner reached this frame does not cost a frame of standing still.
            while (agent.pathIdx < agent.path.size() &&
                   flatDistTo(agent.path[agent.pathIdx]) <= agent.stoppingDist)
                ++agent.pathIdx;

            if (agent.pathIdx >= agent.path.size())
            {
                agent.moving  = false;
                agent.hasPath = false;
                physics->setCharacterVelocity(id, glm::vec3(0.0f, cc->velocity.y, 0.0f));
                agent.drivingCharacter = false;
                continue;
            }

            const glm::vec3& wp   = agent.path[agent.pathIdx];
            const float      dist = flatDistTo(wp);
            const glm::vec2  dir  = (dist > 1e-4f)
                ? glm::vec2(wp.x - worldPos.x, wp.z - worldPos.z) / dist
                : glm::vec2(0.0f);
            // Never faster than the distance that is left, or the agent
            // overshoots a near waypoint and has to come back for it.
            const float step = (dt > 0.0f) ? std::min(agent.speed, dist / dt) : agent.speed;

            // Y is read back, not written: the controller owns gravity and
            // whatever a jump left in flight (see MovementSystem).
            physics->setCharacterVelocity(id, glm::vec3(dir.x * step, cc->velocity.y, dir.y * step));
            agent.drivingCharacter = true;
            continue;   // the physics step writes the transform back
        }

        // ── Everything else: move the transform ──────────────────────────────
        // Two cases reach here. One is an agent with no character controller.
        // The other is any agent while nothing is simulating — the editor ticks
        // this system in edit mode too, with no PhysicsWorld, and that is what
        // the Inspector's "Go" button previews.
        //
        // But if the entity has a physics representation, physics owns its pose:
        // a transform written here is either overwritten by the next step (any
        // non-static body) or leaves the collider behind while the mesh walks
        // away (a static one). Neither is movement, so say what is missing
        // instead of pretending.
        if (physics && physics->hasPhysics(id))
        {
            HE_LOG_THROTTLE(Nav, Warning, 5.0,
                            "Entity %u: nav agent has a physics body but no Character "
                            "Controller — add one, or navigation cannot move it",
                            id);
            continue;
        }

        // Nothing physical in the way: writing the transform IS the movement. A
        // bodyless agent is a legitimate thing — a camera dolly, a marker
        // following a route — and this is the case where it is right. Full 3D
        // distances here, unlike the character path: this entity's origin is
        // wherever the author put it and the waypoint height is meant.
        const glm::vec3& wp   = agent.path[agent.pathIdx];
        const glm::vec3  diff = wp - worldPos;
        const float      dist = glm::length(diff);

        if (dist <= agent.stoppingDist || dist <= agent.speed * dt)
        {
            tc.position = HE::localPositionForWorld(world, e, wp);
            tc.dirty = true;
            ++agent.pathIdx;
            if (agent.pathIdx >= agent.path.size())
            {
                agent.moving  = false;
                agent.hasPath = false;
            }
        }
        else
        {
            const glm::vec3 dir = diff / dist;
            tc.position = HE::localPositionForWorld(world, e,
                                                    worldPos + dir * agent.speed * dt);
            tc.dirty = true;
        }
    }
}

// ── Debug visualization ────────────────────────────────────────────────────
void NavigationSystem::extractNavMeshWireframe(const NavMeshComponent& nmc, DebugDrawBuffer& out,
                                               const glm::vec3& color)
{
    const dtNavMesh* mesh = nmc.navMesh.get();
    if (!mesh) return;

    for (int ti = 0; ti < mesh->getMaxTiles(); ++ti)
    {
        const dtMeshTile* tile = mesh->getTile(ti);
        if (!tile || !tile->header) continue;

        for (int pi = 0; pi < tile->header->polyCount; ++pi)
        {
            const dtPoly& poly = tile->polys[pi];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;

            for (int vi = 0; vi < poly.vertCount; ++vi)
            {
                const int v0 = poly.verts[vi];
                const int v1 = poly.verts[(vi + 1) % poly.vertCount];
                const glm::vec3 a(tile->verts[v0 * 3 + 0], tile->verts[v0 * 3 + 1], tile->verts[v0 * 3 + 2]);
                const glm::vec3 b(tile->verts[v1 * 3 + 0], tile->verts[v1 * 3 + 1], tile->verts[v1 * 3 + 2]);
                out.line(a, b, color);
            }
        }
    }
}
