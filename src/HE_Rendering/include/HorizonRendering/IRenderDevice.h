#pragma once
#include "CommandBuffer.h"
#include <Types/Handle.h>
#include <cstdint>

struct BufferDesc {
    uint64_t sizeBytes;
    uint32_t flags;
};

struct TextureDesc {
    uint32_t width, height, depth;
    uint32_t mipLevels;
    uint32_t format;
};

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;

    virtual RenderHandle createBuffer (const BufferDesc&  desc, const void* initialData = nullptr) = 0;
    virtual RenderHandle createTexture(const TextureDesc& desc, const void* initialData = nullptr) = 0;
    virtual void         destroyBuffer (RenderHandle handle) = 0;
    virtual void         destroyTexture(RenderHandle handle) = 0;

    virtual void submit (const CommandBuffer& cmds) = 0;
    virtual void present() = 0;
};
