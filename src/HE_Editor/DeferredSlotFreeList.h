#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// ── A slot free-list that does not hand a slot back while the GPU may read it ──
// The D3D12 editor keeps its ImGui SRVs in one fixed shader-visible descriptor
// heap and hands slots out from a free-list. That list used to be a plain LIFO
// stack, and the viewport re-registration (Dock-Splitter drag → new offscreen RT
// every frame) does "free my slot, allocate a slot, write the new SRV into it" —
// which got the SAME slot back and rewrote its descriptor while the frame
// submitted just before, whose ImGui::Image still names that slot, was running
// on the GPU. A descriptor in a shader-visible heap must not change while a
// command list that references it executes; what that frame samples is then
// undefined — on screen, a viewport frame with the wrong content while dragging
// (Thema 113, H2 in docs/d3d12-imgui-flicker-befund.md).
//
// So a freed slot is not free yet. It is parked with the value of a GPU fence
// that the caller signals on its queue at the moment of the free; the fence
// passes that value only once everything submitted before the free has
// executed. reclaim() with the fence's completed value moves every slot whose
// value has passed back into the list. A fence rather than a frame count: the
// editor does not render every iteration (minimised, background), and a count
// of editor frames says nothing about how far the GPU got.
//
// Kept free of ImGui and of D3D12 so the rule can be tested without either.
class DeferredSlotFreeList
{
public:
	// All `count` slots free; alloc() hands out 0, 1, 2, … first, the order the
	// old LIFO stack had, so a fresh heap is laid out exactly as before.
	void reset(int count)
	{
		m_free.clear();
		m_pending.clear();
		m_free.reserve(static_cast<size_t>(count));
		for (int n = count - 1; n >= 0; --n)
			m_free.push_back(n);
	}

	void clear()
	{
		m_free.clear();
		m_pending.clear();
	}

	// False when no slot is free right now (pending ones do not count — the
	// caller decides whether to wait for them, see oldestPending()).
	bool alloc(int& out)
	{
		if (m_free.empty()) return false;
		out = m_free.back();
		m_free.pop_back();
		return true;
	}

	// Free `slot` once the fence has reached `fenceValue`.
	void retire(int slot, std::uint64_t fenceValue)
	{
		m_pending.push_back({ slot, fenceValue });
	}

	// Free `slot` right away — only for a slot no submitted work can reference
	// (or when there is no fence to wait on).
	void release(int slot)
	{
		m_free.push_back(slot);
	}

	// Everything parked at or below `completedValue` is free again.
	void reclaim(std::uint64_t completedValue)
	{
		size_t kept = 0;
		for (size_t i = 0; i < m_pending.size(); ++i)
		{
			if (m_pending[i].fenceValue <= completedValue)
				m_free.push_back(m_pending[i].slot);
			else
				m_pending[kept++] = m_pending[i];
		}
		m_pending.resize(kept);
	}

	bool hasPending() const { return !m_pending.empty(); }

	// The smallest fence value anything is waiting for; 0 when nothing is.
	std::uint64_t oldestPending() const
	{
		std::uint64_t v = 0;
		for (const Pending& p : m_pending)
			if (v == 0 || p.fenceValue < v) v = p.fenceValue;
		return v;
	}

	size_t freeCount() const { return m_free.size(); }
	size_t pendingCount() const { return m_pending.size(); }

private:
	struct Pending
	{
		int           slot;
		std::uint64_t fenceValue;
	};
	std::vector<int>     m_free;
	std::vector<Pending> m_pending;
};
