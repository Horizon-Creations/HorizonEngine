#pragma once
#include <Types/Handle.h>
#include <Types/UUID.h>
#include <Math/Math.h>
#include <cstdint>

// One renderable entity extracted from the ECS world each frame.
struct RenderObject {
    // Asset identity straight from MeshComponent. Until the
    // RenderResourceManager resolves UUIDs to RenderHandles at extract time,
    // backends use this to look up / lazily upload the GPU mesh.
    HE::UUID     meshAssetId;
    RenderHandle meshHandle     = RenderHandle::invalid();
    RenderHandle materialHandle = RenderHandle::invalid();
    glm::mat4    transform      = glm::mat4(1.0f);
    uint32_t     entityId       = 0;
    uint8_t      lod            = 0;
};
