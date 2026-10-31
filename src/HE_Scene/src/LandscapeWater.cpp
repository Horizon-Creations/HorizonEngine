#include "HorizonScene/LandscapeWater.h"

#include "HorizonScene/TerrainMeshGenerator.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace HE
{
namespace
{
int clampi(int v, int lo, int hi) { return std::max(lo, std::min(v, hi)); }
} // namespace

LandscapeWater::LandscapeWater(LandscapeWaterConfig config, ThreadPool* pool)
    : m_config(config), m_pool(pool ? pool : &globalPool()), m_shared(std::make_shared<Shared>()), m_token(CancelToken::create())
{
}

LandscapeWater::~LandscapeWater()
{
	m_shared->closed.store(true);
	quiesce();
}

void LandscapeWater::quiesce()
{
	m_token.cancel();
	m_job.cancel();
	const auto t0 = std::chrono::steady_clock::now();
	while (m_shared->running.load() > 0)
	{
		if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 5000.0)
		{
			HE_LOG_WARN(World, "LandscapeWater: a region build is still running after 5 s");
			break;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
}

void LandscapeWater::clear()
{
	quiesce();
	m_token = CancelToken::create();
	m_job = JobHandle();
	m_region.reset();
	m_building = false;
	m_wantOrigin = { -1, -1 };
	std::lock_guard<std::mutex> lock(m_shared->mutex);
	m_shared->done.clear();
}

void LandscapeWater::update(const LandscapeStore& store, const glm::dvec3& camera, const LandscapeTileTerrainParams& params, const LandscapeWaterTable* table)
{
	// 1. a finished region replaces the current one, if it is still the one asked for
	std::vector<Result> done;
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		done.swap(m_shared->done);
	}
	for (Result& r : done)
	{
		if (m_building && r.ticket == m_ticket)
		{
			m_building = false;   // the build the region in flight was waiting for is over, whatever it brought
			if (r.region) { m_region = std::move(r.region); ++m_stats.finished; }
			else
			{
				// It could not make a terrain of its tiles: the old region stays, and this one is asked for again only when the camera moves
				// to another (m_wantOrigin keeps the failed one so the same request is not repeated every frame).
				++m_stats.failed;
				m_failedOrigin = m_wantOrigin;
				m_hasFailed = true;
			}
		}
		else ++m_stats.discarded;
	}
	m_stats.building = m_building;

	if (!table || table->bodies.empty())
	{
		if (m_region) m_region.reset();   // the world has no water (any more)
		return;
	}

	const LandscapeTileSource& src = store.source();
	const double tileM = src.geometry().tileM;
	// A TerrainComponent holds kTerrainMaxResolution samples a side: the region cannot have more tiles than they cover.
	const int fits = static_cast<int>((kTerrainMaxResolution - 1) / std::max<uint32_t>(1, src.geometry().hCells));
	const int n = std::min({ m_config.tilesPerSide, src.tilesX(), src.tilesZ(), fits });
	if (n < m_config.tilesPerSide && n == fits && !m_warnedSize)
	{
		HE_LOG_WARN(World, "LandscapeWater: %d tiles a side do not fit a terrain of %u samples; the region has %d", m_config.tilesPerSide, kTerrainMaxResolution, n);
		m_warnedSize = true;
	}
	if (n < 1) return;
	const int cx = clampi(static_cast<int>(std::floor(camera.x / tileM)), 0, src.tilesX() - 1);
	const int cz = clampi(static_cast<int>(std::floor(camera.z / tileM)), 0, src.tilesZ() - 1);
	const LandscapeTileKey desired{ clampi(cx - n / 2, 0, src.tilesX() - n), clampi(cz - n / 2, 0, src.tilesZ() - n) };

	// 2. where the region should be: where it is, until the camera leaves its inner tiles
	const bool have = m_building || m_region;
	const LandscapeTileKey current = m_building ? m_wantOrigin : (m_region ? m_region->origin : LandscapeTileKey{ -1, -1 });
	const int currentN = m_building ? n : (m_region ? m_region->n : 0);
	bool inner = false;
	if (have && currentN == n && n >= 3)
		inner = cx >= current.x + 1 && cx <= current.x + n - 2 && cz >= current.z + 1 && cz <= current.z + n - 2;
	LandscapeTileKey target = desired;
	if (have && currentN == n && (inner || desired == current)) target = current;
	if (have && target == current && currentN == n) return;   // nothing to do: the region (built or in flight) is the right one

	if (m_hasFailed && target == m_failedOrigin) return;     // that request failed; the camera has to move on before it is tried again
	m_hasFailed = m_hasFailed && target == m_failedOrigin;

	// 3. start the build for `target`, if every tile of it is in memory
	std::vector<std::shared_ptr<const LandscapeTile>> tiles(static_cast<size_t>(n) * n);
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i)
		{
			tiles[static_cast<size_t>(j) * n + i] = store.tile({ target.x + i, target.z + j });
			if (!tiles[static_cast<size_t>(j) * n + i]) { ++m_stats.deferred; return; }
		}

	m_job.cancel();   // a build for an older region that has not started never does
	m_ticket = m_nextRevision++;
	m_wantOrigin = target;
	m_building = true;
	m_stats.building = true;
	++m_stats.started;

	const uint64_t ticket = m_ticket;
	const LandscapeWaterTable tableCopy = *table;
	const float uvMeters = m_config.uvMetersPerTile;
	std::shared_ptr<Shared> shared = m_shared;
	JobDesc desc;
	desc.name = "LandscapeWaterRegion";
	desc.priority = JobPriority::Low;
	desc.cancel = m_token.child();
	m_job = m_pool->scheduleFn([shared, tiles, tableCopy, params, target, n, ticket, uvMeters]() mutable {
		shared->running.fetch_add(1);
		std::shared_ptr<const LandscapeWaterRegion> result;
		if (!shared->closed.load())
		{
			LandscapeRegionTerrain rt;
			const auto tileAt = [&](LandscapeTileKey k) -> std::shared_ptr<const LandscapeTile> {
				const int i = k.x - target.x, j = k.z - target.z;
				return (i >= 0 && j >= 0 && i < n && j < n) ? tiles[static_cast<size_t>(j) * n + i] : nullptr;
			};
			if (landscapeRegionTerrain(tileAt, target, n, params, &tableCopy, rt))
			{
				auto region = std::make_shared<LandscapeWaterRegion>();
				region->origin = target;
				region->n = n;
				region->centre = rt.centre;
				region->table = tableCopy;
				region->clipToGround = rt.terrain.water.clipToGround;
				region->shoreOvershoot = rt.terrain.water.shoreOvershoot;
				region->revision = ticket;
				water::SurfaceOptions opt;
				opt.uvOrigin = glm::vec2(static_cast<float>(rt.centre.x), static_cast<float>(rt.centre.z));
				opt.uvMetersPerTile = uvMeters;
				opt.clipToGround = rt.terrain.water.clipToGround;     // SurfaceOptions leaves it off: the Field says whether the world clips
				opt.shoreOvershoot = rt.terrain.water.shoreOvershoot;
				if (rt.hasWater)
					for (const water::Body& b : rt.terrain.water.bodies)
					{
						water::SurfaceMesh sm;
						if (!water::buildSurface(rt.terrain, b.id, opt, sm) || sm.empty()) continue;
						LandscapeWaterSheet sheet;
						sheet.body = b.id;
						sheet.level = b.level;
						sheet.surface = std::move(sm);
						if (const LandscapeWaterBody* wb = tableCopy.find(b.id)) sheet.params = *wb;
						region->sheets.push_back(std::move(sheet));
					}
				region->terrain = std::make_shared<const TerrainComponent>(std::move(rt.terrain));
				result = std::move(region);
			}
		}
		tiles.clear();   // the region keeps what it needs; the tiles are the store's and not held past the build
		{
			std::lock_guard<std::mutex> lock(shared->mutex);
			shared->done.push_back(Result{ ticket, std::move(result) });
		}
		shared->running.fetch_sub(1);
	}, std::move(desc));
}

bool LandscapeWater::surfaceAt(double x, double z, LandscapeWaterQuery& out) const
{
	out = LandscapeWaterQuery{};
	const std::shared_ptr<const LandscapeWaterRegion> r = m_region;
	if (!r || !r->terrain) return false;
	const TerrainComponent& tc = *r->terrain;
	const double lx = x - r->centre.x, lz = z - r->centre.z;
	if (std::abs(lx) > tc.sizeX * 0.5 || std::abs(lz) > tc.sizeZ * 0.5) return false;
	float level = 0.0f;
	if (!water::levelAt(tc, static_cast<float>(lx), static_cast<float>(lz), level)) return false;
	const float ground = terrainHeightAt(tc, static_cast<float>(lx), static_cast<float>(lz));
	if (r->clipToGround && ground > level + r->shoreOvershoot) return false;
	out.wet = true;
	out.body = water::bodyAt(tc, static_cast<float>(lx), static_cast<float>(lz));
	out.level = level;
	out.depth = std::max(0.0f, level - ground);
	if (const LandscapeWaterBody* b = r->table.find(out.body)) out.params = *b;
	return true;
}

} // namespace HE
