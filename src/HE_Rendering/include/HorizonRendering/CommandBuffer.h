#pragma once
#include "../HE_RENDERING_API.h"
#include <Types/Handle.h>
#include <Types/UUID.h>
#include <Math/Math.h>
#include <vector>
#include <cstdint>

// A single draw recorded by a render pass and replayed by the backend.
//
// Until the RenderResourceManager (3.7) resolves UUIDs to RenderHandles at
// extract time, the backends look meshes up by meshAssetId — the same path
// the immediate submission loop used. The RenderHandle fields are carried
// through for the future handle-based device path but are currently invalid.
struct DrawCall {
    HE::UUID     meshAssetId;                          // backend resolves → GPU mesh
    HE::UUID     materialAssetId;                       // optional material override (null = mesh's own)
    RenderHandle mesh          = RenderHandle::invalid();
    RenderHandle material      = RenderHandle::invalid();
    glm::mat4    transform     = glm::mat4(1.0f);      // first instance (or sole) transform
    uint32_t     instanceCount = 1;
    uint32_t     entityId      = 0;                    // editor picking / debug
    uint8_t      lod           = 0;
    bool         contributesAO = true;                 // false → skipped by the SSAO prepass
    // See RenderObject::receivesShadow. Shared across the whole batch when
    // instanceTransforms is non-empty: GeometryPass only batches runs that agree
    // on it, so a backend can hand it to the shader once per draw.
    bool         receivesShadow = true;
    // PBR material scalars resolved from MaterialAsset at extract time.
    glm::vec3    baseColor     = { 1.0f, 1.0f, 1.0f };
    float        metallic      = 0.0f;
    float        roughness     = 0.5f;
    float        opacity       = 1.0f;
    // Per-instance tint (see RenderObject::instanceTint) — identity = no-op.
    // Shared across the whole batch when instanceTransforms is non-empty:
    // GeometryPass only batches runs that share this value.
    glm::vec4    instanceTint  = { 1.0f, 1.0f, 1.0f, 1.0f };
    // Non-empty when GeometryPass batched multiple same-mesh+same-material objects.
    // The backend uploads these to the instance VBO and calls glDrawElementsInstanced.
    std::vector<glm::mat4> instanceTransforms;
    // Per-entity node-graph param override: empty = use the material's own
    // shaderParamData; otherwise the FULL merged HeParams block (16 vec4 = 64
    // floats) the backend uploads instead. Objects carrying an override are never
    // batched (each is its own DrawCall), so this is per-draw unambiguous.
    std::vector<float> paramOverride;
    // Landscape layer weightmap (see RenderObject::weightmapTextureId). Objects
    // carrying one are never batched with objects carrying a different one.
    HE::UUID     weightmapTextureId;
};

// A draw call that also carries per-joint bone matrices for GPU skinning.
// The backend binds the skinned shader and uploads boneMatrices as a uniform
// array before issuing the draw.
struct SkinnedDrawCall : DrawCall {
    std::vector<glm::mat4> boneMatrices;  // one per joint; identity = bind pose
};

// Collects the draw calls produced by the render passes for one frame.
// The backend replays drawCalls() after binding its pipeline and per-frame
// state (camera, lights). Cleared and refilled every frame.
class HE_RENDERING_API CommandBuffer {
public:
    void reset();
    void recordDraw(const DrawCall& call);
    void recordSkinnedDraw(const SkinnedDrawCall& call);
    void recordPostProcess();   // signals that a post-process pass ran this frame

    const std::vector<DrawCall>&        drawCalls()        const;
    const std::vector<SkinnedDrawCall>& skinnedDrawCalls() const;
    bool hasPostProcess() const { return m_postProcess; }
    bool empty() const { return m_drawCalls.empty() && m_skinnedDrawCalls.empty(); }

private:
    std::vector<DrawCall>        m_drawCalls;
    std::vector<SkinnedDrawCall> m_skinnedDrawCalls;
    bool                         m_postProcess = false;
};
