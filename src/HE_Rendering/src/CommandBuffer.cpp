#include "HorizonRendering/CommandBuffer.h"
#include <algorithm>
#include <functional>
#include <utility>

CommandBuffer::CommandBuffer(const CommandBuffer& other)
	: m_drawCalls(other.m_drawCalls)
	, m_skinnedDrawCalls(other.m_skinnedDrawCalls)
	, m_matrices(other.m_matrices)
	, m_postProcess(other.m_postProcess)
{
	rebase(&other.m_matrices);
}

CommandBuffer::CommandBuffer(CommandBuffer&& other) noexcept
	: m_drawCalls(std::move(other.m_drawCalls))
	, m_skinnedDrawCalls(std::move(other.m_skinnedDrawCalls))
	, m_matrices(std::move(other.m_matrices))
	, m_postProcess(other.m_postProcess)
{
	rebase(&other.m_matrices);
}

CommandBuffer& CommandBuffer::operator=(const CommandBuffer& other)
{
	if (this == &other) return *this;
	m_drawCalls        = other.m_drawCalls;
	m_skinnedDrawCalls = other.m_skinnedDrawCalls;
	m_matrices         = other.m_matrices;
	m_postProcess      = other.m_postProcess;
	rebase(&other.m_matrices);
	return *this;
}

CommandBuffer& CommandBuffer::operator=(CommandBuffer&& other) noexcept
{
	if (this == &other) return *this;
	m_drawCalls        = std::move(other.m_drawCalls);
	m_skinnedDrawCalls = std::move(other.m_skinnedDrawCalls);
	m_matrices         = std::move(other.m_matrices);
	m_postProcess      = other.m_postProcess;
	rebase(&other.m_matrices);
	return *this;
}

// The windows a copy or move brought along still name the source's arena.
void CommandBuffer::rebase(const std::vector<glm::mat4>* from)
{
	auto fix = [&](HE::Mat4Range& r) { if (r.m_arena == from) r.m_arena = &m_matrices; };
	for (DrawCall& dc : m_drawCalls) fix(dc.instanceTransforms);
	for (SkinnedDrawCall& dc : m_skinnedDrawCalls)
	{
		fix(dc.instanceTransforms);
		fix(dc.boneMatrices);
	}
}

void CommandBuffer::reset()
{
	m_drawCalls.clear();
	m_skinnedDrawCalls.clear();
	m_matrices.clear();
	m_postProcess = false;
}

void CommandBuffer::recordPostProcess()
{
	m_postProcess = true;
}

HE::Mat4Range CommandBuffer::storeMatrices(const glm::mat4* matrices, size_t count)
{
	if (count == 0 || !matrices) return {};
	const size_t first = m_matrices.size();
	// The source may be a window onto this very arena; growing it moves the
	// storage, so note the offset first and copy from where it lands.
	const glm::mat4* base   = m_matrices.data();
	const bool       inside = std::greater_equal<const glm::mat4*>()(matrices, base)
	                       && std::less<const glm::mat4*>()(matrices, base + first);
	const size_t     offset = inside ? static_cast<size_t>(matrices - base) : 0;
	m_matrices.resize(first + count);
	std::copy_n(inside ? m_matrices.data() + offset : matrices, count, m_matrices.data() + first);
	return HE::Mat4Range(&m_matrices, static_cast<uint32_t>(first), static_cast<uint32_t>(count));
}

HE::Mat4Range CommandBuffer::matrixRange(size_t first, size_t count) const
{
	if (count == 0 || first >= m_matrices.size()) return {};
	const size_t n = std::min(count, m_matrices.size() - first);
	return HE::Mat4Range(&m_matrices, static_cast<uint32_t>(first), static_cast<uint32_t>(n));
}

void CommandBuffer::adopt(HE::Mat4Range& range)
{
	if (range.empty() || range.m_arena == &m_matrices) return;
	range = storeMatrices(range.data(), range.size());
}

void CommandBuffer::recordDraw(const DrawCall& call)
{
	m_drawCalls.push_back(call);
	adopt(m_drawCalls.back().instanceTransforms);
}

void CommandBuffer::recordDraw(DrawCall&& call)
{
	m_drawCalls.push_back(std::move(call));
	adopt(m_drawCalls.back().instanceTransforms);
}

void CommandBuffer::recordSkinnedDraw(const SkinnedDrawCall& call)
{
	m_skinnedDrawCalls.push_back(call);
	adopt(m_skinnedDrawCalls.back().instanceTransforms);
	adopt(m_skinnedDrawCalls.back().boneMatrices);
}

void CommandBuffer::recordSkinnedDraw(SkinnedDrawCall&& call)
{
	m_skinnedDrawCalls.push_back(std::move(call));
	adopt(m_skinnedDrawCalls.back().instanceTransforms);
	adopt(m_skinnedDrawCalls.back().boneMatrices);
}

const std::vector<DrawCall>& CommandBuffer::drawCalls() const
{
	return m_drawCalls;
}

const std::vector<SkinnedDrawCall>& CommandBuffer::skinnedDrawCalls() const
{
	return m_skinnedDrawCalls;
}
