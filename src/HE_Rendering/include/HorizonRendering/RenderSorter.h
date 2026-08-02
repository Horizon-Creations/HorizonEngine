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
