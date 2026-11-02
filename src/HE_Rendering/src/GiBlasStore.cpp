#include "HorizonRendering/GiBlasStore.h"

#include <algorithm>

namespace HE
{
namespace
{
// Below this much dead data a compaction is not worth its full re-upload
// (64 Ki triangles = 3 MB): a few rebuilt props never trigger one, a flight
// over a landscape that re-stitches its chunks does, every few seconds.
constexpr size_t kMinDeadTris = size_t{1} << 16;
} // namespace

const GiBlasRange* GiBlasStore::find(const UUID& id) const
{
	const auto it = m_ranges.find(id);
	return it == m_ranges.end() ? nullptr : &it->second;
}

GiBlasRange GiBlasStore::add(const UUID& id, const GiBvh& bvh)
{
	GiBlasRange r;
	if (bvh.valid())
	{
		r.nodeOffset = static_cast<int32_t>(m_nodes.size());
		r.nodeCount  = static_cast<int32_t>(bvh.nodes.size());
		r.triOffset  = static_cast<int32_t>(m_tris.size());
		r.triCount   = static_cast<int32_t>(bvh.triangles.size());
		r.valid      = true;
		m_nodes.insert(m_nodes.end(), bvh.nodes.begin(), bvh.nodes.end());
		m_tris.insert(m_tris.end(), bvh.triangles.begin(), bvh.triangles.end());
	}
	auto [it, fresh] = m_ranges.try_emplace(id, r);
	if (!fresh)
	{
		forget(it->second);
		it->second = r;
	}
	return r;
}

bool GiBlasStore::erase(const UUID& id)
{
	const auto it = m_ranges.find(id);
	if (it == m_ranges.end()) return false;
	forget(it->second);
	m_ranges.erase(it);
	return true;
}

void GiBlasStore::forget(const GiBlasRange& r)
{
	if (!r.valid) return;
	m_deadNodes += static_cast<size_t>(r.nodeCount);
	m_deadTris  += static_cast<size_t>(r.triCount);
}

bool GiBlasStore::compactIfWasteful()
{
	const size_t liveTris = m_tris.size() - m_deadTris;
	if (m_deadTris < kMinDeadTris || m_deadTris <= liveTris) return false;

	// Live ranges in array order (add() appends nodes and triangles together, so
	// one order fits both): each one moves down or stays, never onto a live
	// range that has not moved yet, so a forward copy is safe (std::copy only
	// forbids a destination that starts INSIDE the source: skip the stays).
	std::vector<GiBlasRange*> order;
	order.reserve(m_ranges.size());
	for (auto& [id, r] : m_ranges)
		if (r.valid) order.push_back(&r);
	std::sort(order.begin(), order.end(),
	          [](const GiBlasRange* a, const GiBlasRange* b) { return a->nodeOffset < b->nodeOffset; });

	size_t n = 0, t = 0;
	for (GiBlasRange* r : order)
	{
		if (static_cast<size_t>(r->nodeOffset) != n)
			std::copy(m_nodes.begin() + r->nodeOffset, m_nodes.begin() + r->nodeOffset + r->nodeCount,
			          m_nodes.begin() + static_cast<std::ptrdiff_t>(n));
		if (static_cast<size_t>(r->triOffset) != t)
			std::copy(m_tris.begin() + r->triOffset, m_tris.begin() + r->triOffset + r->triCount,
			          m_tris.begin() + static_cast<std::ptrdiff_t>(t));
		r->nodeOffset = static_cast<int32_t>(n);
		r->triOffset  = static_cast<int32_t>(t);
		n += static_cast<size_t>(r->nodeCount);
		t += static_cast<size_t>(r->triCount);
	}
	m_nodes.resize(n);
	m_tris.resize(t);
	m_deadNodes = m_deadTris = 0;
	m_rewrite   = true;
	return true;
}

void GiBlasStore::clear()
{
	m_ranges.clear();
	m_nodes.clear();
	m_tris.clear();
	m_deadNodes = m_deadTris = 0;
	m_nodesUploaded = m_trisUploaded = 0;
	m_rewrite = true;
}

void GiBlasStore::markUploaded()
{
	m_nodesUploaded = m_nodes.size();
	m_trisUploaded  = m_tris.size();
	m_rewrite       = false;
}
} // namespace HE
