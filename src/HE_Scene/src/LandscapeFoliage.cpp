#include "HorizonScene/LandscapeFoliage.h"

#include "HorizonScene/FoliageSystem.h"
#include "HorizonScene/LandscapeTileTerrain.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

uint32_t hash32(uint32_t n)
{
	n = (n ^ 61u) ^ (n >> 16);
	n *= 9u;
	n ^= n >> 4;
	n *= 0x27d4eb2du;
	n ^= n >> 15;
	return n;
}

// The layer's seed, made the tile's own: the same tile gives the same plants whenever it is scattered, and neighbours differ.
int tileSeed(int layerSeed, LandscapeTileKey k)
{
	return static_cast<int>(static_cast<uint32_t>(layerSeed) ^ hash32(static_cast<uint32_t>(k.x) * 73856093u ^ static_cast<uint32_t>(k.z) * 19349663u));
}
} // namespace

LandscapeFoliage::LandscapeFoliage(LandscapeFoliageConfig config, ThreadPool* pool)
    : m_config(config), m_pool(pool ? pool : &globalPool()), m_shared(std::make_shared<Shared>()), m_token(CancelToken::create())
{
}

LandscapeFoliage::~LandscapeFoliage()
{
	m_shared->closed.store(true);
	quiesce();
}

void LandscapeFoliage::quiesce()
{
	m_token.cancel();
	for (auto& [k, e] : m_entries)
		if (!e.done) e.job.cancel();
	const auto t0 = std::chrono::steady_clock::now();
	while (m_shared->running.load() > 0)
	{
		if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > 5000.0)
		{
			HE_LOG_WARN(Foliage, "LandscapeFoliage: a scatter is still running after 5 s");
			break;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
}

void LandscapeFoliage::clear()
{
	quiesce();
	m_token = CancelToken::create();
	m_entries.clear();
	m_stats.scattered = m_stats.pending = m_stats.instances = 0;
}

void LandscapeFoliage::setLayers(const std::vector<FoliageLayer>& layers)
{
	clear();
	m_layers.clear();
	for (const FoliageLayer& l : layers)
	{
		FoliageLayer c = l;
		c.cachedInstances.clear();   // the runtime state of a layer is not ours
		c.store.reset();
		c.densityMask.clear();       // a mask over a 100 km landscape means nothing (see the header)
		m_layers.push_back(std::move(c));
	}
	++m_revision;
}

std::shared_ptr<const FoliageStore> LandscapeFoliage::storeOf(LandscapeTileKey tile, size_t layer) const
{
	const auto it = m_entries.find(Key{ pack(tile), static_cast<uint32_t>(layer) });
	return (it != m_entries.end() && it->second.done) ? it->second.store : nullptr;
}

void LandscapeFoliage::draws(std::vector<LandscapeFoliageDraw>& out) const
{
	out.clear();
	for (const auto& [k, e] : m_entries)
	{
		if (!e.done || !e.store || e.store->buckets.empty()) continue;
		out.push_back({ e.tile, e.centre, k.layer, e.store });
	}
}

void LandscapeFoliage::update(const LandscapeStore& store, const std::vector<glm::dvec3>& anchors, float heightMinM, float heightScaleM)
{
	// 1. what finished
	std::vector<Result> done;
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		done.swap(m_shared->done);
	}
	for (Result& r : done)
	{
		const auto it = m_entries.find(r.key);
		if (it == m_entries.end() || it->second.ticket != r.ticket || it->second.done) { if (r.store) ++m_stats.discarded; continue; }
		it->second.done = true;
		it->second.store = std::move(r.store);
		it->second.instances = r.instances;
		it->second.job = JobHandle();
		++m_stats.finished;
	}

	const LandscapeTileSource& src = store.source();
	const double tileM = src.geometry().tileM;
	const int tx = src.tilesX(), tz = src.tilesZ();

	// 2. who is wanted, who may stay
	std::unordered_set<Key, KeyHash> keep;
	struct Need { double dist; LandscapeTileKey tile; uint32_t layer; };
	std::vector<Need> need;
	for (size_t li = 0; li < m_layers.size(); ++li)
	{
		const FoliageLayer& l = m_layers[li];
		if (!l.visible || l.meshAssetId == HE::UUID{}) continue;
		const double reach = static_cast<double>(l.drawDistance + m_config.margin), keepReach = reach * m_config.keepFactor;
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
					const Key key{ pack({ x, z }), static_cast<uint32_t>(li) };
					keep.insert(key);
					if (d <= reach && !m_entries.count(key)) need.push_back({ d, { x, z }, static_cast<uint32_t>(li) });
				}
		}
	}
	for (auto it = m_entries.begin(); it != m_entries.end();)
	{
		if (keep.count(it->first)) { ++it; continue; }
		if (!it->second.done) it->second.job.cancel();
		++m_stats.dropped;
		it = m_entries.erase(it);
	}

	// 3. start the nearest scatters whose tile is in memory
	std::sort(need.begin(), need.end(), [](const Need& a, const Need& b) {
		if (a.dist != b.dist) return a.dist < b.dist;
		if (a.layer != b.layer) return a.layer < b.layer;
		return a.tile.z != b.tile.z ? a.tile.z < b.tile.z : a.tile.x < b.tile.x;
	});
	need.erase(std::unique(need.begin(), need.end(), [](const Need& a, const Need& b) { return a.layer == b.layer && a.tile == b.tile; }), need.end());
	size_t pending = 0;
	for (const auto& [k, e] : m_entries) pending += e.done ? 0 : 1;
	size_t starts = 0;
	for (const Need& n : need)
	{
		if (starts >= m_config.maxStartsPerUpdate || pending >= m_config.maxInFlight) break;
		const std::shared_ptr<const LandscapeTile> tile = store.tile(n.tile);
		if (!tile) continue;   // not in memory yet: next time
		Entry e;
		e.tile = n.tile;
		e.centre = landscapeTileCentre(n.tile, tile->geometry);
		e.ticket = m_nextTicket++;
		e.cancel = m_token.child();
		FoliageLayer layer = m_layers[n.layer];
		layer.seed = tileSeed(layer.seed, n.tile);
		const Key key{ pack(n.tile), n.layer };
		const uint64_t ticket = e.ticket;
		const LandscapeTileTerrainParams params{ heightMinM, heightScaleM };
		std::shared_ptr<Shared> shared = m_shared;

		JobDesc desc;
		desc.name = "LandscapeFoliageScatter";
		desc.priority = JobPriority::Low;
		desc.cancel = e.cancel;
		e.job = m_pool->scheduleFn([shared, tile = std::shared_ptr<const LandscapeTile>(tile), layer, params, key, ticket]() mutable {
			shared->running.fetch_add(1);
			std::shared_ptr<const FoliageStore> result;
			if (!shared->closed.load())
			{
				const TerrainComponent tc = landscapeTileTerrain(*tile, params);
				if (tc.sizeX > 0.0f)
				{
					FoliageSystem::scatterLayer(tc, glm::vec3(0.0f), layer);
					result = layer.store;
				}
			}
			layer = FoliageLayer{};   // the scatter's world-space copy of every instance is not read by anyone: not held past the job
			tile.reset();
			const size_t n = result ? result->local.size() : 0;
			{
				std::lock_guard<std::mutex> lock(shared->mutex);
				shared->done.push_back(Result{ key, ticket, std::move(result), n });
			}
			shared->running.fetch_sub(1);
		}, std::move(desc));
		m_entries.emplace(key, std::move(e));
		++m_stats.started;
		++starts;
		++pending;
	}

	m_stats.scattered = 0;
	m_stats.pending = 0;
	m_stats.instances = 0;
	for (const auto& [k, e] : m_entries)
	{
		if (e.done) { ++m_stats.scattered; m_stats.instances += e.instances; }
		else ++m_stats.pending;
	}
}

} // namespace HE
