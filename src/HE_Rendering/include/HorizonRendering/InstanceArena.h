#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

// ─── Instance arena: per-frame bookkeeping without a GPU ──────────────────────
// D3D12 and Vulkan give every instanced run of a frame its own {mvp, model}
// slots (each shadow cascade, the SSAO and GI pre-passes, the scene): the GPU
// reads them at submit, so nothing is reused inside a frame. Foliage therefore
// asks for several times its visible count. The backends keep one main buffer
// per frame slot and open further chunks when it is full (Thema 182, plan 2f).
//
// This class is only the sizing policy, backend-free so it can be tested
// without a device. The backend owns the buffers; per frame:
//
//     beginFrame(mainCap);                  // the slot's main buffer, idle again
//     uint32_t c = chunkFor(count);         // 0 = the current chunk takes it
//     if (c == kRefused) -> loop per object, refuse(count)
//     else if (c)        -> create a chunk of c instances, openChunk(c)
//                           (creation failed: refuse(count))
//     uint32_t first = take(count);         // slot index in the current chunk
//     ...
//     nextMainCapacity()                    // before the next beginFrame: what
//                                           // main must hold to take this frame whole
//
// Before Thema 182 step 8 the backends counted the CAPACITIES of the doubling
// chunks against a 1M cap: 64k + 128k + 256k + 512k = 983 040, and the next
// doubled chunk (1M) was refused although the frame needed only ~1.1M. At 500k
// foliage plants all in range D3D12 then ran its per-object loop into the
// k_maxDraws ring and drew no scene, Vulkan looped (290 ms a frame). Now the last
// chunk is clamped to what the budget has left, the next main buffer is sized
// from what the frame ASKED (taken plus refused, an eighth on top), and the
// budget is a byte figure sized for that demand, overridable for a run.
namespace HE
{

class InstanceArena
{
public:
	static constexpr uint32_t kRefused       = 0xFFFFFFFFu;
	static constexpr uint32_t kMinChunk      = 1024;
	static constexpr uint32_t kMainGranule   = 65536;   // main buffers grow in steps of this
	// 512 MiB of 128-byte {mvp, model} pairs per frame slot. A frame asks for
	// roughly visible × (scene + SSAO + GI + the cascades that see the plant):
	// ~1.1M at 500k plants all in range (measured on the RTX 4070, Thema 182
	// step 5), so this holds about 1.5-2M plants in range. Memory follows the
	// demand: a slot's buffers only grow as far as its frames asked.
	static constexpr uint64_t kDefaultBudgetBytes = uint64_t(512) << 20;

	// Instances per frame slot: HE_INST_ARENA_MIB (whole MiB) or the default,
	// never below one main granule.
	static uint32_t budgetFromEnv(uint32_t stride)
	{
		uint64_t bytes = kDefaultBudgetBytes;
		if (const char* e = std::getenv("HE_INST_ARENA_MIB"); e && *e)
		{
			const unsigned long long mib = std::strtoull(e, nullptr, 10);
			if (mib > 0) bytes = static_cast<uint64_t>(mib) << 20;
		}
		return budgetFor(bytes, stride);
	}
	static uint32_t budgetFor(uint64_t bytes, uint32_t stride)
	{
		const uint64_t n = bytes / std::max<uint32_t>(stride, 1);
		return static_cast<uint32_t>(std::clamp<uint64_t>(n, kMainGranule, 0x7FFFFFFFull));
	}

	explicit InstanceArena(uint32_t budget = kMainGranule * 16) : m_budget(std::max(budget, kMainGranule)) {}

	void     setBudget(uint32_t budget) { m_budget = std::max(budget, kMainGranule); }
	uint32_t budget() const { return m_budget; }

	// A new frame on a slot whose main buffer holds mainCap instances (0 = none:
	// every chunkFor refuses).
	void beginFrame(uint32_t mainCap)
	{
		m_mainCap  = mainCap;
		m_curCap   = mainCap;
		m_cursor   = 0;
		m_reserved = mainCap;
		m_taken    = 0;
		m_asked    = 0;
		m_refused  = 0;
	}

	// 0 = the current chunk takes `count`; kRefused = it does not and the budget
	// has no chunk left that would; otherwise the capacity of the chunk to open
	// first: twice the current one (at least kMinChunk, at least `count`),
	// clamped to what the budget has left.
	uint32_t chunkFor(uint32_t count) const
	{
		if (count == 0 || m_curCap == 0) return kRefused;
		if (static_cast<uint64_t>(m_cursor) + count <= m_curCap) return 0;
		uint64_t cap = static_cast<uint64_t>(std::max(m_curCap, kMinChunk)) * 2;
		while (cap < count) cap *= 2;
		const uint64_t left = m_budget > m_reserved ? m_budget - m_reserved : 0;
		cap = std::min(cap, left);
		return cap >= count ? static_cast<uint32_t>(cap) : kRefused;
	}

	// The backend created the chunk chunkFor asked for; it is the current one now.
	void openChunk(uint32_t cap)
	{
		m_reserved += cap;
		m_curCap    = cap;
		m_cursor    = 0;
	}

	// First slot of `count` in the current chunk (after chunkFor returned 0 or
	// openChunk).
	uint32_t take(uint32_t count)
	{
		const uint32_t first = m_cursor;
		m_cursor += count;
		m_taken  += count;
		m_asked  += count;
		return first;
	}

	// `count` instances the arena did not take: their run draws per object.
	void refuse(uint32_t count)
	{
		m_refused += count;
		m_asked   += count;
	}

	uint32_t taken()    const { return m_taken; }
	uint32_t refused()  const { return m_refused; }
	uint32_t asked()    const { return m_asked; }
	uint32_t reserved() const { return m_reserved; } // main + chunks of this frame, in instances
	bool     overflowed() const { return m_reserved > m_mainCap || m_refused > 0; }

	// What a main buffer should hold so that a frame like this one fits in it
	// whole: this frame's main while it did, else the asked count plus an eighth
	// (the camera moves), rounded up to kMainGranule, at most the budget. Never
	// smaller than this frame's main (buffers do not shrink).
	uint32_t nextMainCapacity() const
	{
		if (m_asked <= m_mainCap) return m_mainCap;
		uint64_t want = static_cast<uint64_t>(m_asked) + m_asked / 8;
		want = (want + kMainGranule - 1) / kMainGranule * kMainGranule;
		want = std::min<uint64_t>(want, m_budget);
		return static_cast<uint32_t>(std::max<uint64_t>(want, m_mainCap));
	}

private:
	uint32_t m_budget;
	uint32_t m_mainCap  = 0;
	uint32_t m_curCap   = 0;
	uint32_t m_cursor   = 0;
	uint32_t m_reserved = 0;
	uint32_t m_taken    = 0;
	uint32_t m_asked    = 0;
	uint32_t m_refused  = 0;
};

} // namespace HE
