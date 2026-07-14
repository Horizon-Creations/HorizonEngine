#include "HorizonRendering/GPUMemoryAllocator.h"
#include <cstdint>

GPUMemoryAllocator::GPUMemoryAllocator(uint64_t budgetBytes)
    : m_budget(budgetBytes)
{
}

bool GPUMemoryAllocator::requestAllocation(uint64_t sizeBytes, RenderHandle handle)
{
    if (sizeBytes > m_budget) return false;

    const uint64_t key = encodeHandle(handle);
    auto it = m_sizes.find(key);
    if (it != m_sizes.end())
    {
        // Already tracked — update size in-place without touching LRU order.
        m_used -= it->second;
        it->second = sizeBytes;
    }
    else
    {
        m_sizes[key] = sizeBytes;
        m_lruList.push_front(key);
        m_lruIndex[key] = m_lruList.begin();
    }
    m_used += sizeBytes;
    return true;
}

void GPUMemoryAllocator::freeAllocation(RenderHandle handle)
{
    const uint64_t key = encodeHandle(handle);
    auto sit = m_sizes.find(key);
    if (sit == m_sizes.end()) return;

    m_used -= sit->second;
    m_sizes.erase(sit);

    auto lit = m_lruIndex.find(key);
    if (lit != m_lruIndex.end())
    {
        m_lruList.erase(lit->second);
        m_lruIndex.erase(lit);
    }
}

void GPUMemoryAllocator::onHandleUsed(RenderHandle handle)
{
    const uint64_t key = encodeHandle(handle);
    auto it = m_lruIndex.find(key);
    if (it == m_lruIndex.end()) return;

    // Move to front (most-recently-used position); splice is O(1).
    m_lruList.splice(m_lruList.begin(), m_lruList, it->second);
    it->second = m_lruList.begin();
}

void GPUMemoryAllocator::evictLRU()
{
    if (m_lruList.empty()) return;

    const uint64_t key = m_lruList.back();

    if (m_evictCb)
    {
        RenderHandle h;
        h.index      = static_cast<uint32_t>(key & 0xFFFFFFFFu);
        h.generation = static_cast<uint32_t>(key >> 32);
        m_evictCb(h);
    }

    auto sit = m_sizes.find(key);
    if (sit != m_sizes.end())
    {
        m_used -= sit->second;
        m_sizes.erase(sit);
    }
    m_lruIndex.erase(key);
    m_lruList.pop_back();
}
