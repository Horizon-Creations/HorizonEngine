#include "HorizonRendering/CommandBuffer.h"

void CommandBuffer::reset()
{
	drawCalls_.clear();
}

void CommandBuffer::recordDraw(const DrawCall& call)
{
	drawCalls_.push_back(call);
}

const std::vector<DrawCall>& CommandBuffer::drawCalls() const
{
	return drawCalls_;
}
