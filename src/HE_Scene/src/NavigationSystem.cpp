#include <HorizonScene/NavigationSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

#include <Recast.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>

#include <DebugDraw/DebugDraw.h>

#include <algorithm>
#include <cstring>
#include <cmath>

// ── NavMesh baking ────────────────────────────────────────────────────────────
bool NavigationSystem::bake(NavMeshComponent& nmc)
{
    nmc.navMesh  = nullptr;
    nmc.navQuery = nullptr;
    nmc.isDirty  = false;

    const auto& geo = nmc.geometry;
    if (geo.verts.empty() || geo.tris.empty()) return false;
    if (geo.verts.size() % 3 != 0) return false;
    if (geo.tris.size()  % 3 != 0) return false;

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

    rcContext ctx;

    // Heightfield
    rcHeightfield* hf = rcAllocHeightfield();
    if (!hf || !rcCreateHeightfield(&ctx, *hf, rcCfg.width, rcCfg.height,
                                     rcCfg.bmin, rcCfg.bmax, rcCfg.cs, rcCfg.ch))
    { rcFreeHeightField(hf); return false; }

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
    { rcFreeHeightField(hf); rcFreeCompactHeightfield(chf); return false; }
    rcFreeHeightField(hf);

    rcErodeWalkableArea(&ctx, rcCfg.walkableRadius, *chf);
    rcBuildDistanceField(&ctx, *chf);
    rcBuildRegions(&ctx, *chf, 0, rcCfg.minRegionArea, rcCfg.mergeRegionArea);

    rcContourSet* cset = rcAllocContourSet();
    if (!cset || !rcBuildContours(&ctx, *chf, rcCfg.maxSimplificationError, rcCfg.maxEdgeLen, *cset))
    { rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); return false; }

    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, rcCfg.maxVertsPerPoly, *pmesh))
    { rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pmesh); return false; }
    if (pmesh->npolys == 0)
    { rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pmesh); return false; }

    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, rcCfg.detailSampleDist,
                                          rcCfg.detailSampleMaxError, *dmesh))
    {
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
    { rcFreePolyMesh(pmesh); rcFreePolyMeshDetail(dmesh); return false; }

    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);

    dtNavMesh* rawMesh = dtAllocNavMesh();
    if (!rawMesh || rawMesh->init(navData, navDataSize, DT_TILE_FREE_DATA) != DT_SUCCESS)
    {
        dtFree(navData);
        dtFreeNavMesh(rawMesh);
        return false;
    }

    nmc.navMesh.reset(rawMesh, [](dtNavMesh* m){ dtFreeNavMesh(m); });

    dtNavMeshQuery* rawQuery = dtAllocNavMeshQuery();
    if (!rawQuery || rawQuery->init(rawMesh, 2048) != DT_SUCCESS)
    {
        dtFreeNavMeshQuery(rawQuery);
        return false;
    }
    nmc.navQuery.reset(rawQuery, [](dtNavMeshQuery* q){ dtFreeNavMeshQuery(q); });

    return true;
}

// ── Agent update ──────────────────────────────────────────────────────────────
void NavigationSystem::update(HorizonWorld& world, float dt)
{
    auto& reg = world.registry();

    // Find the first NavMeshComponent in the world
    NavMeshComponent* nmc = nullptr;
    for (auto [e, c] : reg.view<NavMeshComponent>().each())
    { nmc = &c; break; }

    if (!nmc || !nmc->navMesh || !nmc->navQuery) return;

    dtNavMeshQuery* query = nmc->navQuery.get();
    const dtQueryFilter filter;
    const float extents[3] = { 2.0f, 4.0f, 2.0f };

    for (auto [e, agent, tc] : reg.view<NavAgentComponent, TransformComponent>().each())
    {
        if (!agent.moving) continue;

        // If path is empty or stale, find one
        if (!agent.hasPath)
        {
            const float startPos[3] = { tc.position.x, tc.position.y, tc.position.z };
            const float endPos[3]   = { agent.targetPos.x, agent.targetPos.y, agent.targetPos.z };

            dtPolyRef startRef, endRef;
            float nearestStart[3], nearestEnd[3];
            query->findNearestPoly(startPos, extents, &filter, &startRef, nearestStart);
            query->findNearestPoly(endPos,   extents, &filter, &endRef,   nearestEnd);

            if (!startRef || !endRef) continue;

            dtPolyRef pathBuf[256];
            int pathLen = 0;
            query->findPath(startRef, endRef, nearestStart, nearestEnd, &filter, pathBuf, &pathLen, 256);
            if (pathLen == 0) continue;

            float straightPath[256 * 3];
            unsigned char flags[256];
            dtPolyRef   polys[256];
            int straightPathLen = 0;
            query->findStraightPath(nearestStart, nearestEnd,
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
        }

        if (!agent.hasPath || agent.pathIdx >= agent.path.size()) continue;

        // Move toward next waypoint
        const glm::vec3& wp = agent.path[agent.pathIdx];
        const glm::vec3  diff = wp - tc.position;
        const float dist = glm::length(diff);

        if (dist <= agent.stoppingDist || dist <= agent.speed * dt)
        {
            tc.position = wp;
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
            tc.position += dir * agent.speed * dt;
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
