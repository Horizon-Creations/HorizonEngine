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
// Declared so the graph can reference them, but inert until the render-target
// plumbing lands: shadows (3.5) need a depth target rendered from the light's
// POV, HDR post (3.6) needs an offscreen color target + a fullscreen pass.
// Both require the backends to create and bind per-pass targets, which the
// current CPU-side command buffer does not yet model — added in a follow-up.
void ShadowPass::execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) {}
void PostProcessPass::execute(const RenderWorld&, const std::vector<uint32_t>&, CommandBuffer&) {}
