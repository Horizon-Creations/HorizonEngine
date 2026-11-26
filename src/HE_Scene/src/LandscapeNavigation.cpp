#include "HorizonScene/LandscapeNavigation.h"

#include <Diagnostics/Log.h>

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_set>

namespace HE
{
namespace
{
double distToSquare(double px, double pz, double x0, double z0, double x1, double z1)
{
	const double dx = std::max({ x0 - px, 0.0, px - x1 });
	const double dz = std::max({ z0 - pz, 0.0, pz - z1 });
	return std::sqrt(dx * dx + dz * dz);
}

// Recast has its own frees; one place that releases everything a build holds, whatever stage it gave up at.
struct RcScratch
{
	rcHeightfield*        hf = nullptr;
	rcCompactHeightfield* chf = nullptr;
	rcContourSet*         cset = nullptr;
	rcPolyMesh*           pmesh = nullptr;
	rcPolyMeshDetail*     dmesh = nullptr;
	~RcScratch()
	{
		rcFreeHeightField(hf);
		rcFreeCompactHeightfield(chf);
		rcFreeContourSet(cset);
		rcFreePolyMesh(pmesh);
		rcFreePolyMeshDetail(dmesh);
	}
};
} // namespace

LandscapeNavBuild buildLandscapeNavTile(const std::function<std::shared_ptr<const LandscapeTile>(LandscapeTileKey)>& tileAt, LandscapeTileKey key,
                                        int tilesX, int tilesZ, float heightMinM, float heightScaleM, const NavMeshConfig& cfg,
                                        unsigned char*& data, int& size, int& polygons)
{
	data = nullptr;
	size = 0;
	polygons = 0;
	const std::shared_ptr<const LandscapeTile> own = tileAt(key);
	if (!own || own->heights.empty() || key.x < 0 || key.z < 0 || key.x >= tilesX || key.z >= tilesZ) return LandscapeNavBuild::Failed;
	const LandscapeGeometry& g = own->geometry;
	const int cells = g.hCells;
	if (own->heights[0].size() != static_cast<size_t>(cells + 1) * (cells + 1)) return LandscapeNavBuild::Failed;

	// The grid: a whole number of cells per tile, so that the tile borders of Recast and Detour are the same line.
	const int tileSize = std::max(1, static_cast<int>(std::ceil(g.tileM / cfg.cellSize)));
	const float cs = g.tileM / static_cast<float>(tileSize);
	const float ch = cfg.cellHeight;
	rcConfig rc{};
	rc.cs = cs;
	rc.ch = ch;
	rc.walkableSlopeAngle = cfg.maxSlope;
	rc.walkableHeight = static_cast<int>(std::ceil(cfg.walkableHeight / ch));
	rc.walkableClimb = static_cast<int>(std::floor(cfg.walkableClimb / ch));
	rc.walkableRadius = static_cast<int>(std::ceil(cfg.walkableRadius / cs));
	rc.maxEdgeLen = static_cast<int>(cfg.maxEdgeLen / cs);
	rc.maxSimplificationError = cfg.maxSimplification;
	rc.minRegionArea = static_cast<int>(cfg.minRegionArea);       // cells, as NavigationSystem::bake passes them
	rc.mergeRegionArea = static_cast<int>(cfg.mergeRegionArea);
	rc.maxVertsPerPoly = DT_VERTS_PER_POLYGON;
	rc.detailSampleDist = cfg.detailSampleDist < 0.9f ? 0.0f : cs * cfg.detailSampleDist;
	rc.detailSampleMaxError = ch * cfg.detailMaxError;
	rc.tileSize = tileSize;
	rc.borderSize = rc.walkableRadius + 3;
	rc.width = tileSize + rc.borderSize * 2;
	rc.height = tileSize + rc.borderSize * 2;

	// The heights the build needs: the tile and a strip of its neighbours as wide as the border (plus one sample, for the triangle that
	// crosses the border). Global sample index -> the tile that owns it; the border samples two tiles share belong to the higher one.
	const float spacing = g.tileM / static_cast<float>(cells);
	const int k = static_cast<int>(std::ceil(rc.borderSize * cs / spacing)) + 1;
	const int dim = cells + 1 + 2 * k;
	const float scale = heightScaleM / 65535.0f;
	std::vector<float> hts(static_cast<size_t>(dim) * dim);
	float ymin = 1e30f, ymax = -1e30f;
	{
		std::shared_ptr<const LandscapeTile> cacheTile;
		LandscapeTileKey cacheKey{ -1, -1 };
		for (int j = 0; j < dim; ++j)
		{
			const int gz = std::clamp(key.z * cells + (j - k), 0, tilesZ * cells);
			const int oz = std::min(gz / cells, tilesZ - 1);
			for (int i = 0; i < dim; ++i)
			{
				const int gx = std::clamp(key.x * cells + (i - k), 0, tilesX * cells);
				const int ox = std::min(gx / cells, tilesX - 1);
				if (!cacheTile || cacheKey.x != ox || cacheKey.z != oz)
				{
					cacheTile = tileAt({ ox, oz });
					cacheKey = { ox, oz };
				}
				if (!cacheTile || cacheTile->heights.empty() || cacheTile->geometry != g) return LandscapeNavBuild::Failed;
				const float h = heightMinM + static_cast<float>(cacheTile->heights[0][static_cast<size_t>(gz - oz * cells) * (cells + 1) + (gx - ox * cells)]) * scale;
				hts[static_cast<size_t>(j) * dim + i] = h;
				ymin = std::min(ymin, h);
				ymax = std::max(ymax, h);
			}
		}
	}

	// Vertical bounds with room for the agent's height above the highest floor.
	ymax = std::max(ymax + cfg.walkableHeight + 2.0f * ch, ymin + cfg.walkableHeight + 2.0f * ch);
	if ((ymax - ymin) / ch > 8000.0f)
	{
		HE_LOG_WARN(Nav, "LandscapeNavigation: tile (%d, %d) spans %.0f m of height, more than %.0f m at cellHeight %.2f; skipped", key.x, key.z,
		            ymax - ymin, 8000.0f * ch, ch);
		return LandscapeNavBuild::Failed;
	}

	// Triangles of the window, "\" diagonal, counter-clockwise seen from above (an upward normal is what Recast calls walkable).
	std::vector<float> verts(static_cast<size_t>(dim) * dim * 3);
	const float x0 = key.x * g.tileM - k * spacing, z0 = key.z * g.tileM - k * spacing;
	for (int j = 0; j < dim; ++j)
		for (int i = 0; i < dim; ++i)
		{
			float* v = &verts[(static_cast<size_t>(j) * dim + i) * 3];
			v[0] = x0 + i * spacing;
			v[1] = hts[static_cast<size_t>(j) * dim + i];
			v[2] = z0 + j * spacing;
		}
	std::vector<int> tris;
	tris.reserve(static_cast<size_t>(dim - 1) * (dim - 1) * 6);
	for (int j = 0; j + 1 < dim; ++j)
		for (int i = 0; i + 1 < dim; ++i)
		{
			const int a = j * dim + i, b = (j + 1) * dim + i, c = j * dim + i + 1, d = (j + 1) * dim + i + 1;
			tris.insert(tris.end(), { a, b, d, a, d, c });
		}
	const int nverts = dim * dim, ntris = static_cast<int>(tris.size() / 3);

	rc.bmin[0] = key.x * g.tileM - rc.borderSize * cs;
	rc.bmin[1] = ymin;
	rc.bmin[2] = key.z * g.tileM - rc.borderSize * cs;
	rc.bmax[0] = (key.x + 1) * g.tileM + rc.borderSize * cs;
	rc.bmax[1] = ymax;
	rc.bmax[2] = (key.z + 1) * g.tileM + rc.borderSize * cs;

	rcContext ctx;
	RcScratch s;
	s.hf = rcAllocHeightfield();
	if (!s.hf || !rcCreateHeightfield(&ctx, *s.hf, rc.width, rc.height, rc.bmin, rc.bmax, rc.cs, rc.ch)) return LandscapeNavBuild::Failed;
	std::vector<unsigned char> areas(static_cast<size_t>(ntris), 0);
	rcMarkWalkableTriangles(&ctx, rc.walkableSlopeAngle, verts.data(), nverts, tris.data(), ntris, areas.data());
	if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(), areas.data(), ntris, *s.hf, rc.walkableClimb)) return LandscapeNavBuild::Failed;
	verts.clear();
	verts.shrink_to_fit();
	tris.clear();
	tris.shrink_to_fit();

	rcFilterLowHangingWalkableObstacles(&ctx, rc.walkableClimb, *s.hf);
	rcFilterLedgeSpans(&ctx, rc.walkableHeight, rc.walkableClimb, *s.hf);
	rcFilterWalkableLowHeightSpans(&ctx, rc.walkableHeight, *s.hf);

	s.chf = rcAllocCompactHeightfield();
	if (!s.chf || !rcBuildCompactHeightfield(&ctx, rc.walkableHeight, rc.walkableClimb, *s.hf, *s.chf)) return LandscapeNavBuild::Failed;
	rcFreeHeightField(s.hf);
	s.hf = nullptr;
	if (!rcErodeWalkableArea(&ctx, rc.walkableRadius, *s.chf)) return LandscapeNavBuild::Failed;
	if (!rcBuildDistanceField(&ctx, *s.chf)) return LandscapeNavBuild::Failed;
	if (!rcBuildRegions(&ctx, *s.chf, rc.borderSize, rc.minRegionArea, rc.mergeRegionArea)) return LandscapeNavBuild::Failed;

	s.cset = rcAllocContourSet();
	if (!s.cset || !rcBuildContours(&ctx, *s.chf, rc.maxSimplificationError, rc.maxEdgeLen, *s.cset)) return LandscapeNavBuild::Failed;
	if (s.cset->nconts == 0) return LandscapeNavBuild::Empty;
	s.pmesh = rcAllocPolyMesh();
	if (!s.pmesh || !rcBuildPolyMesh(&ctx, *s.cset, rc.maxVertsPerPoly, *s.pmesh)) return LandscapeNavBuild::Failed;
	if (s.pmesh->npolys == 0) return LandscapeNavBuild::Empty;
	if (s.pmesh->npolys >= LandscapeNavigation::kMaxPolysPerTile)
	{
		HE_LOG_WARN(Nav, "LandscapeNavigation: tile (%d, %d) has %d polygons, more than a polygon reference holds (%d); skipped", key.x, key.z,
		            s.pmesh->npolys, LandscapeNavigation::kMaxPolysPerTile);
		return LandscapeNavBuild::Failed;
	}
	s.dmesh = rcAllocPolyMeshDetail();
	if (!s.dmesh || !rcBuildPolyMeshDetail(&ctx, *s.pmesh, *s.chf, rc.detailSampleDist, rc.detailSampleMaxError, *s.dmesh)) return LandscapeNavBuild::Failed;
	rcFreeCompactHeightfield(s.chf);
	s.chf = nullptr;

	for (int i = 0; i < s.pmesh->npolys; ++i) s.pmesh->flags[i] = 1;   // all walkable, like NavigationSystem::bake

	dtNavMeshCreateParams p{};
	p.verts = s.pmesh->verts;
	p.vertCount = s.pmesh->nverts;
	p.polys = s.pmesh->polys;
	p.polyAreas = s.pmesh->areas;
	p.polyFlags = s.pmesh->flags;
	p.polyCount = s.pmesh->npolys;
	p.nvp = s.pmesh->nvp;
	p.detailMeshes = s.dmesh->meshes;
	p.detailVerts = s.dmesh->verts;
	p.detailVertsCount = s.dmesh->nverts;
	p.detailTris = s.dmesh->tris;
	p.detailTriCount = s.dmesh->ntris;
	p.walkableHeight = cfg.walkableHeight;
	p.walkableRadius = cfg.walkableRadius;
	p.walkableClimb = cfg.walkableClimb;
	p.tileX = key.x;
	p.tileY = key.z;
	p.tileLayer = 0;
	std::memcpy(p.bmin, s.pmesh->bmin, sizeof(p.bmin));
	std::memcpy(p.bmax, s.pmesh->bmax, sizeof(p.bmax));
	p.cs = rc.cs;
	p.ch = rc.ch;
	p.buildBvTree = true;
	polygons = s.pmesh->npolys;
	if (!dtCreateNavMeshData(&p, &data, &size))
	{
		data = nullptr;
		size = 0;
		polygons = 0;
		return LandscapeNavBuild::Failed;
	}
	return LandscapeNavBuild::Ok;
}

// ── The object ─────────────────────────────────────────────────────────────────────────────────────

LandscapeNavigation::LandscapeNavigation(LandscapeNavConfig config, ThreadPool* pool)
    : m_config(config), m_pool(pool ? pool : &globalPool()), m_shared(std::make_shared<Shared>()), m_token(CancelToken::create())
{
}

LandscapeNavigation::~LandscapeNavigation()
{
	m_shared->closed.store(true);
	quiesce();
	for (Result& r : m_shared->done) dtFree(r.data);
	m_shared->done.clear();
}

void LandscapeNavigation::quiesce()
{
	m_token.cancel();
	for (auto& [k, e] : m_entries)
		if (!e.done) e.job.cancel();
	const auto t0 = std::chrono::steady_clock::now();
	while (m_shared->running.load() > 0)
	{
		if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 5000.0)
		{
			HE_LOG_WARN(Nav, "LandscapeNavigation: a tile build is still running after 5 s");
			break;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
}

void LandscapeNavigation::clear()
{
	quiesce();
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		for (Result& r : m_shared->done) dtFree(r.data);
		m_shared->done.clear();
	}
	m_token = CancelToken::create();
	m_entries.clear();
	m_query.reset();   // the query first: it points into the mesh
	m_mesh.reset();
	m_tileM = 0.0f;
	m_stats = LandscapeNavStats{};
}

bool LandscapeNavigation::ensureMesh(float tileM)
{
	if (m_mesh && m_tileM == tileM) return true;
	if (m_mesh) clear();
	dtNavMeshParams params{};
	params.orig[0] = params.orig[1] = params.orig[2] = 0.0f;
	params.tileWidth = params.tileHeight = tileM;
	params.maxTiles = kMaxTiles;
	params.maxPolys = kMaxPolysPerTile;
	dtNavMesh* raw = dtAllocNavMesh();
	if (!raw || raw->init(&params) != DT_SUCCESS)
	{
		HE_LOG_ERROR(Nav, "%s", "LandscapeNavigation: dtNavMesh::init failed");
		dtFreeNavMesh(raw);
		return false;
	}
	m_mesh.reset(raw, [](dtNavMesh* m) { dtFreeNavMesh(m); });
	dtNavMeshQuery* q = dtAllocNavMeshQuery();
	if (!q || q->init(raw, 2048) != DT_SUCCESS)
	{
		HE_LOG_ERROR(Nav, "%s", "LandscapeNavigation: dtNavMeshQuery::init failed");
		dtFreeNavMeshQuery(q);
		m_mesh.reset();
		return false;
	}
	m_query.reset(q, [](dtNavMeshQuery* x) { dtFreeNavMeshQuery(x); });
	m_tileM = tileM;
	return true;
}

void LandscapeNavigation::attach(NavMeshComponent& nmc) const
{
	if (!m_mesh || !m_query) return;
	nmc.navMesh = m_mesh;
	nmc.navQuery = m_query;
	nmc.isDirty = false;
}

bool LandscapeNavigation::tileBuilt(LandscapeTileKey tile) const
{
	const auto it = m_entries.find(pack(tile));
	return it != m_entries.end() && it->second.inMesh;
}

bool LandscapeNavigation::tileDone(LandscapeTileKey tile) const
{
	const auto it = m_entries.find(pack(tile));
	return it != m_entries.end() && it->second.done;
}

void LandscapeNavigation::removeTile(Entry& e, uint64_t)
{
	if (e.inMesh && m_mesh) m_mesh->removeTile(e.ref, nullptr, nullptr);
	e.inMesh = false;
	e.ref = 0;
}

void LandscapeNavigation::update(const LandscapeStore& store, const std::vector<glm::dvec3>& anchors, float heightMinM, float heightScaleM)
{
	const LandscapeTileSource& src = store.source();
	const double tileM = src.geometry().tileM;
	const int tx = src.tilesX(), tz = src.tilesZ();
	if (!ensureMesh(static_cast<float>(tileM))) return;

	// 1. what finished
	std::vector<Result> done;
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		done.swap(m_shared->done);
	}
	for (Result& r : done)
	{
		const auto it = m_entries.find(r.key);
		if (it == m_entries.end() || it->second.ticket != r.ticket || it->second.done)
		{
			if (r.status == LandscapeNavBuild::Ok) ++m_stats.discarded;
			dtFree(r.data);
			continue;
		}
		Entry& e = it->second;
		e.done = true;
		e.job = JobHandle();
		if (r.status == LandscapeNavBuild::Ok)
		{
			dtTileRef ref = 0;
			if (dtStatusFailed(m_mesh->addTile(r.data, r.size, DT_TILE_FREE_DATA, 0, &ref)))
			{
				// No entry left behind: a tile that is "done" without being in the mesh would stay a hole until the anchors left. Without an
				// entry it is wanted again, and step 3 waits for a free slot.
				HE_LOG_WARN(Nav, "LandscapeNavigation: the mesh took no tile (all %d slots in use?)", kMaxTiles);
				dtFree(r.data);
				++m_stats.failed;
				m_entries.erase(it);
				continue;
			}
			e.inMesh = true;
			e.ref = ref;
			e.polygons = r.polygons;
			++m_stats.finished;
		}
		else if (r.status == LandscapeNavBuild::Failed) ++m_stats.failed;
		else ++m_stats.finished;
	}

	// 2. who is wanted, who may stay
	const double reach = m_config.radius, keepReach = reach * m_config.keepFactor;
	std::unordered_set<uint64_t> keep;
	struct Need { double dist; LandscapeTileKey tile; };
	std::vector<Need> need;
	for (const glm::dvec3& a : anchors)
	{
		const int x0 = std::max(0, static_cast<int>(std::floor((a.x - keepReach) / tileM)));
		const int x1 = std::min(tx - 1, static_cast<int>(std::floor((a.x + keepReach) / tileM)));
		const int z0 = std::max(0, static_cast<int>(std::floor((a.z - keepReach) / tileM)));
		const int z1 = std::min(tz - 1, static_cast<int>(std::floor((a.z + keepReach) / tileM)));
		for (int z = z0; z <= z1; ++z)
			for (int x = x0; x <= x1; ++x)
			{
				const double d = distToSquare(a.x, a.z, x * tileM, z * tileM, (x + 1) * tileM, (z + 1) * tileM);
				if (d > keepReach) continue;
				keep.insert(pack({ x, z }));
				if (d <= reach && !m_entries.count(pack({ x, z }))) need.push_back({ d, { x, z } });
			}
	}
	for (auto it = m_entries.begin(); it != m_entries.end();)
	{
		if (keep.count(it->first)) { ++it; continue; }
		if (!it->second.done) it->second.job.cancel();
		else removeTile(it->second, it->first);
		++m_stats.dropped;
		it = m_entries.erase(it);
	}

	// 3. start the nearest builds whose tile and neighbours are in memory
	std::sort(need.begin(), need.end(), [](const Need& a, const Need& b) {
		if (a.dist != b.dist) return a.dist < b.dist;
		return a.tile.z != b.tile.z ? a.tile.z < b.tile.z : a.tile.x < b.tile.x;
	});
	need.erase(std::unique(need.begin(), need.end(), [](const Need& a, const Need& b) { return a.tile == b.tile; }), need.end());
	size_t pending = 0, inMesh = 0;
	for (const auto& [k, e] : m_entries)
	{
		pending += e.done ? 0 : 1;
		inMesh += e.inMesh ? 1 : 0;
	}
	size_t starts = 0;
	for (const Need& n : need)
	{
		if (starts >= m_config.maxStartsPerUpdate || pending >= m_config.maxInFlight) break;
		if (inMesh + pending >= static_cast<size_t>(kMaxTiles)) break;   // every slot is taken or promised: wait for one to come free
		std::vector<std::shared_ptr<const LandscapeTile>> nb(9);
		bool ready = true;
		for (int dz = -1; dz <= 1 && ready; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const int x = n.tile.x + dx, z = n.tile.z + dz;
				if (x < 0 || z < 0 || x >= tx || z >= tz) continue;   // outside the world: the border repeats the edge
				nb[static_cast<size_t>(dz + 1) * 3 + (dx + 1)] = store.tile({ x, z });
				if (!nb[static_cast<size_t>(dz + 1) * 3 + (dx + 1)]) { ready = false; break; }
			}
		if (!ready) { ++m_stats.deferred; continue; }

		Entry e;
		e.ticket = m_nextTicket++;
		e.cancel = m_token.child();
		const uint64_t key = pack(n.tile);
		const uint64_t ticket = e.ticket;
		const LandscapeTileKey tile = n.tile;
		const NavMeshConfig cfg = m_config.nav;
		std::shared_ptr<Shared> shared = m_shared;

		JobDesc desc;
		desc.name = "LandscapeNavTile";
		desc.priority = JobPriority::Low;
		desc.cancel = e.cancel;
		e.job = m_pool->scheduleFn([shared, nb = std::move(nb), tile, tx, tz, heightMinM, heightScaleM, cfg, key, ticket]() mutable {
			shared->running.fetch_add(1);
			Result r;
			r.key = key;
			r.ticket = ticket;
			if (!shared->closed.load())
			{
				const auto tileAt = [&](LandscapeTileKey k) -> std::shared_ptr<const LandscapeTile> {
					const int dx = k.x - tile.x, dz = k.z - tile.z;
					return (dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1) ? nb[static_cast<size_t>(dz + 1) * 3 + (dx + 1)] : nullptr;
				};
				r.status = buildLandscapeNavTile(tileAt, tile, tx, tz, heightMinM, heightScaleM, cfg, r.data, r.size, r.polygons);
			}
			nb.clear();   // the tiles are the store's; not held past the build
			{
				std::lock_guard<std::mutex> lock(shared->mutex);
				shared->done.push_back(r);
			}
			shared->running.fetch_sub(1);
		}, std::move(desc));
		m_entries.emplace(key, std::move(e));
		++m_stats.started;
		++starts;
		++pending;
	}

	m_stats.tiles = m_stats.empty = m_stats.pending = m_stats.polygons = 0;
	for (const auto& [k, e] : m_entries)
	{
		if (!e.done) ++m_stats.pending;
		else if (e.inMesh) { ++m_stats.tiles; m_stats.polygons += static_cast<size_t>(e.polygons); }
		else ++m_stats.empty;
	}
}

} // namespace HE
