#pragma once
#include "../HE_RENDERING_API.h"
#include <Types/Handle.h>
#include <Types/UUID.h>
#include <Math/Math.h>
#include <vector>
#include <cstddef>
#include <cstdint>

class CommandBuffer;

namespace HE {

// A read-only window onto matrices a CommandBuffer holds for the frame
// (DrawCall::instanceTransforms, SkinnedDrawCall::boneMatrices). It names the
// buffer's flat arena plus a [first, first + count) slice and resolves the
// address only when read, so the arena may grow while later draws are still
// being recorded. Copying a DrawCall copies the window, never the matrices:
// every slot of a multi-section mesh and every chunk of a split run share the
// one copy GeometryPass made (before, each slot and chunk was a deep copy of up
// to kMaxInstancesPerDraw mat4, plus another one in recordDraw). Valid until
// the owning CommandBuffer is reset or destroyed. It reads like the
// std::vector it replaced (empty/size/data/[]/begin/end), so the backends'
// loops over it are unchanged.
class Mat4Range {
public:
    Mat4Range() = default;
    bool             empty() const { return m_count == 0; }
    size_t           size()  const { return m_count; }
    const glm::mat4* data()  const { return m_count ? m_arena->data() + m_first : nullptr; }
    const glm::mat4* begin() const { return data(); }
    const glm::mat4* end()   const { return data() + m_count; }
    const glm::mat4& operator[](size_t i) const { return m_arena->data()[m_first + i]; }
    // [from, from + count) of this window, clamped to it.
    Mat4Range slice(size_t from, size_t count) const
    {
        if (from >= m_count) return {};
        const size_t n = count < m_count - from ? count : m_count - from;
        return Mat4Range(m_arena, m_first + static_cast<uint32_t>(from), static_cast<uint32_t>(n));
    }
    void clear() { m_count = 0; }

private:
    friend class ::CommandBuffer;
    Mat4Range(const std::vector<glm::mat4>* arena, uint32_t first, uint32_t count)
        : m_arena(arena), m_first(first), m_count(count) {}
    const std::vector<glm::mat4>* m_arena = nullptr;
    uint32_t                      m_first = 0;
    uint32_t                      m_count = 0;
};

} // namespace HE

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
    // Index range of this draw. indexCount == 0 means "the whole mesh" — the
    // shape every draw had before sections existed and still the only shape a
    // one-section mesh produces. A multi-section mesh arrives as one DrawCall
    // per section (GeometryPass expands RenderObject::sections), each with the
    // section's [indexOffset, indexCount) and its own materialAssetId, and
    // sectionIndex says which slot it is (-1 = whole mesh). Every backend (GL,
    // Metal, D3D11, D3D12, Vulkan) draws per section and applies the range; a
    // backend that did not would have to skip sectionIndex > 0 and draw slot 0
    // whole — see RenderSorter::partitionByOpacity, which still offers that mode.
    uint32_t     indexOffset   = 0;
    uint32_t     indexCount    = 0;
    int32_t      sectionIndex  = -1;
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
    // A window into the CommandBuffer's matrix arena (see HE::Mat4Range), shared by
    // every section and chunk of the run; set it with CommandBuffer::storeMatrices.
    HE::Mat4Range instanceTransforms;
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
    // One per joint; identity = bind pose. A window into the CommandBuffer's
    // matrix arena like instanceTransforms: every slot of the mesh shares it.
    HE::Mat4Range boneMatrices;
};

// Collects the draw calls produced by the render passes for one frame.
// The backend replays drawCalls() after binding its pipeline and per-frame
// state (camera, lights). Cleared and refilled every frame; reset() keeps the
// capacity of the draw lists and of the matrix arena, so a steady frame records
// without touching the heap.
class HE_RENDERING_API CommandBuffer {
public:
    CommandBuffer() = default;
    // Copies and moves point the recorded windows at the new buffer's own arena.
    CommandBuffer(const CommandBuffer& other);
    CommandBuffer(CommandBuffer&& other) noexcept;
    CommandBuffer& operator=(const CommandBuffer& other);
    CommandBuffer& operator=(CommandBuffer&& other) noexcept;

    void reset();
    // A draw whose windows name another buffer's arena (recorded from a copy of
    // someone else's DrawCall) gets its matrices copied into this one's.
    void recordDraw(const DrawCall& call);
    void recordDraw(DrawCall&& call);
    void recordSkinnedDraw(const SkinnedDrawCall& call);
    void recordSkinnedDraw(SkinnedDrawCall&& call);
    void recordPostProcess();   // signals that a post-process pass ran this frame

    // The frame's flat matrix arena behind every HE::Mat4Range of this buffer.
    // storeMatrices appends a copy and returns the window onto it. A producer that
    // fills in place (RenderWorld::appendInstances) notes matrixArena().size(),
    // appends, then takes matrixRange(first, count). Only ever append: shrinking
    // or rewriting the arena changes draws already recorded.
    HE::Mat4Range storeMatrices(const glm::mat4* matrices, size_t count);
    HE::Mat4Range storeMatrices(const std::vector<glm::mat4>& matrices)
    {
        return storeMatrices(matrices.data(), matrices.size());
    }
    std::vector<glm::mat4>& matrixArena() { return m_matrices; }
    HE::Mat4Range           matrixRange(size_t first, size_t count) const;

    const std::vector<DrawCall>&        drawCalls()        const;
    const std::vector<SkinnedDrawCall>& skinnedDrawCalls() const;
    bool hasPostProcess() const { return m_postProcess; }
    bool empty() const { return m_drawCalls.empty() && m_skinnedDrawCalls.empty(); }

private:
    void adopt(HE::Mat4Range& range);
    void rebase(const std::vector<glm::mat4>* from);

    std::vector<DrawCall>        m_drawCalls;
    std::vector<SkinnedDrawCall> m_skinnedDrawCalls;
    std::vector<glm::mat4>       m_matrices;
    bool                         m_postProcess = false;
};
