#include "HorizonRendering/RenderResourceManager.h"
#include <cstdint>

RenderResourceManager::RenderResourceManager(GPUMemoryAllocator& allocator)
    : m_allocator(allocator)
{
}

RenderHandle RenderResourceManager::nextHandle()
{
    RenderHandle h;
    h.index      = m_nextIndex++;
    h.generation = m_generation++;
    return h;
}

static uint64_t encodeH(RenderHandle h) noexcept
{
    return (static_cast<uint64_t>(h.generation) << 32) | h.index;
}

RenderHandle RenderResourceManager::uploadMesh(const HE::UUID& assetId, const MeshData& data)
{
    auto it = m_assetIndex.find(assetId);
    if (it != m_assetIndex.end()) return it->second; // already uploaded

    RenderHandle h = nextHandle();
    m_allocator.requestAllocation(data.sizeBytes(), h);
    m_assetIndex[assetId]     = h;
    m_handleToAsset[encodeH(h)] = assetId;
    return h;
}

RenderHandle RenderResourceManager::uploadTexture(const HE::UUID& assetId, const TextureData& data)
{
    auto it = m_assetIndex.find(assetId);
    if (it != m_assetIndex.end()) return it->second;

    RenderHandle h = nextHandle();
    m_allocator.requestAllocation(data.sizeBytes(), h);
    m_assetIndex[assetId]     = h;
    m_handleToAsset[encodeH(h)] = assetId;
    return h;
}

RenderHandle RenderResourceManager::createMaterial(const HE::UUID& assetId, const MaterialDesc& desc)
{
    auto it = m_assetIndex.find(assetId);
    if (it != m_assetIndex.end()) return it->second;

    RenderHandle h = nextHandle();
    m_allocator.requestAllocation(desc.sizeBytes(), h);
    m_assetIndex[assetId]     = h;
    m_handleToAsset[encodeH(h)] = assetId;
    return h;
}

void RenderResourceManager::release(RenderHandle handle)
{
    if (!handle.isValid()) return;

    const uint64_t key = encodeH(handle);
    auto hit = m_handleToAsset.find(key);
    if (hit == m_handleToAsset.end()) return;

    m_assetIndex.erase(hit->second);
    m_handleToAsset.erase(hit);
    m_allocator.freeAllocation(handle);
}

RenderHandle RenderResourceManager::findHandle(const HE::UUID& assetId) const
{
    auto it = m_assetIndex.find(assetId);
    return it != m_assetIndex.end() ? it->second : RenderHandle::invalid();
}

bool RenderResourceManager::isLoaded(const HE::UUID& assetId) const
{
    return m_assetIndex.count(assetId) > 0;
}
