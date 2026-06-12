#pragma once
#include <Types/Handle.h>
#include <Types/UUID.h>
#include "GPUMemoryAllocator.h"
#include <unordered_map>

struct MeshData     { };
struct TextureData  { };
struct MaterialDesc { };

class RenderResourceManager {
public:
    explicit RenderResourceManager(GPUMemoryAllocator& allocator);

    RenderHandle uploadMesh    (const HE::UUID& assetId, const MeshData&     data);
    RenderHandle uploadTexture (const HE::UUID& assetId, const TextureData&  data);
    RenderHandle createMaterial(const HE::UUID& assetId, const MaterialDesc& desc);

    void release(RenderHandle handle);

    RenderHandle findHandle(const HE::UUID& assetId) const;
    bool         isLoaded  (const HE::UUID& assetId) const;

private:
    GPUMemoryAllocator& allocator_;
    std::unordered_map<HE::UUID, RenderHandle> assetIndex_;
};
