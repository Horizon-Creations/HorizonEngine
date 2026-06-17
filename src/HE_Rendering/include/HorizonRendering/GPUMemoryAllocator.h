#pragma once
#include <Types/Handle.h>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <functional>

// Tracks GPU memory budget with LRU-eviction signalling.
// The allocator does NOT call GPU APIs — it only tracks byte budgets and
// LRU order. The RenderResourceManager supplies the eviction callback which
// actually tears down the GPU resource.
class GPUMemoryAllocator {
public:
    using EvictCallback = std::function<void(RenderHandle)>;

    explicit GPUMemoryAllocator(uint64_t budgetBytes);

    // Register a new allocation. Returns false if the allocation would
    // exceed the budget even after evicting all LRU candidates.
    bool requestAllocation(uint64_t sizeBytes, RenderHandle handle);

    void freeAllocation(RenderHandle handle);

    // Call every time a handle is used (draw call, bind, etc.) to keep it warm.
    void onHandleUsed(RenderHandle handle);

    // Evict the single least-recently-used handle. Does nothing if empty.
    // If an eviction callback is set, it is called before the entry is removed.
    void evictLRU();

    // Optional callback called by evictLRU before removing the entry.
    void setEvictCallback(EvictCallback cb) { m_evictCb = std::move(cb); }

    uint64_t usedBytes()   const { return m_used; }
    uint64_t totalBudget() const { return m_budget; }
    size_t   entryCount()  const { return m_sizes.size(); }

private:
    static uint64_t encodeHandle(RenderHandle h) noexcept
    {
        return (static_cast<uint64_t>(h.generation) << 32) | h.index;
    }

    uint64_t m_budget;
    uint64_t m_used = 0;

    std::unordered_map<uint64_t, uint64_t>                        m_sizes;    // handle → bytes
    std::list<uint64_t>                                            m_lruList;  // MRU front, LRU back
    std::unordered_map<uint64_t, std::list<uint64_t>::iterator>   m_lruIndex; // O(1) position

    EvictCallback m_evictCb;
};
