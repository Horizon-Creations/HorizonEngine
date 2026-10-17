#pragma once
#include "../HE_RENDERING_API.h"
#include "FrustumCuller.h"
#include "RenderSorter.h"
#include "RenderWorld.h"
#include <cstdint>
#include <vector>

namespace HE {

// Cull + sort once per walk instead of once per pass (audit REN-805/304/407).
//
// RenderExtractor::beginFrame already leaves ONE walk of the world in the caller's
// RenderWorld for the whole frame, but every pass still culled and sorted it again:
// Vulkan four times against the camera (decal depth, GI, SSAO, scene), Metal up to four
// (GI G-buffer, SSAO pre-pass, scene, deferred G-buffer), and every backend once per
// shadow layer (up to 3 cascades + 16 local layers), each with a full O(n log n) sort
// by a key — mesh, distance to the camera — that is the same for all of them.
//
// One instance per backend, next to its RenderExtractor. Both halves key on the walk
// (RenderExtractor::fullExtractCount): outside a FrameScope every extract is a walk of
// its own, so nothing is reused and a backend that never opts in behaves as before.
class HE_RENDERING_API FrameVisibility
{
public:
    // What a result was computed from, besides the RenderWorld itself (its object array,
    // count, camera). `walk` is RenderExtractor::fullExtractCount() of the walk the world
    // holds. `boundsStamp` moves whenever the backend rewrites worldBounds in place after
    // the walk (refineObjectBounds): a cull before the refine keeps invalid-bound objects
    // that a cull after it may drop. `variant` is anything else the result depends on
    // (Metal: occlusion culling on/off).
    struct Key
    {
        uint64_t walk        = 0;
        uint64_t boundsStamp = 0;
        uint32_t variant     = 0;
    };

    // ── Camera (REN-805/304) ────────────────────────────────────────────────
    // FrustumCuller::cull against world.camera + RenderSorter::sort, or the earlier
    // result of this walk when key, object array, count, view-projection and camera
    // position all match. Returns true when it computed. The outputs are always
    // written (a copy on reuse), so a pass that reuses its vectors for something else
    // in between cannot hand the next pass a stale answer.
    bool cullAndSortCamera(const RenderWorld& world, const Key& key,
                           std::vector<uint8_t>& outVisible, std::vector<uint32_t>& outSorted);

    // The same cache for a backend whose camera cull has a step of its own between cull
    // and sort (Metal's occlusion refine): findCamera answers from the cache (true) or
    // leaves the outputs alone (false); after computing, storeCamera keeps the result.
    bool findCamera(const RenderWorld& world, const Key& key,
                    std::vector<uint8_t>& outVisible, std::vector<uint32_t>& outSorted);
    void storeCamera(const RenderWorld& world, const Key& key,
                     const std::vector<uint8_t>& visible, const std::vector<uint32_t>& sorted);

    // ── Shadow layers (REN-407) ─────────────────────────────────────────────
    // One depth-only layer's caster runs (RenderSorter::batchDepthCasters, `skipEntity`
    // as there), the same runs with the same members as the per-layer cull + sort it
    // replaces. world.shadow lists every view the pass renders (the cascades, the local
    // layers). The first layer of a walk culls all of them, sorts the casters of their
    // UNION once (RenderSorter::sortShadowCasters: never more than the layers sorted
    // between them before, and nothing outside the shadow range) and keeps each view's
    // cull; that layer and every later one is then one O(n) filter of that list. A
    // `viewProj` that is not in the list (matched exactly) takes the old cull + sort.
    void shadowLayer(const RenderWorld& world, const Key& key, const glm::mat4& viewProj,
                     uint32_t skipEntity, RenderSorter::DepthBatchList& out);

    // For tests and the profiler: how often each half computed and how often it reused.
    uint64_t cameraComputes()  const { return m_cameraComputes; }
    uint64_t cameraReuses()    const { return m_cameraReuses; }
    uint64_t shadowPresorts()  const { return m_shadowPresorts; }
    uint64_t shadowLayers()    const { return m_shadowLayers; }
    uint64_t shadowFallbacks() const { return m_shadowFallbacks; }

private:
    struct WorldKey
    {
        Key                 key{};
        const RenderWorld*  world   = nullptr;
        const RenderObject* objects = nullptr;
        size_t              count   = 0;
        glm::vec3           camPos{ 0.0f };
        glm::mat4           viewProj{ 1.0f };
    };
    static WorldKey makeKey(const RenderWorld& world, const Key& key);
    static bool     sameKey(const WorldKey& a, const WorldKey& b);

    FrustumCuller m_culler;
    RenderSorter  m_sorter;

    bool                  m_cameraValid = false;
    WorldKey              m_cameraKey{};
    std::vector<uint8_t>  m_cameraVisible;
    std::vector<uint32_t> m_cameraSorted;

    bool                              m_shadowValid = false;
    WorldKey                          m_shadowKey{};     // viewProj unused: the views are below
    std::vector<glm::mat4>            m_shadowViews;     // world.shadow's views the presort culled
    std::vector<glm::mat4>            m_viewScratch;
    std::vector<std::vector<uint8_t>> m_shadowViewVisible; // each view's cull, as it was presorted
    std::vector<uint8_t>              m_shadowUnion;     // OR of those culls
    std::vector<uint32_t>             m_shadowPresorted; // the union's casters, sort()'s order
    std::vector<uint8_t>              m_layerVisible;    // scratch, a fallback layer's cull
    std::vector<uint32_t>             m_layerSorted;     // scratch, one layer's visible casters

    uint64_t m_cameraComputes  = 0;
    uint64_t m_cameraReuses    = 0;
    uint64_t m_shadowPresorts  = 0;
    uint64_t m_shadowLayers    = 0;
    uint64_t m_shadowFallbacks = 0;
};

} // namespace HE
