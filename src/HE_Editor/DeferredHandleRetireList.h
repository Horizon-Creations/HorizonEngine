#pragma once
#include "DeferredSlotFreeList.h"
#include <cstdint>
#include <vector>

// ── Handles that must outlive the frames still reading them ──────────────────
// The Vulkan twin of the D3D12 SRV slot problem (Thema 113 → Thema 124). When
// the editor re-registers the viewport RT (Dock-Splitter drag → new offscreen
// image every frame) it used to drop the old ImGui descriptor set with
// ImGui_ImplVulkan_RemoveTexture, which is a vkFreeDescriptorSets on the spot —
// while the frame submitted just before, whose ImGui::Image names that set,
// was still running on the GPU. Freeing a set a pending command buffer uses is
// invalid ("vkFreeDescriptorSets … in use" in the validation layer); a driver
// may hand the set out again and the frame reads whatever lands in it.
//
// So a handle that is no longer wanted is not destroyed yet. It is parked with
// a fence-like serial the caller takes at the moment of the retire (Vulkan:
// VulkanRenderer::GetSubmittedFrameSerial), and reclaim() with the serial the
// GPU has completed destroys every handle whose serial has passed.
//
// The rule itself is DeferredSlotFreeList's, not a second copy of it: each
// parked handle sits in one table slot, retire() parks that slot behind the
// serial, and a slot the list hands out again after reclaim() while it still
// holds a handle is one whose frames are done. The table grows on demand, so
// there is no capacity to run out of — the ImGui pool is the only limit, and
// at most one set per frame in flight is ever parked.
//
// Kept free of ImGui and of Vulkan so the rule can be tested without either.
template <class Handle>
class DeferredHandleRetireList
{
public:
	// Destroy `h` once the completed serial has reached `serial`.
	void retire(Handle h, std::uint64_t serial)
	{
		if (h == Handle{}) return;
		int slot = -1;
		if (!m_slots.alloc(slot))
		{
			slot = static_cast<int>(m_handles.size());
			m_handles.push_back(Handle{});
		}
		m_handles[static_cast<size_t>(slot)] = h;
		m_slots.retire(slot, serial);
	}

	// Calls destroy(h) exactly once for every handle parked at or below
	// `completed`; the others stay parked.
	template <class Destroy>
	void reclaim(std::uint64_t completed, Destroy&& destroy)
	{
		const size_t before = m_slots.pendingCount();
		if (before == 0) return;
		m_slots.reclaim(completed);
		if (m_slots.pendingCount() == before) return;
		// Drain the free slots: those still holding a handle just came back.
		m_scratch.clear();
		int slot = -1;
		while (m_slots.alloc(slot))
		{
			Handle& h = m_handles[static_cast<size_t>(slot)];
			if (h != Handle{})
			{
				destroy(h);
				h = Handle{};
			}
			m_scratch.push_back(slot);
		}
		for (int s : m_scratch)
			m_slots.release(s);
	}

	// Forget every parked handle without destroying it — for shutdown, where
	// the owner of the handles (ImGui's descriptor pool) frees them wholesale.
	void clear()
	{
		m_slots.clear();
		m_handles.clear();
		m_scratch.clear();
	}

	bool   hasPending()   const { return m_slots.hasPending(); }
	size_t pendingCount() const { return m_slots.pendingCount(); }

private:
	DeferredSlotFreeList m_slots;
	std::vector<Handle>  m_handles;   // slot → parked handle; Handle{} = slot unused
	std::vector<int>     m_scratch;
};
