#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "../HE_RENDERING_API.h"
#include "FrustumCuller.h"
#include "RenderSorter.h"
#include "RenderWorld.h"
#include <Math/AABB.h>

// What every backend's frame keeps about the walk in its RenderWorld to cull it cheaply
// (docs/perf-audit/frame-parallelism-audit-2026-10-26.md, levers 4, 5 and 7):
//
//  * refine() replaces each object's bound by the real bound of its mesh (once per walk) and keeps
//    a compact copy of them in object order. Every frustum cull of the frame (camera, cascades,
//    local lights) reads that copy: 24 bytes an object instead of a new cache line out of each
//    ~280-byte RenderObject.
//  * cullCamera() is the camera cull + draw order of the frame, shared by the passes that ask with
//    the same walk, camera and variant (SSAO, GI, decal depth and the scene all cull and sort the
//    same objects with the same view).
//  * sceneBox() is the union of the bounds, for the cloud shadow's ground height.
//
// The compact copy belongs to ONE walk. A cull asked for another walk, another object array or
// another object count does not read it: it tests the objects themselves, so a pass that runs
// before its backend refined (or after a new extract) is slower but never wrong.
// One depth view of the shadow pass (a cascade or a local light's face) and what is prepared for it
// on the CPU before any view is drawn: the objects its frustum sees, in draw order by mesh, cut into
// same-mesh runs. Every view owns its scratch (and its sorter), so the views are independent.
struct ShadowViewCpu
{
	glm::mat4                    viewProj{ 1.0f };
	uint32_t                     skip = kNoOwnerEntity;   // the light's own entity: its mesh casts nothing here
	std::vector<uint8_t>         visible;
	std::vector<uint32_t>        sorted;
	RenderSorter                 sorter;
	RenderSorter::DepthBatchList batches;
};

class HE_RENDERING_API FrameCull {
public:
	// Refine the bounds of `world` for the walk `walk` (RenderExtractor::fullExtractCount()), unless
	// they are already refined for it. `localBounds(obj)` returns the mesh's local bound of an object
	// or nullptr when its mesh is not resolved yet (the object keeps the bound the walk gave it).
	// The answer must depend on obj.meshAssetId alone: each distinct mesh is asked once per walk
	// (a scene has a few hundred meshes under tens of thousands of objects), then the per-object
	// work runs wide. A mesh that is not resolved yet is not asked again within the walk (its objects keep the walk's bound).
	template <class LocalBounds>
	void refine(RenderWorld& world, uint64_t walk, LocalBounds&& localBounds)
	{
		if (holds(world, walk)) return;
		const size_t count = world.objects.size();
		m_bounds.resize(count);
		// Phase 1, serial: each distinct mesh is asked for once (`localBounds` may resolve,
		// upload, mutate the backend's caches). Phase 2: the per-object product with the
		// object's transform and the copy into the compact array are pure reads of the table
		// and writes of the object's own slot, wide on the workers from 4096 objects on.
		struct Local { HE::AABB box; bool known = false; };   // `known` false: asked, not resolved yet
		std::unordered_map<HE::UUID, Local> table;
		HE::UUID seen[256] = {};   // direct-mapped front of `table`: ids asked already (the null id never has bounds)
		for (size_t i = 0; i < count; ++i)
		{
			const HE::UUID& id = world.objects[i].meshAssetId;
			HE::UUID& s = seen[std::hash<HE::UUID>{}(id) & 255u];
			if (s == id) continue;
			s = id;
			if (table.find(id) != table.end()) continue;
			const HE::AABB* local = localBounds(world.objects[i]);
			table.emplace(id, local ? Local{ *local, true } : Local{});
		}
		auto refineOne = [&](size_t i)
		{
			RenderObject& obj = world.objects[i];
			const auto it = table.find(obj.meshAssetId);
			if (it != table.end() && it->second.known) obj.refineWorldBounds(it->second.box);
			m_bounds[i] = obj.worldBounds;
		};
		if (count >= 4096) parallel_for(count, refineOne, "RefineBounds", 2048);
		else for (size_t i = 0; i < count; ++i) refineOne(i);
		m_walk    = walk;
		m_objects = world.objects.data();
		m_count   = count;
		m_sceneBoxValid = false;
		m_camera.stamp  = ~0ull;   // answers of an earlier refinement
	}

	// True when the compact bounds are those of `world` as walk `walk` left it.
	bool holds(const RenderWorld& world, uint64_t walk) const
	{
		return m_walk == walk && m_objects == world.objects.data() && m_count == world.objects.size();
	}

	// outVisible[i] for world.objects[i] against `viewProj`, over the compact bounds when they hold.
	void cull(FrustumCuller& culler, const RenderWorld& world, uint64_t walk, const glm::mat4& viewProj,
	          std::vector<uint8_t>& outVisible) const
	{
		if (holds(world, walk))
			culler.cull(m_bounds, viewProj, outVisible);
		else
			culler.cull(world, viewProj, outVisible);
	}

	// Union of every object's bound (invalid when there is none).
	HE::AABB sceneBox(const RenderWorld& world, uint64_t walk);

	// Cull, sort by mesh and cut into runs views[0, n) (their viewProj and skip set), side by side on
	// the job system's workers: a cascade's cull, sort and batching do not depend on another's, and
	// the backend's API calls stay on its own thread afterwards. Runs read in place or copied as
	// RenderSorter::instanceSpansEnabled() says; RenderSorter::parallelShadowViewsEnabled() = false
	// prepares them one after the other. Never call cullCamera from inside: it keeps its answer.
	void prepareShadowViews(FrustumCuller& culler, const RenderWorld& world, uint64_t walk,
	                        ShadowViewCpu* views, size_t n, const char* label) const;

	// The camera cull and distance sort of `world` into `visible` / `sorted`. `frame` is the backend's
	// frame counter and `variant` whatever else the answer depends on (occlusion on or off): a hit
	// needs the same refined walk, frame, camera, view-projection and variant, and copies the
	// cached answer out; a miss computes it and keeps it, but only over refined bounds (while they
	// may still change within the walk nothing is kept).
	void cullCamera(FrustumCuller& culler, RenderSorter& sorter, const RenderWorld& world, uint64_t walk,
	                uint64_t frame, uint32_t variant, std::vector<uint8_t>& visible,
	                std::vector<uint32_t>& sorted);

private:
	uint64_t              m_walk    = ~0ull;
	const RenderObject*   m_objects = nullptr;
	size_t                m_count   = 0;
	std::vector<HE::AABB> m_bounds;
	HE::AABB              m_sceneBox{};
	bool                  m_sceneBoxValid = false;
	struct CameraAnswer
	{
		uint64_t              stamp   = ~0ull;
		uint32_t              variant = 0;
		glm::mat4             viewProj{ 1.0f };
		glm::vec3             camPos{ 0.0f };
		std::vector<uint8_t>  visible;
		std::vector<uint32_t> sorted;
	} m_camera;
};
