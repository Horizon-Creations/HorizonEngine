#include "HorizonRendering/RenderGraph.h"
#include <cstdint>

void RenderGraph::addPass(std::unique_ptr<RenderPass> pass)
{
	if (pass)
		passes_.push_back(std::move(pass));
}

void RenderGraph::execute(const RenderWorld&           world,
                          const std::vector<uint32_t>& sortedIndices,
                          const PassSink&              sink)
{
	// Each pass records into the scratch buffer on its own, then the backend
	// binds the pass's declared target and replays it. Passes run in order.
	for (const auto& pass : passes_)
	{
		scratch_.reset();
		pass->execute(world, sortedIndices, scratch_);
		if (sink) sink(*pass, pass->describe(), scratch_);
	}
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
