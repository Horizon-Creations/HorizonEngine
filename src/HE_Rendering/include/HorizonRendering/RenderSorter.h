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
    static void partitionByOpacity(const std::vector<DrawCall>&  drawCalls,
                                   std::vector<const DrawCall*>& outOpaque,
                                   std::vector<const DrawCall*>& outTransparent);

    // Order the blended pass back-to-front (farthest first) so alpha compositing
    // is correct. std::sort is NOT stable, so draws at exactly equal distance may
    // come out in either order — that has always been true of every backend copy
    // and is deliberately not "fixed" here: making it stable would change the
    // existing draw order on equal-depth ties.
    static void sortBackToFront(std::vector<const DrawCall*>& transparent,
                                const glm::vec3&              camPos);

    // ── Depth-only batching (shadow cascades / local shadow layers) ─────────
    // The shadow passes on GL and Metal cull + sort per light view themselves
    // and used to draw one depth-only call per caster. A depth pass has no
    // material, section, tint or texture input — the only thing that decides
    // whether two casters can share a draw is the mesh. So a run of consecutive
    // same-mesh casters in the sorted list (the sorter already groups by mesh
    // id) collapses into ONE instanced draw over a flat transform array.
    //
    // One run of same-mesh casters: `count` transforms starting at
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

    // Walk `sortedIndices` (a light-view cull + sort of world.objects), drop
    // what the depth pass never draws — non-casters (billboards, precipitation)
    // and `skipEntity` (the local light's own mesh, kNoOwnerEntity skips
    // nothing) — and form runs of consecutive equal meshAssetId. Filtering
    // happens BEFORE run-forming, so a skipped object in the middle of a run
    // does not split it. Only adjacency counts: A,B,A stays three runs; merging
    // non-adjacent objects is the sorter's job, not this one's.
    static void batchDepthCasters(const RenderWorld&           world,
                                  const std::vector<uint32_t>& sortedIndices,
                                  uint32_t                     skipEntity,
                                  DepthBatchList&              out);

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
