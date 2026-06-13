#include "HorizonRendering/RenderGraph.h"

void RenderGraph::addPass(std::unique_ptr<RenderPass> pass)
{
	if (pass)
		passes_.push_back(std::move(pass));
}

void RenderGraph::execute(const RenderWorld&           world,
                          const std::vector<uint32_t>& sortedIndices,
                          CommandBuffer&               outCmds)
{
	// Passes run in insertion order, each appending to the shared command
	// buffer. Reset once up front so the buffer holds exactly this frame.
	outCmds.reset();
	for (const auto& pass : passes_)
		pass->execute(world, sortedIndices, outCmds);
}

void RenderGraph::clear()
{
	passes_.clear();
}
