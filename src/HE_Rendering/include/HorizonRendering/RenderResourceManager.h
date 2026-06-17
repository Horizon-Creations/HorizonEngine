#pragma once
#include <Types/Handle.h>
#include <Types/UUID.h>
#include "GPUMemoryAllocator.h"
#include <unordered_map>
#include <cstdint>

struct MeshData {
    uint64_t vertexBytes = 0;
    uint64_t indexBytes  = 0;
    uint64_t sizeBytes() const { return vertexBytes + indexBytes; }
};

struct TextureData {
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t channels = 4;     // bytes per pixel
    uint64_t sizeBytes() const { return static_cast<uint64_t>(width) * height * channels; }
};

struct MaterialDesc {
    uint64_t constantBufferBytes = 256;
    uint64_t sizeBytes() const { return constantBufferBytes; }
};

// CPU-side index that maps asset UUIDs to GPU RenderHandles and delegates
// budget tracking + LRU eviction to the GPUMemoryAllocator.
// Backends call uploadMesh/uploadTexture/createMaterial when they perform the
// actual GPU upload, and release() when they tear it down.
class RenderResourceManager {
public:
    explicit RenderResourceManager(GPUMemoryAllocator& allocator);

    RenderHandle uploadMesh    (const HE::UUID& assetId, const MeshData&     data);
    RenderHandle uploadTexture (const HE::UUID& assetId, const TextureData&  data);
    RenderHandle createMaterial(const HE::UUID& assetId, const MaterialDesc& desc);

    // Unregister a handle (backend has already released the GPU resource).
    void release(RenderHandle handle);

    // Lookup — returns invalid handle if not loaded.
    RenderHandle findHandle(const HE::UUID& assetId) const;
    bool         isLoaded  (const HE::UUID& assetId) const;

    // Forward to allocator so callers don't need to keep the allocator reference.
    void onHandleUsed(RenderHandle handle) { allocator_.onHandleUsed(handle); }

    uint64_t usedBytes()   const { return allocator_.usedBytes(); }
    uint64_t totalBudget() const { return allocator_.totalBudget(); }
    size_t   loadedCount() const { return assetIndex_.size(); }

private:
    RenderHandle nextHandle();

    GPUMemoryAllocator& allocator_;
    std::unordered_map<HE::UUID, RenderHandle> assetIndex_;
    // Reverse map: encoded handle → UUID (for release by handle)
    std::unordered_map<uint64_t, HE::UUID>     handleToAsset_;

    uint32_t m_nextIndex      = 1; // 0 is reserved for invalid
    uint32_t m_generation     = 1; // bumped on every nextHandle() call
};
