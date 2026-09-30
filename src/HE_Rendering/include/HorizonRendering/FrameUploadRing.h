#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// ─── Per-frame upload ring: bookkeeping without a GPU ─────────────────────────
// The Metal backend used to upload every per-frame array (instance matrices per
// batch, the clustered-light lists, the debug lines) into a FRESH MTLBuffer, i.e.
// hundreds of kernel buffer allocations per frame (perf audit 2026-09-27, B6).
// This class hands out sub-ranges of a few large, long-lived chunks instead and
// recycles a frame's chunks once the GPU has finished that frame.
//
// It is only the bookkeeping, backend-free so it can be tested without a device:
// chunks are indices, the backend owns the real buffers (chunk i ↔ its i-th
// buffer, created with chunkCapacity(i) bytes the first time allocate() returns
// an index it has not seen). The lifecycle per frame:
//
//     beginFrame();                     // main thread, before encoding
//     Slice s = allocate(bytes);        // any number, main thread
//     Ticket t = endFrame();            // main thread, before the commit
//     ... GPU runs the frame ...
//     release(t);                       // ANY thread (a completion handler)
//
// Frame-in-flight tracking is the completion handler, not a frame count, so it
// holds however far the GPU falls behind: a chunk is only ever reused after the
// command buffer that read it completed. The pool therefore grows to the peak
// in-flight upload volume and stays there (chunks are never freed before the
// ring is destroyed).
//
// A frame that is begun but never ended (an early return before the commit)
// hands its chunks straight back on the next beginFrame(): an uncommitted
// command buffer never reaches the GPU, so nothing can still be reading them.
namespace HE
{

class FrameUploadRing
{
public:
	static constexpr uint32_t kNoChunk = 0xFFFFFFFFu;

	struct Slice
	{
		uint32_t chunk  = kNoChunk;   // kNoChunk = no frame open, caller falls back
		size_t   offset = 0;          // byte offset inside the chunk, aligned
		bool valid() const { return chunk != kNoChunk; }
	};
	// The chunks one frame used, handed back through release() when it completed.
	using Ticket = std::vector<uint32_t>;

	// chunkSize: capacity of a regular chunk; a larger request gets a chunk of
	// its own size. alignment: every slice offset is a multiple of it (a power of
	// two; 256 satisfies every Metal buffer-binding offset rule, Intel/AMD Macs
	// included).
	explicit FrameUploadRing(size_t chunkSize = size_t(1) << 20, size_t alignment = 256)
		: m_chunkSize(std::max<size_t>(chunkSize, 1)), m_alignment(std::max<size_t>(alignment, 1)) {}

	FrameUploadRing(const FrameUploadRing&) = delete;
	FrameUploadRing& operator=(const FrameUploadRing&) = delete;

	void beginFrame()
	{
		if (m_open) release(m_frame);   // never committed → never on the GPU
		m_frame.clear();
		m_current = kNoChunk;
		m_used    = 0;
		m_open    = true;
	}

	bool frameOpen() const { return m_open; }

	// A slice of `bytes` (0 is treated as 1) inside some chunk. Outside an open
	// frame the result is invalid: there is no command buffer whose completion
	// could return the space, so the caller must allocate on its own.
	Slice allocate(size_t bytes)
	{
		if (!m_open) return {};
		bytes = std::max<size_t>(bytes, 1);
		if (m_current != kNoChunk)
		{
			const size_t at = alignUp(m_used);
			if (at + bytes <= m_capacity[m_current])
			{
				m_used = at + bytes;
				return { m_current, at };
			}
		}
		m_current = acquire(bytes);
		m_frame.push_back(m_current);
		m_used = bytes;
		return { m_current, 0 };
	}

	// Closes the frame. The ticket must reach release() exactly once, after the
	// last command buffer that read the frame's slices completed.
	Ticket endFrame()
	{
		Ticket t;
		t.swap(m_frame);
		m_current = kNoChunk;
		m_used    = 0;
		m_open    = false;
		return t;
	}

	// Thread-safe: returns a completed frame's chunks to the free list.
	void release(const Ticket& chunks)
	{
		if (chunks.empty()) return;
		std::lock_guard<std::mutex> lk(m_freeMutex);
		m_free.insert(m_free.end(), chunks.begin(), chunks.end());
	}

	// Main thread. Every index allocate() ever returned is < chunkCount().
	size_t   chunkCount() const             { return m_capacity.size(); }
	size_t   chunkCapacity(uint32_t i) const { return i < m_capacity.size() ? m_capacity[i] : 0; }
	size_t   alignment() const              { return m_alignment; }
	size_t   freeChunkCount() const
	{
		std::lock_guard<std::mutex> lk(m_freeMutex);
		return m_free.size();
	}

private:
	size_t alignUp(size_t v) const { return (v + m_alignment - 1) / m_alignment * m_alignment; }

	// The smallest free chunk that fits, else a new one.
	uint32_t acquire(size_t bytes)
	{
		{
			std::lock_guard<std::mutex> lk(m_freeMutex);
			size_t best = m_free.size();
			for (size_t i = 0; i < m_free.size(); ++i)
			{
				const size_t cap = m_capacity[m_free[i]];
				if (cap >= bytes && (best == m_free.size() || cap < m_capacity[m_free[best]]))
					best = i;
			}
			if (best != m_free.size())
			{
				const uint32_t c = m_free[best];
				m_free[best] = m_free.back();
				m_free.pop_back();
				return c;
			}
		}
		m_capacity.push_back(std::max(m_chunkSize, alignUp(bytes)));
		return static_cast<uint32_t>(m_capacity.size() - 1);
	}

	const size_t m_chunkSize;
	const size_t m_alignment;
	// Written on the main thread only; release() never reads it, so the lock
	// covers m_free alone.
	std::vector<size_t> m_capacity;
	Ticket   m_frame;                 // chunks the open frame uses, in order
	uint32_t m_current = kNoChunk;    // chunk being filled
	size_t   m_used    = 0;           // bytes used in m_current
	bool     m_open    = false;

	mutable std::mutex    m_freeMutex;
	std::vector<uint32_t> m_free;
};

} // namespace HE
