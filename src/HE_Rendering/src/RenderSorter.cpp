#include "HorizonRendering/RenderSorter.h"
#include "HorizonRendering/RenderConstants.h"
#include <cstdint>
#include <algorithm>
#include <cstdlib>

// Sort key: group by mesh asset (minimises GPU state changes), then
// front-to-back within a group (early-z friendliness). The distance and mesh
// id are computed once per object into a key array, so the std::sort comparator
// — invoked O(n log n) times — only compares cheap precomputed scalars instead
// of re-extracting matrix columns and recomputing squared distances each call.
void RenderSorter::sort(const RenderWorld&          world,
                        const std::vector<uint8_t>& visible,
                        std::vector<uint32_t>&       outSortedIndices)
{
	const glm::vec3 camPos = world.camera.position;

	m_keys.clear();
	m_keys.reserve(world.objects.size());
	for (uint32_t i = 0; i < world.objects.size(); ++i)
	{
		if (i < visible.size() && !visible[i])
			continue;
		const RenderObject& o = world.objects[i];
		const glm::vec3     d = glm::vec3(o.transform[3]) - camPos;
		m_keys.push_back(SortKey{ o.meshAssetId.hi, o.meshAssetId.lo,
		                          glm::dot(d, d), i });
	}

	std::sort(m_keys.begin(), m_keys.end(),
		[](const SortKey& a, const SortKey& b)
		{
			if (a.meshHi != b.meshHi) return a.meshHi < b.meshHi;
			if (a.meshLo != b.meshLo) return a.meshLo < b.meshLo;
			return a.distSq < b.distSq;
		});

	outSortedIndices.clear();
	outSortedIndices.reserve(m_keys.size());
	for (const SortKey& k : m_keys)
		outSortedIndices.push_back(k.index);
}

void RenderSorter::sortByMesh(const RenderWorld&          world,
                              const std::vector<uint8_t>& visible,
                              std::vector<uint32_t>&       outSortedIndices)
{
	// Mesh ascending, then object index ascending. A scene has a few hundred meshes under tens
	// of thousands of objects, so this is a counting sort over the meshes' rank (sorting only
	// the distinct ids), not a comparison sort of every object: the same order, in O(n).
	const uint32_t total = static_cast<uint32_t>(world.objects.size());
	m_visible.clear();
	m_denseOf.clear();
	m_visible.reserve(total);
	m_denseOf.reserve(total);
	size_t cap = 1024;   // power of two, kept under half full
	m_meshTable.assign(cap, MeshSlot{ 0, 0, 0, false });
	std::vector<std::pair<uint64_t, uint64_t>> distinct;   // (hi, lo) by dense id
	for (uint32_t i = 0; i < total; ++i)
	{
		if (i < visible.size() && !visible[i])
			continue;
		const HE::UUID& id = world.objects[i].meshAssetId;
		size_t at = static_cast<size_t>((id.hi * 0x9E3779B97F4A7C15ull) ^ (id.lo * 0xC2B2AE3D27D4EB4Full)) & (cap - 1);
		while (m_meshTable[at].used && !(m_meshTable[at].hi == id.hi && m_meshTable[at].lo == id.lo))
			at = (at + 1) & (cap - 1);
		if (!m_meshTable[at].used)
		{
			m_meshTable[at] = MeshSlot{ id.hi, id.lo, static_cast<uint32_t>(distinct.size()), true };
			distinct.emplace_back(id.hi, id.lo);
			if (distinct.size() * 2 > cap)
			{
				// Grow and re-insert; `at` of the object in hand is looked up again below.
				cap *= 2;
				std::vector<MeshSlot> old;
				old.swap(m_meshTable);
				m_meshTable.assign(cap, MeshSlot{ 0, 0, 0, false });
				for (const MeshSlot& s : old)
					if (s.used)
					{
						size_t k = static_cast<size_t>((s.hi * 0x9E3779B97F4A7C15ull) ^ (s.lo * 0xC2B2AE3D27D4EB4Full)) & (cap - 1);
						while (m_meshTable[k].used) k = (k + 1) & (cap - 1);
						m_meshTable[k] = s;
					}
				at = static_cast<size_t>((id.hi * 0x9E3779B97F4A7C15ull) ^ (id.lo * 0xC2B2AE3D27D4EB4Full)) & (cap - 1);
				while (!(m_meshTable[at].hi == id.hi && m_meshTable[at].lo == id.lo && m_meshTable[at].used))
					at = (at + 1) & (cap - 1);
			}
		}
		m_visible.push_back(i);
		m_denseOf.push_back(m_meshTable[at].dense);
	}

	// Rank of each distinct mesh in (hi, lo) order.
	const uint32_t nMesh = static_cast<uint32_t>(distinct.size());
	std::vector<uint32_t> order(nMesh);
	for (uint32_t d = 0; d < nMesh; ++d) order[d] = d;
	std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return distinct[a] < distinct[b]; });
	m_rankOfDense.assign(nMesh, 0);
	for (uint32_t r = 0; r < nMesh; ++r) m_rankOfDense[order[r]] = r;

	// Counting sort by rank; the scatter walks the objects in index order, so each mesh's
	// objects stay in index order (the old comparator's tie-break).
	m_bucket.assign(static_cast<size_t>(nMesh) + 1, 0);
	for (uint32_t d : m_denseOf) ++m_bucket[m_rankOfDense[d] + 1];
	for (uint32_t r = 0; r < nMesh; ++r) m_bucket[r + 1] += m_bucket[r];
	outSortedIndices.resize(m_visible.size());
	for (size_t k = 0; k < m_visible.size(); ++k)
		outSortedIndices[m_bucket[m_rankOfDense[m_denseOf[k]]]++] = m_visible[k];
}

void RenderSorter::partitionByOpacity(const std::vector<DrawCall>&  drawCalls,
                                      std::vector<const DrawCall*>& outOpaque,
                                      std::vector<const DrawCall*>& outTransparent,
                                      bool                          sectionAware)
{
	outOpaque.clear();
	outTransparent.clear();
	outOpaque.reserve(drawCalls.size());
	for (const DrawCall& dc : drawCalls)
	{
		// GL and Metal classify inline and honour DrawCall::indexOffset/
		// indexCount; the backends collecting through here (D3D11, D3D12,
		// Vulkan) do too and pass sectionAware. The default guards a caller that
		// draws the whole index buffer per DrawCall: for it a multi-section
		// mesh's second, third… draw would repaint the entire mesh in another
		// material, so it gets slot 0 alone — one draw, the mesh's own material,
		// exactly where they were before sections existed. A one-section mesh
		// never carries sectionIndex > 0, so nothing changes for it either way.
		if (!sectionAware && dc.sectionIndex > 0) continue;
		(isTransparent(dc) ? outTransparent : outOpaque).push_back(&dc);
	}
}

void RenderSorter::sortBackToFront(std::vector<const DrawCall*>& transparent,
                                   const glm::vec3&              camPos)
{
	std::sort(transparent.begin(), transparent.end(),
		[&camPos](const DrawCall* a, const DrawCall* b)
		{
			return backToFrontKey(a->transform, camPos)
			     > backToFrontKey(b->transform, camPos);
		});
}

void RenderSorter::batchDepthRuns(const RenderWorld&           world,
                                  const std::vector<uint32_t>& sortedIndices,
                                  DepthFilter                  filter,
                                  uint32_t                     skipEntity,
                                  DepthBatchList&              out)
{
	out.clear();
	if (out.useSpans)
	{
		// The same runs, the instances read in place (DepthBatchList::useSpans): a span per
		// object, cut where a run reaches kMaxInstancesPerDraw.
		for (uint32_t idx : sortedIndices)
		{
			if (idx >= world.objects.size()) continue;
			const RenderObject& obj = world.objects[idx];
			if (filter == DepthFilter::ShadowCasters  && !obj.castsShadow)   continue;
			if (filter == DepthFilter::AoContributors && !obj.contributesAO) continue;
			if (obj.entityId == skipEntity) continue;
			const uint32_t total = world.instanceCountOf(obj);
			uint32_t done = 0;
			while (done < total)
			{
				const bool extend = !out.batches.empty() && out.batches.back().meshAssetId == obj.meshAssetId
				                 && out.batches.back().count < HE::kMaxInstancesPerDraw;
				if (!extend)
					out.batches.push_back(DepthBatch{ obj.meshAssetId, 0, 0,
					                                  static_cast<uint32_t>(out.spans.size()), 0 });
				DepthBatch&    b    = out.batches.back();
				const uint32_t room = HE::kMaxInstancesPerDraw - b.count;
				const uint32_t take = std::min(total - done, room);
				out.spans.push_back(world.instanceSpan(obj, done, take));
				++b.spanCount;
				b.count += take;
				done    += take;
			}
		}
		return;
	}
	// Two phases: the runs and where each object's instances go (serial, no matrix touched),
	// then the matrices into one array sized once, on the job system's workers. With foliage the
	// list used to grow through every doubling while each cluster unfolded on the main thread,
	// half of the 300k frame (docs/perf-audit/frame-parallelism-audit-2026-10-26.md, lever 1).
	// An ordinary object's one matrix is copied right here; only clusters become pieces.
	size_t upper = 0;
	for (uint32_t idx : sortedIndices)
		if (idx < world.objects.size()) upper += world.instanceCountOf(world.objects[idx]);
	out.transforms.resize(upper);
	std::vector<RenderWorld::InstancePiece> pieces;
	size_t total = 0;
	for (uint32_t idx : sortedIndices)
	{
		if (idx >= world.objects.size()) continue;
		const RenderObject& obj = world.objects[idx];
		// Billboards (precip/particles) opt out of both depth maps and AO.
		if (filter == DepthFilter::ShadowCasters  && !obj.castsShadow)   continue;
		if (filter == DepthFilter::AoContributors && !obj.contributesAO) continue;
		if (obj.entityId == skipEntity) continue; // the light's own mesh

		// What the object stands for: itself, or — a foliage cluster — every plant of
		// its bucket. A depth-only pass has no per-instance state, so a cluster joins
		// the run exactly as that many ordinary objects of the same mesh would.
		uint32_t left = world.instanceCountOf(obj);
		size_t   pos  = total;
		if (obj.isCluster()) world.addInstancePiece(pieces, obj, 0, left, pos);
		else                 out.transforms[pos] = obj.transform;
		total += left;

		// Extend the current run when the mesh matches; the sorter grouped by
		// mesh id, so a change here means a genuinely new mesh. A run holds at most
		// kMaxInstancesPerDraw: backends fall back to one draw per caster above
		// their instance ring, so a longer run is cut into several instanced draws.
		if (!out.batches.empty() && out.batches.back().meshAssetId == obj.meshAssetId)
		{
			DepthBatch& b = out.batches.back();
			const uint32_t room = b.count < HE::kMaxInstancesPerDraw ? HE::kMaxInstancesPerDraw - b.count : 0u;
			const uint32_t take = left < room ? left : room;
			b.count += take;
			left    -= take;
			pos     += take;
		}
		while (left > 0)
		{
			const uint32_t take = left < HE::kMaxInstancesPerDraw ? left : HE::kMaxInstancesPerDraw;
			out.batches.push_back(DepthBatch{ obj.meshAssetId, static_cast<uint32_t>(pos), take });
			pos  += take;
			left -= take;
		}
	}
	out.transforms.resize(total);   // the filters dropped what the upper bound counted
	world.writeInstancePieces(pieces, out.transforms.data());
}

bool RenderSorter::depthInstancingEnabled()
{
	static const bool on = []{
		const char* v = std::getenv("HE_DEPTH_INSTANCING");
		return !(v && *v && std::atoi(v) == 0);
	}();
	return on;
}

bool RenderSorter::instanceSpansEnabled()
{
	static const bool on = []{
		const char* v = std::getenv("HE_INSTANCE_SPANS");
		return !(v && *v && std::atoi(v) == 0);
	}();
	return on;
}

bool RenderSorter::parallelShadowViewsEnabled()
{
	static const bool on = []{
		const char* v = std::getenv("HE_MTL_PARALLEL_SHADOW");
		return !(v && *v && std::atoi(v) == 0);
	}();
	return on;
}

bool RenderSorter::graphInstancingEnabled()
{
	static const bool on = []{
		const char* v = std::getenv("HE_GRAPH_INSTANCING");
		return !(v && *v && std::atoi(v) == 0);
	}();
	return on;
}
