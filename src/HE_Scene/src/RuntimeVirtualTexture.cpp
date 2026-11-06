#include <HorizonScene/RuntimeVirtualTexture.h>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace HE::rvt
{

// ── VolumeLayout ──────────────────────────────────────────────────────────────────────────────
VolumeLayout::VolumeLayout(const VolumeDesc& d) : m_desc(d)
{
	m_desc.pageSize = std::max(d.pageSize, 8);
	m_desc.border   = std::max(d.border, 0);
	m_desc.finestMetersPerTexel = std::max(d.finestMetersPerTexel, 1e-4f);
	const float pageM0 = m_desc.finestMetersPerTexel * float(m_desc.pageSize);
	const int   need   = std::max(1, int(std::ceil(std::max(d.sizeM, 1e-3f) / pageM0)));
	int p = 1;
	while (p < need) p <<= 1;               // a power of two, so every mip halves cleanly down to one page
	m_pages0 = p;
	m_mipCount = 1;
	while ((p >> (m_mipCount - 1)) > 1) ++m_mipCount;
	m_extentM = pageM0 * float(m_pages0);
}

void VolumeLayout::pageRect(int mip, int x, int y, glm::vec2& mn, glm::vec2& mx, bool withBorder) const
{
	const float pm = pageMeters(mip);
	const float bm = withBorder ? float(m_desc.border) * texelMeters(mip) : 0.0f;
	mn = m_desc.origin + glm::vec2(float(x) * pm - bm, float(y) * pm - bm);
	mx = m_desc.origin + glm::vec2(float(x + 1) * pm + bm, float(y + 1) * pm + bm);
}

void VolumeLayout::pageAt(int mip, glm::vec2 world, int& x, int& y) const
{
	const float pm = pageMeters(mip);
	const int   n  = pagesPerSide(mip);
	const glm::vec2 l = (world - m_desc.origin) / pm;
	x = std::clamp(int(std::floor(l.x)), 0, n - 1);
	y = std::clamp(int(std::floor(l.y)), 0, n - 1);
}

int VolumeLayout::mipForMetersPerPixel(float mpp) const
{
	const float ratio = std::max(mpp / m_desc.finestMetersPerTexel, 1.0f);
	return std::clamp(int(std::floor(std::log2(ratio))), 0, m_mipCount - 1);
}

// ── RvtManager ────────────────────────────────────────────────────────────────────────────────
RvtManager::RvtManager(uint32_t poolSlots, uint32_t maxRendersPerFrame, int pinnedMips)
	: m_poolSlots(poolSlots), m_maxRenders(maxRendersPerFrame), m_pinnedMips(std::max(pinnedMips, 0))
{
	m_freeSlots.reserve(poolSlots);
	for (uint32_t i = poolSlots; i > 0; --i) m_freeSlots.push_back(i - 1);   // slot 0 is handed out first
}

uint16_t RvtManager::addVolume(const VolumeDesc& d)
{
	for (size_t i = 0; i < m_volumes.size(); ++i)
		if (!m_volumes[i].live)
		{
			m_volumes[i].layout = VolumeLayout(d);
			m_volumes[i].live = true;
			++m_volumes[i].tableVersion;
			return uint16_t(i);
		}
	Volume v;
	v.layout = VolumeLayout(d);
	v.live = true;
	m_volumes.push_back(v);
	return uint16_t(m_volumes.size() - 1);
}

void RvtManager::removeVolume(uint16_t id)
{
	if (!hasVolume(id)) return;
	dropVolumePages(id);
	m_volumes[id].live = false;
}

void RvtManager::dropVolumePages(uint16_t volume)
{
	for (auto it = m_pages.begin(); it != m_pages.end();)
	{
		if (uint16_t(it->first >> 40) == volume) { m_freeSlots.push_back(it->second.slot); it = m_pages.erase(it); }
		else ++it;
	}
	if (hasVolume(volume)) ++m_volumes[volume].tableVersion;
}

bool RvtManager::acquireSlot(uint32_t& slot)
{
	if (!m_freeSlots.empty()) { slot = m_freeSlots.back(); m_freeSlots.pop_back(); return true; }
	// Least recently used, but never a page the current frame already wants and never a pinned one. A linear
	// scan: the pool holds a few thousand pages and update() evicts at most maxRendersPerFrame of them.
	auto victim = m_pages.end();
	for (auto it = m_pages.begin(); it != m_pages.end(); ++it)
	{
		if (it->second.pinned || it->second.lastUsed >= m_frame) continue;
		if (victim == m_pages.end() || it->second.lastUsed < victim->second.lastUsed) victim = it;
	}
	if (victim == m_pages.end()) return false;
	slot = victim->second.slot;
	++m_volumes[uint16_t(victim->first >> 40)].tableVersion;
	m_pages.erase(victim);
	++m_stats.evictions;
	return true;
}

std::vector<PageRender> RvtManager::update(uint16_t volume, const ViewDesc& view)
{
	return updateAll({ VolumeView{ volume, view } });
}

std::vector<PageRender> RvtManager::updateAll(const std::vector<VolumeView>& views)
{
	std::vector<PageRender> out;
	struct Want { PageKey key; int group; float dist; };
	std::vector<Want> wants;
	m_missing.assign(m_volumes.size(), 0);

	// Phase 1: every volume's wanted pages are touched BEFORE anything is allocated, so a page one volume still needs this
	// frame can never be the victim of another volume's allocation.
	for (const VolumeView& vv : views)
	{
		const uint16_t volume = vv.volume;
		if (!hasVolume(volume)) continue;
		const ViewDesc& view = vv.view;
		Volume& v = m_volumes[volume];
		const VolumeLayout& L = v.layout;
		const glm::vec2 vmin = L.desc().origin, vmax = vmin + glm::vec2(L.extentM());

		for (int mip = 0; mip < L.mipCount(); ++mip)
		{
			const bool pin = isPinnedMip(v, mip);
			const float h = view.nearRadiusM * float(1 << std::min(mip, 30));
			int x0 = 0, y0 = 0, x1 = L.pagesPerSide(mip) - 1, y1 = x1;
			if (!pin)
			{
				// The camera square of this mip, clipped to the volume; a volume the square does not touch needs nothing here.
				if (view.cameraXZ.x + h < vmin.x || view.cameraXZ.x - h > vmax.x ||
				    view.cameraXZ.y + h < vmin.y || view.cameraXZ.y - h > vmax.y) continue;
				L.pageAt(mip, view.cameraXZ - glm::vec2(h), x0, y0);
				L.pageAt(mip, view.cameraXZ + glm::vec2(h), x1, y1);
			}
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
				{
					PageKey k{ volume, uint8_t(mip), uint16_t(x), uint16_t(y) };
					glm::vec2 mn, mx;
					L.pageRect(mip, x, y, mn, mx, false);
					const glm::vec2 c = (mn + mx) * 0.5f;
					const float dist = std::max(std::abs(c.x - view.cameraXZ.x), std::abs(c.y - view.cameraXZ.y));
					auto it = m_pages.find(k.pack());
					if (it != m_pages.end())
					{
						it->second.lastUsed = m_frame;                      // needed: not evictable this frame
						it->second.pinned = pin;
						if (it->second.dirty) wants.push_back({ k, 1, dist });
					}
					else
					{
						// Nothing coarser to show in its place yet: these come first, coarsest first, so the whole view is covered at once.
						const bool bare = resolve(volume, mip, x, y) == kPageTableInvalid;
						wants.push_back({ k, bare ? 0 : 1, bare ? float(-mip) : dist });
					}
				}
		}
	}

	// Phase 2: one ranking over all volumes, one budget.
	std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) {
		return std::tie(a.group, a.dist, a.key.mip, a.key.volume) < std::tie(b.group, b.dist, b.key.mip, b.key.volume); });

	uint32_t budget = m_maxRenders > m_stats.renderedLastUpdate ? m_maxRenders - m_stats.renderedLastUpdate : 0;
	uint32_t missing = 0;
	for (const Want& w : wants)
	{
		Volume& v = m_volumes[w.key.volume];
		const VolumeLayout& L = v.layout;
		auto it = m_pages.find(w.key.pack());
		const bool repaint = it != m_pages.end();
		if (budget == 0) { if (!repaint) { ++missing; ++m_missing[w.key.volume]; } continue; }
		uint32_t slot = 0;
		if (repaint) { slot = it->second.slot; it->second.dirty = false; }
		else
		{
			if (!acquireSlot(slot)) { ++m_stats.starved; ++missing; ++m_missing[w.key.volume]; continue; }
			Page p; p.slot = slot; p.lastUsed = m_frame; p.pinned = isPinnedMip(v, w.key.mip);
			m_pages[w.key.pack()] = p;
			++v.tableVersion;
		}
		PageRender r;
		r.key = w.key; r.slot = slot; r.repaint = repaint;
		L.pageRect(w.key.mip, w.key.x, w.key.y, r.rectMin, r.rectMax, true);
		out.push_back(r);
		--budget;
		++m_stats.renderedLastUpdate;
		++m_stats.totalRendered;
	}
	m_stats.wantedMissing = missing;
	return out;
}

uint32_t RvtManager::invalidateRect(uint16_t volume, glm::vec2 mn, glm::vec2 mx)
{
	if (!hasVolume(volume)) return 0;
	const VolumeLayout& L = m_volumes[volume].layout;
	uint32_t n = 0;
	for (auto& kv : m_pages)
	{
		if (uint16_t(kv.first >> 40) != volume) continue;
		const int mip = int((kv.first >> 32) & 0xFF), x = int((kv.first >> 16) & 0xFFFF), y = int(kv.first & 0xFFFF);
		glm::vec2 pmn, pmx;
		L.pageRect(mip, x, y, pmn, pmx, true);
		if (pmx.x <= mn.x || pmn.x >= mx.x || pmx.y <= mn.y || pmn.y >= mx.y) continue;
		kv.second.dirty = true;
		++n;
	}
	return n;
}

uint32_t RvtManager::invalidateVolume(uint16_t volume)
{
	uint32_t n = 0;
	for (auto& kv : m_pages)
		if (uint16_t(kv.first >> 40) == volume) { kv.second.dirty = true; ++n; }
	return n;
}

bool RvtManager::isDirty(const PageKey& k) const
{
	auto it = m_pages.find(k.pack());
	return it != m_pages.end() && it->second.dirty;
}

uint32_t RvtManager::resolve(uint16_t volume, int mip, int x, int y) const
{
	if (!hasVolume(volume)) return kPageTableInvalid;
	const int mips = m_volumes[volume].layout.mipCount();
	for (int m = mip; m < mips; ++m, x >>= 1, y >>= 1)
	{
		auto it = m_pages.find(PageKey{ volume, uint8_t(m), uint16_t(x), uint16_t(y) }.pack());
		if (it != m_pages.end()) return packPageTableEntry(it->second.slot, m);
	}
	return kPageTableInvalid;
}

std::vector<uint32_t> RvtManager::pageTable(uint16_t volume, int mip) const
{
	std::vector<uint32_t> t;
	if (!hasVolume(volume) || mip >= m_volumes[volume].layout.mipCount()) return t;
	const int n = m_volumes[volume].layout.pagesPerSide(mip);
	t.resize(size_t(n) * size_t(n));
	for (int y = 0; y < n; ++y)
		for (int x = 0; x < n; ++x) t[size_t(y) * size_t(n) + size_t(x)] = resolve(volume, mip, x, y);
	return t;
}

Stats RvtManager::stats() const
{
	Stats s = m_stats;
	s.poolSlots = m_poolSlots;
	s.resident = uint32_t(m_pages.size());
	s.dirty = s.pinned = 0;
	for (const auto& kv : m_pages) { s.dirty += kv.second.dirty ? 1u : 0u; s.pinned += kv.second.pinned ? 1u : 0u; }
	return s;
}

} // namespace HE::rvt
