#pragma once
#include <Types/Handle.h>
#include <Types/UUID.h>
#include <Math/Math.h>
#include <Math/AABB.h>
#include <cstdint>
#include <vector>

// One renderable entity extracted from the ECS world each frame.
struct RenderObject {
    // Asset identity straight from MeshComponent. Until the
    // RenderResourceManager resolves UUIDs to RenderHandles at extract time,
    // backends use this to look up / lazily upload the GPU mesh.
    HE::UUID     meshAssetId;
    // Optional material override straight from MaterialComponent. When set, the
    // backend resolves it instead of the mesh's embedded material. Null UUID =
    // fall back to the material baked into the mesh asset.
    HE::UUID     materialAssetId;
    RenderHandle meshHandle     = RenderHandle::invalid();
    RenderHandle materialHandle = RenderHandle::invalid();
    glm::mat4    transform      = glm::mat4(1.0f);
    // World-space bounds for culling. Seeded by the extractor with the
    // fallback cube's box; backends refine it with the real mesh AABB once
    // the asset is resolved. Invalid box = never culled.
    HE::AABB     worldBounds;
    uint32_t     entityId       = 0;
    uint8_t      lod            = 0;
    // Cheap-billboard opt-outs. Precipitation/particles set these false so thousands of
    // them skip the per-object shadow-map depth pass and the SSAO position prepass (where
    // they are NOT instanced) — and so rain/snow don't wrongly cast shadows or darken AO.
    bool         castsShadow    = true;
    bool         contributesAO  = true;
};

// Skinned renderable: same as RenderObject but carries bone matrices for GPU skinning.
struct SkinnedRenderObject : RenderObject {
    std::vector<glm::mat4> boneMatrices;
};
