#pragma once
// ─── GpuPassTimeline ─────────────────────────────────────────────────
// The bookkeeping behind per-pass GPU timing on the backends that measure with
// timestamps (D3D11 timestamp queries, D3D12 query heap, Vulkan query pool): which
// timestamp index belongs to which pass of one frame, and how the resolved ticks
// become IRenderer::GpuPassTime rows. The API-specific half (writing a timestamp,
// reading the results) stays in each backend; this is the part that was about to
// be written three times.
//
// The frame is a stream of marks, one timestamp each. A pass owns the stretch from
// the mark it was entered at to the next mark; a pass entered inside another one
// (a Decals scope inside the Scene) interrupts it, and the outer one carries on
// from the mark the inner one ends at, so every pass reads exclusive time and the
// rows add up to the frame. What runs outside every scope belongs to no row. A
// frame has kMaxTimestamps marks; a scope that would leave too few for the marks
// that close the open ones is not entered (left out, never wrong).
//
// Header-only so HE_Core, the backends and the tests share one copy.

#include "Renderer/IRenderer.h"
#if defined(HE_PROFILING_ENABLED)
#  include "Diagnostics/EngineProfiler.h"   // ProfileScope: the CPU side of a pass scope
#  include <optional>
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

class GpuPassTimeline
{
public:
	static constexpr uint32_t kMaxTimestamps = 128;
	static constexpr uint32_t kMaxDepth      = 4;

	// HE_GPU_PASS_TIMING=0 turns the per-pass marks off (the whole-frame time stays): a timestamp
	// between two passes makes the GPU drain to it, which can cost a frame whose GPU side is the
	// slow one, so the A/B of a CPU change measures without them. Read once.
	static bool envEnabled()
	{
		static const bool on = [] { const char* v = std::getenv("HE_GPU_PASS_TIMING"); return !(v && v[0] == '0'); }();
		return on;
	}

	// A new frame. The first mark (index 0) is the frame's start: write a timestamp
	// at the index start() hands out.
	void reset() { m_names.clear(); m_stack.clear(); }
	uint32_t start() { m_names.push_back(nullptr); return 0; }

	// Enter the pass `name` (a static literal): write a timestamp at `idx`.
	bool push(const char* name, uint32_t& idx)
	{
		const uint32_t depth = static_cast<uint32_t>(m_stack.size());
		if (m_names.empty() || depth >= kMaxDepth
		    || m_names.size() + 2 * (depth + 1) + 1 > kMaxTimestamps)
			return false;
		m_stack.push_back(name);
		idx = static_cast<uint32_t>(m_names.size());
		m_names.push_back(name);
		return true;
	}
	// Leave the innermost pass: write a timestamp at `idx`; the pass it was entered
	// from (or none) continues from there.
	bool pop(uint32_t& idx)
	{
		if (m_stack.empty()) return false;
		m_stack.pop_back();
		idx = static_cast<uint32_t>(m_names.size());
		m_names.push_back(m_stack.empty() ? nullptr : m_stack.back());
		return true;
	}
	// The frame's last mark: write a timestamp at `idx` (any pass still open ends here).
	uint32_t finish()
	{
		m_stack.clear();
		m_names.push_back(nullptr);
		return static_cast<uint32_t>(m_names.size() - 1);
	}

	uint32_t used() const { return static_cast<uint32_t>(m_names.size()); }
	bool     empty() const { return m_names.size() <= 1; }

	// The resolved timestamps (ticks[i] = mark i, `ticksPerSecond` their clock) as one
	// row per pass name, in the order the passes first ran; a pass that ran in several
	// stretches adds them up. A clock that ran backwards reads 0.
	void toPasses(const uint64_t* ticks, double ticksPerSecond,
	              std::vector<IRenderer::GpuPassTime>& out) const
	{
		out.clear();
		for (size_t i = 0; i + 1 < m_names.size(); ++i)
		{
			const char* name = m_names[i];
			if (!name) continue;
			const uint64_t t0 = ticks[i], t1 = ticks[i + 1];
			const double   ms = (t1 > t0 && ticksPerSecond > 0.0)
			                  ? static_cast<double>(t1 - t0) * 1000.0 / ticksPerSecond : 0.0;
			IRenderer::GpuPassTime* row = nullptr;
			for (IRenderer::GpuPassTime& r : out)
				if (std::strcmp(r.name, name) == 0) { row = &r; break; }
			if (row) row->ms += ms;
			else     out.push_back({ name, ms, /*approx=*/false });
		}
	}

private:
	std::vector<const char*> m_names;   // m_names[i]: the pass the stretch after mark i belongs to
	std::vector<const char*> m_stack;   // the passes entered and not left
};

// A pass's timing scope: enters at construction, leaves at destruction. `Owner`
// supplies bool gpuPassBegin(const char*) (false = not timing, nothing written)
// and void gpuPassEnd(). Null owner = no GPU timing.
//
// The same scope is the pass's CPU scope too (the EngineProfiler's, same name): what the
// CPU spends recording it, which is the cost of a frame that is CPU-bound and the GPU
// rows cannot show. CPU scopes are inclusive (the Scene's contains the SSAO nested in it),
// the GPU rows are exclusive.
template <class Owner>
class GpuPassScopeT
{
public:
	GpuPassScopeT(Owner* owner, const char* name) : m_owner(owner), m_on(owner && owner->gpuPassBegin(name)) { cpuEnter(name); }
	~GpuPassScopeT() { if (m_on) m_owner->gpuPassEnd(); }
	GpuPassScopeT(const GpuPassScopeT&)            = delete;
	GpuPassScopeT& operator=(const GpuPassScopeT&) = delete;
	// Leave this pass and enter `name` in the same scope: a function that runs several
	// stages in a row times each without a block around it.
	void next(const char* name)
	{
		if (!m_owner) return;
		if (m_on) m_owner->gpuPassEnd();
		m_on = m_owner->gpuPassBegin(name);
		cpuEnter(name);
	}
private:
#if defined(HE_PROFILING_ENABLED)
	void cpuEnter(const char* name) { m_cpu.reset(); m_cpu.emplace(name); }
	std::optional<ProfileScope> m_cpu;
#else
	void cpuEnter(const char*) {}
#endif
	Owner* m_owner;
	bool   m_on;
};
