#include "HorizonRendering/RenderResourceManager.h"

RenderResourceManager::RenderResourceManager(GPUMemoryAllocator& allocator)
    : allocator_(allocator)
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
    auto it = assetIndex_.find(assetId);
    if (it != assetIndex_.end()) return it->second; // already uploaded

    RenderHandle h = nextHandle();
    allocator_.requestAllocation(data.sizeBytes(), h);
    assetIndex_[assetId]     = h;
    handleToAsset_[encodeH(h)] = assetId;
    return h;
}

RenderHandle RenderResourceManager::uploadTexture(const HE::UUID& assetId, const TextureData& data)
{
    auto it = assetIndex_.find(assetId);
    if (it != assetIndex_.end()) return it->second;

    RenderHandle h = nextHandle();
    allocator_.requestAllocation(data.sizeBytes(), h);
    assetIndex_[assetId]     = h;
    handleToAsset_[encodeH(h)] = assetId;
    return h;
}

RenderHandle RenderResourceManager::createMaterial(const HE::UUID& assetId, const MaterialDesc& desc)
{
    auto it = assetIndex_.find(assetId);
    if (it != assetIndex_.end()) return it->second;

    RenderHandle h = nextHandle();
    allocator_.requestAllocation(desc.sizeBytes(), h);
    assetIndex_[assetId]     = h;
    handleToAsset_[encodeH(h)] = assetId;
    return h;
}

void RenderResourceManager::release(RenderHandle handle)
{
    if (!handle.isValid()) return;

    const uint64_t key = encodeH(handle);
    auto hit = handleToAsset_.find(key);
    if (hit == handleToAsset_.end()) return;

    assetIndex_.erase(hit->second);
    handleToAsset_.erase(hit);
    allocator_.freeAllocation(handle);
}

RenderHandle RenderResourceManager::findHandle(const HE::UUID& assetId) const
{
    auto it = assetIndex_.find(assetId);
    return it != assetIndex_.end() ? it->second : RenderHandle::invalid();
}

bool RenderResourceManager::isLoaded(const HE::UUID& assetId) const
{
    return assetIndex_.count(assetId) > 0;
}
