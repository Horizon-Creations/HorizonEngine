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
    RenderHandle mesh          = RenderHandle::invalid();
    RenderHandle material      = RenderHandle::invalid();
    glm::mat4    transform     = glm::mat4(1.0f);
    uint32_t     instanceCount = 1;
    uint32_t     entityId      = 0;                    // editor picking / debug
    uint8_t      lod           = 0;
};

// Collects the draw calls produced by the render passes for one frame.
// The backend replays drawCalls() after binding its pipeline and per-frame
// state (camera, lights). Cleared and refilled every frame.
class HE_RENDERING_API CommandBuffer {
public:
    void reset();
    void recordDraw(const DrawCall& call);

    const std::vector<DrawCall>& drawCalls() const;
    bool empty() const { return drawCalls_.empty(); }

private:
    std::vector<DrawCall> drawCalls_;
};
