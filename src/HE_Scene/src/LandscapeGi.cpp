#include "HorizonScene/LandscapeGi.h"

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

LandscapeGi::LandscapeGi(LandscapeGiConfig config, ThreadPool* pool)
    : m_config(config), m_pool(pool ? pool : &globalPool()), m_shared(std::make_shared<Shared>()), m_token(CancelToken::create())
{
}

LandscapeGi::~LandscapeGi()
{
	m_shared->closed.store(true);
	quiesce();
}

void LandscapeGi::quiesce()
{
	m_token.cancel();
	m_job.cancel();
	const auto t0 = std::chrono::steady_clock::now();
	while (m_shared->running.load() > 0)
	{
		if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 5000.0)
		{
			HE_LOG_WARN(World, "LandscapeGi: a clipmap build is still running after 5 s");
			break;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
}

void LandscapeGi::clear()
{
	quiesce();
	m_token = CancelToken::create();
	m_job = JobHandle();
	m_hf = LandscapeGiHeightfield{};
	m_lastOrigin = { -1, -1 };
	m_lastN = 0;
	m_building = false;
	m_wantOrigin = { -1, -1 };
	m_wantN = 0;
	std::lock_guard<std::mutex> lock(m_shared->mutex);
	m_shared->done.clear();
}

void LandscapeGi::update(const LandscapeStore& store, const glm::dvec3& camera, const LandscapeTileTerrainParams& params)
{
	// 1. a finished clipmap replaces the current one, if it is still the one asked for
	std::vector<Result> done;
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		done.swap(m_shared->done);
	}
	for (Result& r : done)
	{
		if (m_building && r.ticket == m_ticket)
		{
			m_building = false;   // the build the clipmap in flight was waiting for is over, whatever it brought
			if (r.ok)
			{
				m_hf = std::move(r.hf);
				m_lastOrigin = m_wantOrigin;
				m_lastN = m_wantN;
				++m_stats.finished;
			}
			// A build that could not make a terrain of its tiles leaves the old clipmap (one frame
			// behind rather than dark); m_wantOrigin already names it, so it is not retried every frame.
		}
		else ++m_stats.discarded;
	}
	m_stats.building = m_building;

	const LandscapeTileSource& src = store.source();
	const double tileM = src.geometry().tileM;
	// A TerrainComponent holds kTerrainMaxResolution samples a side: the window cannot have more tiles than they cover.
	const int fits = static_cast<int>((kTerrainMaxResolution - 1) / std::max<uint32_t>(1, src.geometry().hCells));
	const int n = std::min({ m_config.tilesPerSide, src.tilesX(), src.tilesZ(), fits });
	if (n < 1) return;
	const int cx = clampi(static_cast<int>(std::floor(camera.x / tileM)), 0, src.tilesX() - 1);
	const int cz = clampi(static_cast<int>(std::floor(camera.z / tileM)), 0, src.tilesZ() - 1);
	const LandscapeTileKey desired{ clampi(cx - n / 2, 0, src.tilesX() - n), clampi(cz - n / 2, 0, src.tilesZ() - n) };

	// 2. where the window should be: where it is, until the camera leaves its inner tiles (half a
	// kilometre of play either way at n = 4) — the same hysteresis LandscapeWater uses for its region.
	const bool have = m_building || m_hf.active();
	const LandscapeTileKey current = m_building ? m_wantOrigin : (m_hf.active() ? m_lastOrigin : LandscapeTileKey{ -1, -1 });
	const int currentN = m_building ? m_wantN : (m_hf.active() ? m_lastN : 0);
	bool inner = false;
	if (have && currentN == n && n >= 3)
		inner = cx >= current.x + 1 && cx <= current.x + n - 2 && cz >= current.z + 1 && cz <= current.z + n - 2;
	LandscapeTileKey target = desired;
	if (have && currentN == n && (inner || desired == current)) target = current;
	if (have && target == current && currentN == n) return;   // nothing to do: the clipmap (built or in flight) is the right one

	// 3. start the build for `target`, if every tile of it is in memory
	std::vector<std::shared_ptr<const LandscapeTile>> tiles(static_cast<size_t>(n) * n);
	for (int j = 0; j < n; ++j)
		for (int i = 0; i < n; ++i)
		{
			tiles[static_cast<size_t>(j) * n + i] = store.tile({ target.x + i, target.z + j });
			if (!tiles[static_cast<size_t>(j) * n + i]) { ++m_stats.deferred; return; }
		}

	m_job.cancel();   // a build for an older window that has not started never does
	m_ticket = m_nextVersion++;
	m_wantOrigin = target;
	m_wantN = n;
	m_building = true;
	m_stats.building = true;
	++m_stats.started;

	const uint64_t ticket = m_ticket;
	const uint64_t version = ticket;
	std::shared_ptr<Shared> shared = m_shared;
	JobDesc desc;
	desc.name = "LandscapeGiHeightfield";
	desc.priority = JobPriority::Low;
	desc.cancel = m_token.child();
	m_job = m_pool->scheduleFn([shared, tiles, params, target, n, ticket, version]() mutable {
		shared->running.fetch_add(1);
		Result out; out.ticket = ticket;
		if (!shared->closed.load())
		{
			LandscapeRegionTerrain rt;
			const auto tileAt = [&](LandscapeTileKey k) -> std::shared_ptr<const LandscapeTile> {
				const int i = k.x - target.x, j = k.z - target.z;
				return (i >= 0 && j >= 0 && i < n && j < n) ? tiles[static_cast<size_t>(j) * n + i] : nullptr;
			};
			if (landscapeRegionTerrain(tileAt, target, n, params, nullptr, rt))
			{
				LandscapeGiHeightfield hf;
				hf.res = rt.terrain.resolution;
				hf.data = std::make_shared<const std::vector<float>>(std::move(rt.terrain.sculptHeights));
				hf.version = version;
				hf.centreAbsolute = rt.centre;
				hf.sizeX = rt.terrain.sizeX;
				hf.sizeZ = rt.terrain.sizeZ;
				out.hf = std::move(hf);
				out.ok = true;
			}
		}
		tiles.clear();   // the clipmap keeps only the flattened heights; the tiles are the store's
		{
			std::lock_guard<std::mutex> lock(shared->mutex);
			shared->done.push_back(std::move(out));
		}
		shared->running.fetch_sub(1);
	}, std::move(desc));
}

} // namespace HE
