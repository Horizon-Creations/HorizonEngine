#include "HorizonRendering/RenderPass.h"
#include "HorizonRendering/RenderObject.h"

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
		dc.meshAssetId   = obj.meshAssetId;
		dc.mesh          = obj.meshHandle;
		dc.material      = obj.materialHandle;
		dc.transform     = obj.transform;
		dc.instanceCount = 1;
		dc.entityId      = obj.entityId;
		dc.lod           = obj.lod;
		outCmds.recordDraw(dc);
	}
}

// ─── ShadowPass ─────────────────────────────────────────────────────────────
// Records the same visible geometry as GeometryPass; the backend replays it
// depth-only from the light's POV (world.shadow.viewProj) into the shadow map.
// Skips everything when no directional light is present.
void ShadowPass::execute(const RenderWorld&           world,
                         const std::vector<uint32_t>& sortedIndices,
                         CommandBuffer&               outCmds)
{
	if (!world.shadow.enabled) return;
	for (uint32_t idx : sortedIndices)
	{
		if (idx >= world.objects.size())
			continue;
		const RenderObject& obj = world.objects[idx];
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
// Inert until HDR (3.6): needs a fullscreen tonemap reading the scene color.
void PostProcessPass::execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) {}

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
