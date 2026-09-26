#pragma once
#include "../HE_RENDERING_API.h"
#include "CommandBuffer.h"
#include "RenderWorld.h"
#include <vector>
#include <cstdint>

class HE_RENDERING_API RenderSorter {
public:
    void sort(const RenderWorld&          world,
              const std::vector<uint8_t>& visible,
              std::vector<uint32_t>&      outSortedIndices);

    // ── Transparency (shared by every backend's draw loop) ───────────────────
    // A draw counts as opaque only at (effectively) full opacity. The epsilon
    // keeps 1.0f-rounding noise out of the blended pass, which costs a depth
    // write and a back-to-front sort per draw.
    static constexpr float kOpaqueOpacityThreshold = 0.999f;

    // The opacity actually rendered: the resolved material opacity times the
    // per-instance tint alpha (particle alpha-over-life, foliage fades, ...).
    // Classifying on dc.opacity ALONE misses tint-driven translucency and draws a
    // fading particle in the opaque pass.
    static float effectiveOpacity(const DrawCall& dc)
    {
        return dc.opacity * dc.instanceTint.a;
    }
    static bool isTransparent(const DrawCall& dc)
    {
        return effectiveOpacity(dc) < kOpaqueOpacityThreshold;
    }

    // Sort key for the blended pass: SQUARED distance from the camera to the
    // draw's world origin. Squared, not the true length — sqrt is monotonic, so
    // the resulting order is identical, and skipping it keeps the key exact.
    static float backToFrontKey(const glm::mat4& transform, const glm::vec3& camPos)
    {
        const glm::vec3 d = glm::vec3(transform[3]) - camPos;
        return glm::dot(d, d);
    }

    // Split a frame's draw calls into the opaque and the blended pass, preserving
    // record order within each (the opaque pass relies on that for its
    // same-mesh/same-material batching).
    //
    // sectionAware says whether the caller draws DrawCall::indexOffset/indexCount
    // or the whole index buffer per draw. Since the section port every backend
    // that collects through here (D3D11, D3D12, Vulkan) is section-aware and
    // passes true; the default stays the conservative guard for a caller that
    // says nothing. A section-UNAWARE backend must not see a multi-section
    // mesh's second, third… slot — each would repaint the entire mesh in
    // another material — so the default drops sectionIndex > 0 and hands it
    // slot 0 alone: one draw, the mesh's own material, as before sections
    // existed. A section-aware backend gets every slot. Whole-mesh draws
    // (sectionIndex -1) and a one-section mesh (never sectionIndex > 0) pass
    // either way.
    static void partitionByOpacity(const std::vector<DrawCall>&  drawCalls,
                                   std::vector<const DrawCall*>& outOpaque,
                                   std::vector<const DrawCall*>& outTransparent,
                                   bool                          sectionAware = false);

    // Order the blended pass back-to-front (farthest first) so alpha compositing
    // is correct. std::sort is NOT stable, so draws at exactly equal distance may
    // come out in either order — that has always been true of every backend copy
    // and is deliberately not "fixed" here: making it stable would change the
    // existing draw order on equal-depth ties.
    static void sortBackToFront(std::vector<const DrawCall*>& transparent,
                                const glm::vec3&              camPos);

    // ── Depth-only batching (shadow layers, SSAO / GI pre-passes) ───────────
    // The shadow passes on GL and Metal cull + sort per light view themselves,
    // and Metal's SSAO position pre-pass and GI G-buffer pre-pass walk the
    // camera-sorted list the same way; all of them used to draw one call per
    // object. A depth-only pass has no material, section, tint or texture
    // input — the only thing that decides whether two objects can share a
    // draw is the mesh. So a run of consecutive same-mesh objects in the
    // sorted list (the sorter already groups by mesh id) collapses into ONE
    // instanced draw over a flat transform array.
    //
    // One run of same-mesh objects: `count` transforms starting at
    // DepthBatchList::transforms[first]. count == 1 is the plain single draw
    // (the backends keep their non-instanced program for it).
    struct DepthBatch {
        HE::UUID meshAssetId;
        uint32_t first = 0;
        uint32_t count = 0;
    };
    struct DepthBatchList {
        std::vector<DepthBatch> batches;
        std::vector<glm::mat4>  transforms; // world transforms, batch-contiguous
        void clear() { batches.clear(); transforms.clear(); }
    };

    // Which per-object opt-out a depth-only pass honours. The flags live on
    // RenderObject (castsShadow / contributesAO); precipitation and particle
    // billboards clear both so thousands of them never reach a depth map or
    // the AO pre-pass. `All` is the GI G-buffer pre-pass on Metal, which has
    // always drawn every visible object (every shaded pixel needs a shadow
    // value, occluder or not).
    enum class DepthFilter : uint8_t {
        ShadowCasters,  // obj.castsShadow
        AoContributors, // obj.contributesAO
        All,
    };

    // Walk `sortedIndices` (a cull + sort of world.objects), drop what the pass
    // never draws — objects the filter rejects and `skipEntity` (the local
    // light's own mesh; kNoOwnerEntity skips nothing) — and form runs of
    // consecutive equal meshAssetId. Filtering happens BEFORE run-forming, so a
    // skipped object in the middle of a run does not split it. Only adjacency
    // counts: A,B,A stays three runs; merging non-adjacent objects is the
    // sorter's job, not this one's.
    static void batchDepthRuns(const RenderWorld&           world,
                               const std::vector<uint32_t>& sortedIndices,
                               DepthFilter                  filter,
                               uint32_t                     skipEntity,
                               DepthBatchList&              out);

    // The shadow-pass spelling: batchDepthRuns with DepthFilter::ShadowCasters.
    static void batchDepthCasters(const RenderWorld&           world,
                                  const std::vector<uint32_t>& sortedIndices,
                                  uint32_t                     skipEntity,
                                  DepthBatchList&              out)
    {
        batchDepthRuns(world, sortedIndices, DepthFilter::ShadowCasters, skipEntity, out);
    }

    // HE_DEPTH_INSTANCING=0 sends the instanced depth-only draws of D3D11, D3D12
    // and Vulkan (shadow runs, SSAO + GI pre-pass batches) back to the
    // per-object loop — the A/B for a hardware smoke test, the same role
    // HE_MTL_INSTANCING plays on Metal. Read once per process.
    static bool depthInstancingEnabled();

private:
    // Precomputed per-object sort key so the O(n log n) comparator never has to
    // recompute camera distance or extract a matrix column. Reused across frames
    // to avoid reallocating every frame.
    struct SortKey {
        uint64_t meshHi;
        uint64_t meshLo;
        float    distSq;
        uint32_t index;
    };
    std::vector<SortKey> m_keys;
};
