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

// ─── ShadowPass / PostProcessPass ───────────────────────────────────────────
// Declared so the graph can reference them, and they now declare their target
// I/O (so backends can wire them up), but they record no draws yet: shadows
// (3.5) need the geometry re-rendered from the light's POV into the depth
// target, HDR post (3.6) needs a fullscreen tonemap reading the scene color.
// Those land with the passes themselves.
void ShadowPass::execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) {}
void PostProcessPass::execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) {}

RenderPassIO ShadowPass::describe() const
{
    RenderPassIO io;
    io.output.id        = 1; // shadow map
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
    io.inputs[0]        = 2; // scene color (HDR)
    io.inputCount       = 1;
    return io;
}
