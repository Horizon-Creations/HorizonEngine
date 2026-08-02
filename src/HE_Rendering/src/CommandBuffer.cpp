#include "HorizonRendering/CommandBuffer.h"

void CommandBuffer::reset()
{
	m_drawCalls.clear();
	m_skinnedDrawCalls.clear();
	m_postProcess = false;
}

void CommandBuffer::recordPostProcess()
{
	m_postProcess = true;
}

void CommandBuffer::recordDraw(const DrawCall& call)
{
	m_drawCalls.push_back(call);
}

void CommandBuffer::recordSkinnedDraw(const SkinnedDrawCall& call)
{
	m_skinnedDrawCalls.push_back(call);
}

const std::vector<DrawCall>& CommandBuffer::drawCalls() const
{
	return m_drawCalls;
}

const std::vector<SkinnedDrawCall>& CommandBuffer::skinnedDrawCalls() const
{
	return m_skinnedDrawCalls;
}
