#include "HorizonScene/LandscapeStore.h"

#include <Diagnostics/Log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace HE
{
namespace
{
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// Ground-plane distance from (px, pz) to the square [x0, x1] x [z0, z1], 0 inside it.
double distToSquare(double px, double pz, double x0, double z0, double x1, double z1)
{
	const double dx = std::max({ x0 - px, 0.0, px - x1 });
	const double dz = std::max({ z0 - pz, 0.0, pz - z1 });
	return std::sqrt(dx * dx + dz * dz);
}
} // namespace

LandscapeStore::LandscapeStore(std::shared_ptr<const LandscapeTileSource> source, LandscapeStoreConfig config, ThreadPool* pool)
    : m_source(std::move(source)), m_pool(pool ? pool : &globalPool()), m_shared(std::make_shared<Shared>()),
      m_storeToken(CancelToken::create())
{
	setConfig(config);
}

LandscapeStore::~LandscapeStore()
{
	// A load body that starts after this point does not touch the source; the ones running let go of it before the destructor returns.
	// So when a store is gone nothing of it holds the tile source, and a pack file can be deleted (Windows cannot delete an open file).
	m_shared->closed.store(true);
	quiesce();
}

// Cancels the jobs that have not started (the token is only looked at when a worker dequeues a job, a handle finishes it at once) and
// waits for the loads that are running. They are short (one tile), but a source that blocks would block this: it gives up after 5 s.
void LandscapeStore::quiesce()
{
	m_storeToken.cancel();
	for (auto& [k, e] : m_tiles)
		if (e.state == Entry::State::Loading) e.job.cancel();
	const auto t0 = Clock::now();
	while (m_shared->running.load() > 0)
	{
		if (msSince(t0) > 5000.0)
		{
			HE_LOG_WARN(World, "LandscapeStore: a tile load is still running after 5 s; the source stays open");
			break;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
}

void LandscapeStore::setConfig(const LandscapeStoreConfig& config)
{
	m_config = config;
	m_config.keepFactor = std::max(1.0f, m_config.keepFactor);
	m_config.loadRadius = std::max(0.0f, m_config.loadRadius);
	const LandscapeGeometry& g = m_source->geometry();
	m_tileBytes = std::max<size_t>(1, g.decodedBytes(m_config.maxLevels));
	// A world with water carries two more rasters per tile (coverage u8, owner u16, at the weights' level 0): three bytes a texel.
	if (m_source->water() && m_config.maxLevels != 0) m_tileBytes += static_cast<size_t>(g.weightDim(0)) * g.weightDim(0) * 3;
	m_maxTiles  = m_config.byteBudget / m_tileBytes;
	m_stats.maxTiles = m_maxTiles;
	m_dirty = true;
}

void LandscapeStore::dropEntry(std::unordered_map<LandscapeTileKey, Entry, LandscapeTileKeyHash>::iterator it)
{
	switch (it->second.state)
	{
	case Entry::State::Loading:  --m_loading; break;
	case Entry::State::Resident: --m_resident; m_residentBytes -= it->second.tile ? it->second.tile->bytes() : 0; break;
	case Entry::State::Failed:   --m_failed; break;
	}
	m_tiles.erase(it);
}

void LandscapeStore::clear()
{
	quiesce();
	m_storeToken = CancelToken::create();
	// A body that slipped past the cancel check finished by now; anything still arriving carries a ticket nobody holds.
	m_tiles.clear();
	m_keep.clear();
	m_wanted.clear();
	m_resident = m_residentBytes = m_loading = m_failed = 0;
	m_havePlan = false;
	m_dirty = true;
}

LandscapeStore::TilePtr LandscapeStore::tile(LandscapeTileKey key) const
{
	const auto it = m_tiles.find(key);
	return (it != m_tiles.end() && it->second.state == Entry::State::Resident) ? it->second.tile : nullptr;
}

bool LandscapeStore::isResident(LandscapeTileKey key) const
{
	const auto it = m_tiles.find(key);
	return it != m_tiles.end() && it->second.state == Entry::State::Resident;
}

bool LandscapeStore::isLoading(LandscapeTileKey key) const
{
	const auto it = m_tiles.find(key);
	return it != m_tiles.end() && it->second.state == Entry::State::Loading;
}

std::vector<LandscapeTileKey> LandscapeStore::residentKeys() const
{
	std::vector<LandscapeTileKey> out;
	out.reserve(m_resident);
	for (const auto& [k, e] : m_tiles)
		if (e.state == Entry::State::Resident) out.push_back(k);
	return out;
}

bool LandscapeStore::settled() const
{
	if (!m_havePlan || m_dirty || m_loading != 0) return false;   // before the first update nothing has been planned
	for (const auto& k : m_wanted)
		if (m_tiles.find(k) == m_tiles.end()) return false;
	return true;
}

// ── update ───────────────────────────────────────────────────────────────────
void LandscapeStore::update(const std::vector<LandscapeAnchor>& anchors, double budgetMs)
{
	const auto t0 = Clock::now();
	drain(budgetMs);
	if (needsPlan(anchors)) plan(anchors);
	pump();

	m_stats.resident = m_resident;
	m_stats.loading = m_loading;
	m_stats.failed = m_failed;
	m_stats.residentBytes = m_residentBytes;
	m_stats.peakBytes = std::max(m_stats.peakBytes, m_residentBytes + m_loading * m_tileBytes);
	m_stats.lastUpdateMs = msSince(t0);
	m_stats.maxUpdateMs = std::max(m_stats.maxUpdateMs, m_stats.lastUpdateMs);
	++m_stats.updates;
	m_stats.avgUpdateMs += (m_stats.lastUpdateMs - m_stats.avgUpdateMs) / static_cast<double>(m_stats.updates);
}

bool LandscapeStore::needsPlan(const std::vector<LandscapeAnchor>& anchors) const
{
	if (m_dirty || !m_havePlan || anchors.size() != m_planAnchors.size()) return true;
	const double limit = m_config.replanTiles * m_source->geometry().tileM;
	for (size_t i = 0; i < anchors.size(); ++i)
	{
		const LandscapeAnchor& a = anchors[i];
		const LandscapeAnchor& b = m_planAnchors[i];
		if (a.radiusScale != b.radiusScale) return true;
		const glm::dvec3 ahead  = a.position + glm::dvec3(a.velocity) * static_cast<double>(m_config.lookaheadSec);
		const glm::dvec3 aheadB = b.position + glm::dvec3(b.velocity) * static_cast<double>(m_config.lookaheadSec);
		if (std::hypot(a.position.x - b.position.x, a.position.z - b.position.z) >= limit) return true;
		if (std::hypot(ahead.x - aheadB.x, ahead.z - aheadB.z) >= limit) return true;
	}
	return false;
}

void LandscapeStore::plan(const std::vector<LandscapeAnchor>& anchors)
{
	++m_stats.plans;
	const LandscapeGeometry& g = m_source->geometry();
	const double tileM = g.tileM;
	const double R  = m_config.loadRadius;
	const double Rk = R * m_config.keepFactor;
	const int tx = m_source->tilesX(), tz = m_source->tilesZ();

	// Distance of every tile within the kept ring to its nearest anchor (scaled by that anchor's radiusScale).
	std::unordered_map<LandscapeTileKey, double, LandscapeTileKeyHash> cand;
	for (const LandscapeAnchor& a : anchors)
	{
		const double s = a.radiusScale > 0.0f ? a.radiusScale : 1.0;
		const double reach = Rk * s;
		const glm::dvec3 pts[2] = { a.position, a.position + glm::dvec3(a.velocity) * static_cast<double>(m_config.lookaheadSec) };
		for (const glm::dvec3& p : pts)
		{
			const int x0 = std::max(0, static_cast<int>(std::floor((p.x - reach) / tileM)));
			const int x1 = std::min(tx - 1, static_cast<int>(std::floor((p.x + reach) / tileM)));
			const int z0 = std::max(0, static_cast<int>(std::floor((p.z - reach) / tileM)));
			const int z1 = std::min(tz - 1, static_cast<int>(std::floor((p.z + reach) / tileM)));
			for (int z = z0; z <= z1; ++z)
				for (int x = x0; x <= x1; ++x)
				{
					const double d = distToSquare(p.x, p.z, x * tileM, z * tileM, (x + 1) * tileM, (z + 1) * tileM) / s;
					if (d > Rk) continue;
					const auto [it, inserted] = cand.emplace(LandscapeTileKey{ x, z }, d);
					if (!inserted && d < it->second) it->second = d;
				}
		}
	}

	m_keep.clear();
	std::vector<std::pair<double, LandscapeTileKey>> wanted;
	for (const auto& [k, d] : cand)
	{
		m_keep.insert(k);
		if (d <= R) wanted.emplace_back(d, k);
	}
	std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) {
		if (a.first != b.first) return a.first < b.first;
		return a.second.z != b.second.z ? a.second.z < b.second.z : a.second.x < b.second.x;
	});
	const bool wasClamped = m_stats.budgetClamped;
	m_stats.budgetClamped = wanted.size() > m_maxTiles;
	if (m_stats.budgetClamped)
	{
		if (!wasClamped)
			HE_LOG_WARN(World, "LandscapeStore: %zu tiles wanted within %.0f m but the budget holds %zu; the effective radius shrinks",
			            wanted.size(), R, m_maxTiles);
		wanted.resize(m_maxTiles);
	}
	m_wanted.clear();
	m_wanted.reserve(wanted.size());
	for (const auto& w : wanted) m_wanted.push_back(w.second);
	m_stats.wanted = m_wanted.size();

	// Entries that left the kept ring go; the rest learn their distance.
	for (auto it = m_tiles.begin(); it != m_tiles.end();)
	{
		const auto c = cand.find(it->first);
		if (c != cand.end())
		{
			it->second.dist = c->second;
			it->second.unwanted = false;
			++it;
			continue;
		}
		Entry& e = it->second;
		if (e.state == Entry::State::Loading)
		{
			e.job.cancel();                       // finishes a job that has not started, at once
			if (e.job.status() != JobStatus::Cancelled)
			{
				e.unwanted = true;                // running: let it finish, its tile is thrown away
				++it;
				continue;
			}
		}
		else if (e.state == Entry::State::Resident)
			++m_stats.evictions;
		const auto victim = it++;
		dropEntry(victim);
	}
	m_planAnchors = anchors;
	m_havePlan = true;
	m_dirty = false;
}

void LandscapeStore::pump()
{
	if (m_wanted.empty()) return;
	// Missing wanted tiles, nearest first.
	std::vector<LandscapeTileKey> need;
	for (const auto& k : m_wanted)
	{
		if (m_tiles.find(k) == m_tiles.end()) need.push_back(k);
		if (need.size() >= m_config.maxInFlight) break;
	}
	if (need.empty() || m_loading >= m_config.maxInFlight) return;
	size_t want = std::min(need.size(), m_config.maxInFlight - m_loading);

	// Room: free slots first, then the farthest resident tiles that are kept but no longer wanted.
	const size_t used = slotsUsed();
	size_t freeSlots = m_maxTiles > used ? m_maxTiles - used : 0;
	if (freeSlots < want)
	{
		std::vector<std::pair<double, LandscapeTileKey>> evictable;
		std::unordered_set<LandscapeTileKey, LandscapeTileKeyHash> wantedSet(m_wanted.begin(), m_wanted.end());
		for (const auto& [k, e] : m_tiles)
			if (e.state == Entry::State::Resident && !wantedSet.count(k)) evictable.emplace_back(e.dist, k);
		std::sort(evictable.begin(), evictable.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		for (const auto& [d, k] : evictable)
		{
			if (freeSlots >= want) break;
			dropEntry(m_tiles.find(k));
			++m_stats.evictions;
			++freeSlots;
		}
	}
	want = std::min(want, freeSlots);

	for (size_t i = 0; i < want; ++i)
	{
		const LandscapeTileKey key = need[i];
		Entry e;
		e.state  = Entry::State::Loading;
		e.ticket = m_nextTicket++;
		e.cancel = m_storeToken.child();
		const uint64_t ticket = e.ticket;
		const int maxLevels = m_config.maxLevels;
		// The job holds the source only WHILE it loads: a queued, cancelled or finished job keeps nothing alive. (A strong pointer in
		// the closure outlives the job until the pool lets go of it, which holds a pack file open after the store is gone: Windows
		// cannot delete it then.) A store destroyed before the job runs makes it a no-op.
		std::weak_ptr<const LandscapeTileSource> source = m_source;
		std::shared_ptr<Shared> shared = m_shared;

		JobDesc desc;
		desc.name     = "LandscapeTileLoad";
		desc.priority = JobPriority::Low;
		desc.cancel   = e.cancel;
		desc.onCancelled = [shared, key, ticket] {
			std::lock_guard<std::mutex> lock(shared->mutex);
			shared->done.push_back(Done{ key, ticket, Done::Kind::Cancelled, nullptr });
		};
		e.job = m_pool->scheduleFn([shared, source, key, ticket, maxLevels] {
			shared->running.fetch_add(1);
			auto tile = std::make_shared<LandscapeTile>();
			bool ok = false;
			if (!shared->closed.load())
			{
				if (auto src = source.lock())
				{
					try { ok = src->load(key, maxLevels, *tile); } catch (...) { ok = false; }
				}   // `src` is gone before the result is published: whoever sees the result can delete the file
			}
			{
				std::lock_guard<std::mutex> lock(shared->mutex);
				shared->done.push_back(Done{ key, ticket, ok ? Done::Kind::Loaded : Done::Kind::Failed, ok ? tile : nullptr });
			}
			shared->running.fetch_sub(1);
		}, std::move(desc));
		const auto c = m_tiles.emplace(key, std::move(e));
		(void)c;
		++m_loading;
		++m_stats.loadsStarted;
	}
}

void LandscapeStore::drain(double budgetMs)
{
	std::vector<Done> done;
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		done.swap(m_shared->done);
	}
	if (done.empty()) return;
	const auto t0 = Clock::now();
	size_t i = 0;
	for (; i < done.size(); ++i)
	{
		// At least one tile per update, then while the budget lasts.
		if (i > 0 && msSince(t0) >= budgetMs) break;
		Done& d = done[i];
		const auto it = m_tiles.find(d.key);
		const bool mine = it != m_tiles.end() && it->second.ticket == d.ticket && it->second.state == Entry::State::Loading;
		if (d.kind == Done::Kind::Cancelled)
		{
			++m_stats.loadsCancelled;
			if (mine) dropEntry(it);
			continue;
		}
		if (!mine)
		{
			if (d.kind == Done::Kind::Loaded) ++m_stats.loadsDiscarded;
			continue;
		}
		if (d.kind == Done::Kind::Failed)
		{
			++m_stats.loadsFailed;
			if (it->second.unwanted) { dropEntry(it); continue; }
			--m_loading;
			++m_failed;
			it->second.state = Entry::State::Failed;
			it->second.job = JobHandle();
			continue;
		}
		if (it->second.unwanted || !m_keep.count(d.key))
		{
			++m_stats.loadsDiscarded;
			dropEntry(it);
			continue;
		}
		Entry& e = it->second;
		e.tile  = std::move(d.tile);
		e.state = Entry::State::Resident;
		e.job   = JobHandle();
		--m_loading;
		++m_resident;
		m_residentBytes += e.tile->bytes();
		++m_stats.loadsFinished;
	}
	if (i < done.size())
	{
		// Over budget: the rest go back in front of what arrived meanwhile.
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		m_shared->done.insert(m_shared->done.begin(), std::make_move_iterator(done.begin() + static_cast<std::ptrdiff_t>(i)),
		                      std::make_move_iterator(done.end()));
	}
}

} // namespace HE
