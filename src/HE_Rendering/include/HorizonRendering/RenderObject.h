#pragma once
#include <Types/Handle.h>
#include <Math/Math.h>
#include <cstdint>

// One renderable entity extracted from the ECS world each frame.
struct RenderObject {
    RenderHandle meshHandle;
    RenderHandle materialHandle;
    glm::mat4    transform;
    uint32_t     entityId;
    uint8_t      lod;
};
