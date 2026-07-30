#include "HorizonRendering/RenderGraph.h"
#include <cstdint>

void RenderGraph::addPass(std::unique_ptr<RenderPass> pass)
{
	if (pass)
		m_passes.push_back(std::move(pass));
}

void RenderGraph::execute(const RenderWorld&           world,
                          const std::vector<uint32_t>& sortedIndices,
                          const PassSink&              sink)
{
	// Each pass records into the scratch buffer on its own, then the backend
	// binds the pass's declared target and replays it. Passes run in order.
	for (const auto& pass : m_passes)
	{
		m_scratch.reset();
		pass->execute(world, sortedIndices, m_scratch);
		if (sink) sink(*pass, pass->describe(), m_scratch);
	}
}

void RenderGraph::execute(const RenderWorld&           world,
                          const std::vector<uint32_t>& sortedIndices,
                          CommandBuffer&               outCmds)
{
	// Passes run in insertion order, each appending to the shared command
	// buffer. Reset once up front so the buffer holds exactly this frame.
	outCmds.reset();
	for (const auto& pass : m_passes)
		pass->execute(world, sortedIndices, outCmds);
}

void RenderGraph::clear()
{
	m_passes.clear();
}
