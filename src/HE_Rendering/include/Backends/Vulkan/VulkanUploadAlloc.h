#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// The two offset allocators behind VulkanRenderer's asset upload path
// (REN-802/803, docs/audit/modul_rendering.md):
//
//   StagingRing     — the persistent, mapped staging buffer. Every upload of a
//                     batch takes its bytes from here; a batch's bytes come back
//                     when its fence has signalled (retire(ticket)). Before, each
//                     upload made its own staging buffer and waited for the
//                     queue to go idle so it could free it.
//   RangeAllocator  — first-fit free list over one big DEVICE_LOCAL
//                     VkDeviceMemory block. Mesh buffers are bound at an offset
//                     into such a block instead of getting a vkAllocateMemory of
//                     their own (maxMemoryAllocationCount is 4096 on most drivers).
//
// No <vulkan/vulkan.h> here: he_tests builds on macOS/Linux without the Vulkan
// SDK, and CI has no Vulkan device, so this arithmetic is the part of the upload
// path a test can hold to account (tests/test_vulkan_upload_alloc.cpp).
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>

namespace HE::vkup
{
    inline constexpr uint64_t kFail = ~uint64_t(0);

    // align must be a power of two (Vulkan alignments are).
    inline uint64_t alignUp(uint64_t v, uint64_t align)
    {
        return align > 1 ? (v + align - 1) & ~(align - 1) : v;
    }

    // Byte ring with retire tickets. Positions are virtual (monotonic 64-bit);
    // the physical offset is pos % capacity. An allocation never straddles the
    // end: it skips to the next lap instead, and the skipped tail belongs to the
    // allocation's ticket, so it frees with it. Tickets must not decrease from
    // one allocate() to the next (a batch id that only grows).
    class StagingRing
    {
    public:
        StagingRing() = default;
        explicit StagingRing(uint64_t capacity) { reset(capacity); }

        void reset(uint64_t capacity)
        {
            m_cap = capacity; m_head = m_tail = 0; m_spans.clear();
        }

        uint64_t capacity() const { return m_cap; }
        uint64_t used()     const { return m_head - m_tail; }

        // Physical offset of `size` bytes aligned to `align`, or kFail when the
        // ring has no room until something retires (or size > capacity — the
        // caller then stages through a buffer of its own). `align` must divide
        // the capacity, or a lap boundary would land misaligned.
        uint64_t allocate(uint64_t size, uint64_t align, uint64_t ticket)
        {
            if (size == 0 || size > m_cap || (align > 1 && m_cap % align != 0)) return kFail;
            uint64_t pos = alignUp(m_head, align);
            if (pos % m_cap + size > m_cap) pos = (pos / m_cap + 1) * m_cap; // next lap, offset 0
            if (pos + size - m_tail > m_cap) return kFail;
            m_head = pos + size;
            if (!m_spans.empty() && m_spans.back().ticket == ticket) m_spans.back().end = m_head;
            else m_spans.push_back({ ticket, m_head });
            return pos % m_cap;
        }

        // Every allocation whose ticket is <= `completed` is done with.
        void retire(uint64_t completed)
        {
            while (!m_spans.empty() && m_spans.front().ticket <= completed)
            {
                m_tail = m_spans.front().end;
                m_spans.pop_front();
            }
            // Nothing live: start over at offset 0, so a big upload after an idle
            // stretch is not refused for the lap's tail end it would have to skip.
            if (m_spans.empty()) m_head = m_tail = 0;
        }

    private:
        struct Span { uint64_t ticket; uint64_t end; };
        uint64_t         m_cap  = 0;
        uint64_t         m_head = 0;   // next free virtual position
        uint64_t         m_tail = 0;   // oldest live virtual position
        std::deque<Span> m_spans;      // per ticket: where its bytes end
    };

    // First-fit free list over [0, capacity). An allocation occupies exactly
    // [offset, offset + size); the alignment gap in front of it stays free, so
    // free() takes the same (offset, size) that allocate() was asked for and
    // handed back. Neighbouring free ranges are merged.
    class RangeAllocator
    {
    public:
        RangeAllocator() = default;
        explicit RangeAllocator(uint64_t capacity) { reset(capacity); }

        void reset(uint64_t capacity)
        {
            m_cap = capacity; m_used = 0; m_free.clear();
            if (capacity) m_free.emplace(0, capacity);
        }

        uint64_t capacity() const { return m_cap; }
        uint64_t used()     const { return m_used; }
        bool     empty()    const { return m_used == 0; }
        size_t   freeRanges() const { return m_free.size(); }

        uint64_t allocate(uint64_t size, uint64_t align)
        {
            if (size == 0) return kFail;
            for (auto it = m_free.begin(); it != m_free.end(); ++it)
            {
                const uint64_t start = it->first, end = it->first + it->second;
                const uint64_t off   = alignUp(start, align);
                if (off < start || off > end || end - off < size) continue;
                m_free.erase(it);
                if (off > start)       m_free.emplace(start, off - start);
                if (off + size < end)  m_free.emplace(off + size, end - (off + size));
                m_used += size;
                return off;
            }
            return kFail;
        }

        void free(uint64_t offset, uint64_t size)
        {
            if (size == 0) return;
            m_used -= size;
            auto next = m_free.lower_bound(offset);
            if (next != m_free.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == offset)
                {
                    offset = prev->first; size += prev->second;
                    m_free.erase(prev);
                }
            }
            if (next != m_free.end() && offset + size == next->first)
            {
                size += next->second;
                m_free.erase(next);
            }
            m_free.emplace(offset, size);
        }

    private:
        uint64_t                     m_cap  = 0;
        uint64_t                     m_used = 0;
        std::map<uint64_t, uint64_t> m_free;   // start → length
    };
}
