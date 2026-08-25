#include "HorizonRendering/RenderPass.h"
#include <cstdint>
#include "HorizonRendering/RenderObject.h"
#include "HorizonRendering/FrustumCuller.h"

// ─── GeometryPass ───────────────────────────────────────────────────────────
// Turns the culled + sorted visible objects into draw calls. It produces no
// GPU work itself — the backend replays the resulting command buffer with its
// own pipeline + per-frame state (camera, lights). It is the one pass every
// backend wires into its graph; which of the others join it is the backend's
// choice (GL: Shadow → Geometry → PostProcess; D3D11/D3D12: Shadow → Geometry;
// Metal/Vulkan: Geometry alone, their shadow and tonemap work is encoded
// outside the graph).
void GeometryPass::execute(const RenderWorld&           world,
                           const std::vector<uint32_t>& sortedIndices,
                           CommandBuffer&               outCmds)
{
	// Batch consecutive runs of objects that share mesh + material.
	// The sorter already groups by mesh id, so runs are the common case.
	size_t i = 0;
	while (i < sortedIndices.size())
	{
		const uint32_t idx = sortedIndices[i];
		if (idx >= world.objects.size()) { ++i; continue; }

		const RenderObject& first = world.objects[idx];

		// Extend the run while the next valid index has the same key.
		size_t j = i + 1;
		while (j < sortedIndices.size())
		{
			const uint32_t jdx = sortedIndices[j];
			if (jdx >= world.objects.size()) break;
			const RenderObject& next = world.objects[jdx];
			if (next.meshAssetId != first.meshAssetId ||
			    next.materialAssetId != first.materialAssetId) break;
			// Per-entity param overrides make objects visually distinct even when
			// they share mesh+material — they must not be instanced together.
			if (!next.paramOverride.empty() || !first.paramOverride.empty()) break;
			// Same for a per-instance tint (particle color/alpha-over-life): two
			// particles at different points in their life share mesh+material but
			// must NOT be instanced together, or they'd all draw with one shared
			// tint. Identity tint (the default for everything else) always matches
			// identity, so non-particle batching is unaffected.
			if (next.instanceTint != first.instanceTint) break;
			// The shadow opt-out is one per-draw uniform lane, so a batch may only
			// hold objects that answer it the same way.
			if (next.receivesShadow != first.receivesShadow) break;
			// Landscape chunks of DIFFERENT terrains share mesh+material but each
			// samples its own painted weightmap, so they can't share a draw.
			if (next.weightmapTextureId != first.weightmapTextureId) break;
			++j;
		}

		const size_t runLen = j - i;
		DrawCall dc;
		dc.meshAssetId     = first.meshAssetId;
		dc.materialAssetId = first.materialAssetId;
		dc.mesh            = first.meshHandle;
		dc.material        = first.materialHandle;
		dc.transform       = first.transform;
		dc.entityId        = first.entityId;
		dc.lod             = first.lod;
		dc.contributesAO   = first.contributesAO;
		dc.receivesShadow  = first.receivesShadow; // batching guarantees the whole run shares this
		dc.baseColor       = first.baseColor;
		dc.metallic        = first.metallic;
		dc.roughness       = first.roughness;
		dc.opacity         = first.opacity;
		dc.instanceTint    = first.instanceTint; // batching guarantees the whole run shares this
		dc.paramOverride   = first.paramOverride; // per-entity HeParams block (empty = none)
		dc.weightmapTextureId = first.weightmapTextureId; // landscape layer weights

		if (runLen > 1)
		{
			dc.instanceCount = static_cast<uint32_t>(runLen);
			dc.instanceTransforms.reserve(runLen);
			for (size_t k = i; k < j; ++k)
				dc.instanceTransforms.push_back(world.objects[sortedIndices[k]].transform);
		}

		outCmds.recordDraw(dc);
		i = j;
	}

	// Skinned objects are not in the culled+sorted set: they are always drawn in
	// submission order. Frustum-culling and sorting can be added in 4d.3+.
	for (const SkinnedRenderObject& obj : world.skinnedObjects)
	{
		SkinnedDrawCall dc;
		dc.meshAssetId     = obj.meshAssetId;
		dc.materialAssetId = obj.materialAssetId;
		dc.mesh          = obj.meshHandle;
		dc.material      = obj.materialHandle;
		dc.transform     = obj.transform;
		dc.instanceCount = 1;
		dc.entityId      = obj.entityId;
		dc.lod           = obj.lod;
		dc.receivesShadow  = obj.receivesShadow;
		dc.boneMatrices  = obj.boneMatrices;
		dc.paramOverride = obj.paramOverride; // per-entity HeParams block (empty = none)
		outCmds.recordSkinnedDraw(dc);
	}
}

// ─── ShadowPass ─────────────────────────────────────────────────────────────
// Records the shadow casters and the backend replays them depth-only from the
// light's POV (world.shadow.viewProj) into the shadow map. Skips everything when
// no directional light is present.
//
// Casters are culled against the LIGHT frustum, not the camera-culled
// sortedIndices the geometry pass uses: an object outside the camera view still
// casts a shadow into the visible scene as long as it lies within the shadow
// map's coverage. (Reusing the camera-culled set made shadows pop out the
// moment their caster left the screen, even though the caster still influenced
// the scene.)
void ShadowPass::execute(const RenderWorld&           world,
                         const std::vector<uint32_t>& /*sortedIndices*/,
                         CommandBuffer&               outCmds)
{
	if (!world.shadow.enabled) return;
	const Frustum lightFrustum = Frustum::fromViewProj(world.shadow.viewProj);
	for (size_t idx = 0; idx < world.objects.size(); ++idx)
	{
		const RenderObject& obj = world.objects[idx];
		if (!obj.castsShadow) continue; // billboards (precip/particles) don't cast shadows
		if (obj.worldBounds.isValid() && !lightFrustum.intersects(obj.worldBounds))
			continue; // caster lies entirely outside the shadow map's coverage
		DrawCall dc;
		dc.meshAssetId   = obj.meshAssetId;
		dc.transform     = obj.transform;
		dc.instanceCount = 1;
		dc.entityId      = obj.entityId;
		dc.lod           = obj.lod;
		outCmds.recordDraw(dc);
	}
}

// ─── PostProcessPass ─────────────────────────────────────────────────────────
// Signals the backend that post-processing should run this frame. The actual
// fullscreen tonemap/bloom/FXAA passes are dispatched by the backend directly
// (they need per-backend pipeline state); this marker lets the graph and tests
// verify the pass executed without pulling in GPU code.
void PostProcessPass::execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer& outCmds)
{
	outCmds.recordPostProcess();
}

RenderPassIO ShadowPass::describe() const
{
    RenderPassIO io;
    io.output.id        = kShadowMapTarget;
    io.output.format    = RenderTargetFormat::Depth;
    io.output.sizeMode  = RenderTargetSize::Fixed;
    io.output.width     = 2048;
    io.output.height    = 2048;
    io.output.depth     = true;
    io.output.debugName = "shadowmap";
    return io;
}

RenderPassIO PostProcessPass::describe() const
{
    RenderPassIO io;
    io.output.id        = kBackbufferTarget; // tonemap to the backbuffer
    io.output.debugName = "backbuffer";
    io.inputs[0]        = kSceneColorTarget;
    io.inputCount       = 1;
    return io;
}
