#include "HorizonScene/LandscapeAtlas.h"

#include <algorithm>

namespace HE
{

LandscapeAtlas::LandscapeAtlas(size_t capacity, int window)
    : m_capacity(std::min<size_t>(capacity, 0xFFFE)), m_window(window)
{
	// the window is a power of two
	int w = 1;
	while (w < window) w <<= 1;
	m_window = w;
	m_table.assign(static_cast<size_t>(w) * w, kNoSlot);
	m_tableOwner.assign(static_cast<size_t>(w) * w, LandscapeTileKey{ -1, -1 });
	m_free.reserve(m_capacity);
	for (size_t i = m_capacity; i-- > 0;) m_free.push_back(static_cast<uint16_t>(i));   // slot 0 comes out first
}

uint16_t LandscapeAtlas::slotOf(LandscapeTileKey key) const
{
	const auto it = m_slotOf.find(key);
	return it == m_slotOf.end() ? kNoSlot : it->second;
}

uint16_t LandscapeAtlas::lookup(int tx, int tz) const
{
	const size_t idx = static_cast<size_t>(tz & (m_window - 1)) * m_window + static_cast<size_t>(tx & (m_window - 1));
	const LandscapeTileKey& o = m_tableOwner[idx];
	return (o.x == tx && o.z == tz) ? m_table[idx] : kNoSlot;
}

void LandscapeAtlas::sync(const LandscapeStore& store, std::vector<Upload>& uploads, std::vector<Release>& releases,
                          std::vector<TableUpdate>& table, size_t maxUploads)
{
	uploads.clear();
	releases.clear();
	table.clear();
	const auto entry = [&](LandscapeTileKey k) {
		return static_cast<uint32_t>((k.z & (m_window - 1)) * m_window + (k.x & (m_window - 1)));
	};

	// 1. give back what the store dropped
	for (auto it = m_slotOf.begin(); it != m_slotOf.end();)
	{
		if (store.isResident(it->first)) { ++it; continue; }
		const uint32_t e = entry(it->first);
		releases.push_back({ it->first, it->second });
		m_free.push_back(it->second);
		if (m_tableOwner[e] == it->first)
		{
			m_table[e] = kNoSlot;
			m_tableOwner[e] = { -1, -1 };
			table.push_back({ e, kNoSlot });
		}
		it = m_slotOf.erase(it);
		++m_stats.releases;
	}

	// 2. new tiles. Nearest-first is not known here; the store's resident set is small and mostly already in the atlas.
	for (const LandscapeTileKey& k : store.residentKeys())
	{
		if (m_slotOf.count(k)) continue;
		if (uploads.size() >= maxUploads) { ++m_stats.deferred; continue; }
		const uint32_t e = entry(k);
		if (m_tableOwner[e].x != -1 && !(m_tableOwner[e] == k))
		{
			++m_stats.aliased;   // another resident tile owns the entry: the window is too small for this disc
			continue;
		}
		if (m_free.empty()) { ++m_stats.full; continue; }
		const uint16_t slot = m_free.back();
		m_free.pop_back();
		m_slotOf.emplace(k, slot);
		m_table[e] = slot;
		m_tableOwner[e] = k;
		table.push_back({ e, slot });
		uploads.push_back({ k, slot, store.tile(k) });
		++m_stats.uploads;
	}
	m_stats.used = m_slotOf.size();
}

} // namespace HE
