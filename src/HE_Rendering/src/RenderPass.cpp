#include "HorizonRendering/RenderPass.h"
#include "HorizonRendering/RenderObject.h"
#include "HorizonRendering/FrustumCuller.h"

// ─── GeometryPass ───────────────────────────────────────────────────────────
// Turns the culled + sorted visible objects into draw calls. It produces no
// GPU work itself — the backend replays the resulting command buffer with its
// own pipeline + per-frame state (camera, lights). This is the only pass wired
// into the default graph today.
void GeometryPass::execute(const RenderWorld&           world,
                           const std::vector<uint32_t>& sortedIndices,
                           CommandBuffer&               outCmds)
{
	for (uint32_t idx : sortedIndices)
	{
		if (idx >= world.objects.size())
			continue;

		const RenderObject& obj = world.objects[idx];
		DrawCall dc;
		dc.meshAssetId     = obj.meshAssetId;
		dc.materialAssetId = obj.materialAssetId;
		dc.mesh          = obj.meshHandle;
		dc.material      = obj.materialHandle;
		dc.transform     = obj.transform;
		dc.instanceCount = 1;
		dc.entityId      = obj.entityId;
		dc.lod           = obj.lod;
		outCmds.recordDraw(dc);
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
		dc.boneMatrices  = obj.boneMatrices;
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
