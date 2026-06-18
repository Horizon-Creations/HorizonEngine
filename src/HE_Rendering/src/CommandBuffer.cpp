#include "HorizonRendering/CommandBuffer.h"

void CommandBuffer::reset()
{
	drawCalls_.clear();
	skinnedDrawCalls_.clear();
	postProcess_ = false;
}

void CommandBuffer::recordPostProcess()
{
	postProcess_ = true;
}

void CommandBuffer::recordDraw(const DrawCall& call)
{
	drawCalls_.push_back(call);
}

void CommandBuffer::recordSkinnedDraw(const SkinnedDrawCall& call)
{
	skinnedDrawCalls_.push_back(call);
}

const std::vector<DrawCall>& CommandBuffer::drawCalls() const
{
	return drawCalls_;
}

const std::vector<SkinnedDrawCall>& CommandBuffer::skinnedDrawCalls() const
{
	return skinnedDrawCalls_;
}
