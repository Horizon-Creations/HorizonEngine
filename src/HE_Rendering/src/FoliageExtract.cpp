#include "FoliageExtract.h"
#include "HorizonRendering/RenderWorld.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/EntityActive.h>
#include <Diagnostics/Log.h>
#include <Diagnostics/Profiler.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HE
{
namespace
{
    // ── Switches ────────────────────────────────────────────────────────────
    int g_modeOverride = -1;   // tests; -1 = the environment decides

    FoliageMode envMode()
    {
        static const FoliageMode mode = []{
            const char* v = std::getenv("HE_FOLIAGE_CLUSTERS");
            if (!v || !*v) return FoliageMode::Clusters;
            if (std::string_view(v) == "ordered") return FoliageMode::Ordered;
            return std::atoi(v) == 0 ? FoliageMode::PerInstance : FoliageMode::Clusters;
        }();
        return mode;
    }

    bool envStatsOn()
    {
        static const bool on = []{
            const char* v = std::getenv("HE_FOLIAGE_STATS");
            return v && *v && std::atoi(v) != 0;
        }();
        return on;
    }

    thread_local FoliageExtractStats t_stats;

    // How far a swaying leaf may leave its box: wind moves vertices in the vertex
    // shader, where culling cannot see it. A few decimetres, added on every side.
    constexpr float kWindReserve = 0.5f;

    // Distance classification margin (relative, on the SQUARED distance): a bucket
    // counts as wholly inside or wholly outside the draw distance only when it clears
    // the line by this much, so float rounding in the matrix product can never put an
    // instance on the wrong side of a decision made on the bucket's box. A bucket within
    // the margin is tested instance by instance, exactly as the per-instance path did.
    constexpr float kRangeMargin = 1e-4f;

    // ── Mesh bounds ─────────────────────────────────────────────────────────
    // The object-space box of a mesh, or an invalid box when it cannot be read.
    // The baked box (cooked assets, files the loader scanned) when it is a real
    // volume; otherwise the vertices: the built-in primitives and every mesh
    // registered in memory carry no box at all (boundsMin == boundsMax == 0), which
    // is why extractMeshes leaves such an object's bounds invalid for the backends
    // to refine. A cluster is never refined, so it needs the box NOW.
    HE::AABB scanMeshBounds(const StaticMeshAsset& m)
    {
        HE::AABB baked;
        baked.min = { m.boundsMin[0], m.boundsMin[1], m.boundsMin[2] };
        baked.max = { m.boundsMax[0], m.boundsMax[1], m.boundsMax[2] };
        if (baked.isValid() && baked.max != baked.min) return baked;

        HE::AABB scan;
        if (m.cooked && m.vertexCount > 0 && m.interleaved.size() >= static_cast<size_t>(m.vertexCount) * 8)
        {
            for (uint32_t v = 0; v < m.vertexCount; ++v)
                scan.expand(glm::vec3(m.interleaved[v * 8], m.interleaved[v * 8 + 1], m.interleaved[v * 8 + 2]));
        }
        else if (m.vertices.size() >= 3)
        {
            scan = HE::AABB::fromPositions(m.vertices.data(), m.vertices.size() / 3);
        }
        return scan;
    }

    struct BoundsEntry
    {
        const StaticMeshAsset* asset = nullptr;   // staleness check: a reloaded asset is another object
        size_t                 size  = 0;
        HE::AABB               box;
    };

    HE::AABB meshLocalBounds(const ContentManager* cm, const HE::UUID& id)
    {
        if (!cm) return {};
        const StaticMeshAsset* m = cm->getStaticMesh(id);
        if (!m) return {};
        // Per thread, so no lock: a mesh is scanned once, not once per frame.
        thread_local std::unordered_map<HE::UUID, BoundsEntry> cache;
        const size_t size = m->cooked ? m->vertexCount : m->vertices.size();
        if (auto it = cache.find(id); it != cache.end() && it->second.asset == m && it->second.size == size)
            return it->second.box;
        BoundsEntry e;
        e.asset = m;
        e.size  = size;
        e.box   = scanMeshBounds(*m);
        cache[id] = e;
        return e.box;
    }

    // What a layer's mesh adds around an instance origin, for every pose the scatter
    // can give it: rotated about Y by anything, scaled by anything in [minScale, maxScale].
    struct Reach
    {
        bool  known = false;
        float horizontal = 0.0f;   // from the origin, in X and Z (rotation about Y can turn the mesh any way)
        float below = 0.0f;        // lowest point relative to the origin's Y (usually <= 0)
        float above = 0.0f;        // highest point relative to the origin's Y
    };

    Reach reachOf(const FoliageComponent& fol, const HE::AABB& mb)
    {
        Reach r;
        if (!mb.isValid()) return r;
        float radius2 = 0.0f;
        for (int i = 0; i < 4; ++i)
        {
            const float x = (i & 1) ? mb.max.x : mb.min.x;
            const float z = (i & 2) ? mb.max.z : mb.min.z;
            radius2 = std::max(radius2, x * x + z * z);
        }
        const float sAbs = std::max(std::abs(fol.minScale), std::abs(fol.maxScale));
        r.horizontal = std::sqrt(radius2) * sAbs;
        // Height is linear in the scale, so its extremes sit at the scale range's ends.
        const float y0 = mb.min.y * fol.minScale, y1 = mb.min.y * fol.maxScale;
        const float y2 = mb.max.y * fol.minScale, y3 = mb.max.y * fol.maxScale;
        r.below = std::min(std::min(y0, y1), std::min(y2, y3));
        r.above = std::max(std::max(y0, y1), std::max(y2, y3));
        r.known = true;
        return r;
    }

    // The world box of a local one under the layer's parent matrix.
    HE::AABB worldBox(const HE::AABB& local, const InstanceBlock::Parent kind, const glm::mat4& parent)
    {
        if (!local.isValid()) return local;
        switch (kind)
        {
        case InstanceBlock::Parent::Identity:
            return local;
        case InstanceBlock::Parent::Translation:
        {
            HE::AABB b;
            b.min = local.min + glm::vec3(parent[3]);
            b.max = local.max + glm::vec3(parent[3]);
            return b;
        }
        default:
            return local.transformed(parent);
        }
    }

    // Squared XZ distance from the camera to the nearest and farthest point of a
    // world box (the draw distance is measured in the ground plane, as it always was).
    void rangeOf(const HE::AABB& b, const glm::vec3& cam, float& nearSq, float& farSq)
    {
        const float ndx = std::max(std::max(b.min.x - cam.x, 0.0f), cam.x - b.max.x);
        const float ndz = std::max(std::max(b.min.z - cam.z, 0.0f), cam.z - b.max.z);
        const float fdx = std::max(std::abs(cam.x - b.min.x), std::abs(cam.x - b.max.x));
        const float fdz = std::max(std::abs(cam.z - b.min.z), std::abs(cam.z - b.max.z));
        nearSq = ndx * ndx + ndz * ndz;
        farSq  = fdx * fdx + fdz * fdz;
    }

    // ── The old path ────────────────────────────────────────────────────────
    // One RenderObject per instance in range, bounds left invalid for the backends
    // to refine — what extractFoliage did before clusters (HE_FOLIAGE_CLUSTERS=0).
    void extractPerInstance(const FoliageComponent& fol, entt::entity e, const InstanceBlock& all,
                            const glm::vec3& cam, RenderWorld& out)
    {
        const FoliageStore& store = *fol.store;
        const float dd2 = fol.drawDistance * fol.drawDistance;
        for (const FoliageBucket& b : store.buckets)
            for (uint32_t i = b.first; i < b.first + b.count; ++i)
            {
                const glm::vec3 wp = all.position(i);
                const float dx = wp.x - cam.x;
                const float dz = wp.z - cam.z;
                if (dx * dx + dz * dz > dd2) continue;
                const glm::mat4 inst = all.world(i);

                RenderObject obj;
                obj.meshAssetId     = fol.meshAssetId;
                obj.materialAssetId = fol.materialAssetId;
                obj.transform       = inst;
                // Real bounds are filled in by the backend mesh-resolve refine; leave them invalid
                // here so a not-yet-resident instance stays visible instead of being culled against
                // a unit-cube proxy smaller than the actual foliage mesh.
                obj.worldBounds     = HE::AABB{};
                obj.entityId        = static_cast<uint32_t>(e);
                out.objects.push_back(obj);
            }
    }

    // ── The verification mode ───────────────────────────────────────────────
    // FoliageMode::Ordered: every instance in range, in the order RenderSorter::sort would give
    // the per-instance objects (mesh first — one mesh here — then squared 3D distance from the
    // camera to the instance's origin, nearest first), as ONE cluster over the frame's scratch.
    void extractOrdered(const FoliageComponent& fol, entt::entity e, const InstanceBlock& all,
                        const glm::vec3& cam, RenderWorld& out,
                        std::shared_ptr<std::vector<glm::mat4>>& scratch)
    {
        const FoliageStore& store = *fol.store;
        const float dd2 = fol.drawDistance * fol.drawDistance;
        struct Item { float distSq; uint32_t index; };
        std::vector<Item> items;
        items.reserve(store.local.size());
        for (uint32_t i = 0; i < store.local.size(); ++i)
        {
            const glm::vec3 wp = all.position(i);
            const float dx = wp.x - cam.x, dz = wp.z - cam.z;
            if (dx * dx + dz * dz > dd2) continue;
            const glm::vec3 d = wp - cam;
            items.push_back({ glm::dot(d, d), i });
        }
        if (items.empty()) return;
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b)
        { return a.distSq != b.distSq ? a.distSq < b.distSq : a.index < b.index; });

        if (!scratch) scratch = std::make_shared<std::vector<glm::mat4>>();
        InstanceBlock block;
        block.matrices = scratch;
        block.first    = static_cast<uint32_t>(scratch->size());
        block.count    = static_cast<uint32_t>(items.size());
        block.kind     = InstanceBlock::Parent::Identity;   // the scratch holds world matrices
        for (const Item& it : items) scratch->push_back(all.world(it.index));

        RenderObject obj;
        obj.meshAssetId     = fol.meshAssetId;
        obj.materialAssetId = fol.materialAssetId;
        obj.transform       = block.world(0);
        obj.worldBounds     = HE::AABB{};          // never culled: every pass draws the whole, ordered list
        obj.entityId        = static_cast<uint32_t>(e);
        obj.castsShadow     = fol.castsShadow;
        obj.contributesAO   = fol.contributesAO;
        obj.instanceBlock   = static_cast<int32_t>(out.instanceBlocks.size());
        out.objects.push_back(obj);
        out.instanceBlocks.push_back(block);
        ++t_stats.clusters;
        t_stats.clusterInstances += block.count;
        t_stats.instanceTests    += store.local.size();
    }
}

FoliageMode foliageMode()
{
    return g_modeOverride >= 0 ? static_cast<FoliageMode>(g_modeOverride) : envMode();
}

void setFoliageModeOverride(int mode)
{
    g_modeOverride = mode;
}

const FoliageExtractStats& lastFoliageExtractStats()
{
    return t_stats;
}

void extractFoliage(entt::registry& reg, RenderWorld& out, const ContentManager* cm)
{
    // Its own scope: the foliage share of RenderExtractor::extract, which scaled with
    // every cached instance before clusters and now scales with the buckets in range.
    HE_PROFILE_SCOPE_N("ExtractFoliage");
    t_stats = FoliageExtractStats{};

    const FoliageMode mode = foliageMode();
    const HE::ActiveFilter active(reg);
    const glm::vec3 cam = out.camera.position;

    // The instances of the buckets the draw distance cuts through, filtered one by one.
    // One vector for the whole frame; every such cluster is a range of it. Created on
    // first use, because most frames have no straddler.
    std::shared_ptr<std::vector<glm::mat4>> scratch;

    for (auto [entityBinding, folBinding] : reg.view<FoliageComponent>().each())
    {
        // Plain names for what the lambdas below capture: older compilers reject a lambda
        // that captures a structured binding.
        const entt::entity     e   = entityBinding;
        const FoliageComponent& fol = folBinding;
        if (!fol.visible) continue; // hidden (e.g. a preloaded zone)
        if (active.off(e)) continue;
        if (fol.meshAssetId == HE::UUID{}) continue;
        if (!fol.store || fol.store->buckets.empty()) continue;   // not scattered (yet), or nothing grew

        const FoliageStore& store = *fol.store;
        ++t_stats.layers;
        t_stats.buckets        += static_cast<uint32_t>(store.buckets.size());
        t_stats.totalInstances += store.local.size();

        // The terrain's pose NOW, so a layer follows a terrain that is moved, rotated, scaled
        // or parented after the scatter (the instances are stored relative to it).
        glm::mat4 parent(1.0f);
        if (const auto* tf = reg.try_get<TransformComponent>(e)) parent = tf->worldMatrix;

        InstanceBlock layer;
        layer.matrices = std::shared_ptr<const std::vector<glm::mat4>>(fol.store, &store.local);
        layer.parent   = parent;
        layer.kind     = InstanceBlock::classify(parent);

        if (mode == FoliageMode::PerInstance)
        {
            extractPerInstance(fol, e, layer, cam, out);
            continue;
        }
        if (mode == FoliageMode::Ordered)
        {
            extractOrdered(fol, e, layer, cam, out, scratch);
            continue;
        }

        const float dd2     = fol.drawDistance * fol.drawDistance;
        const float inside  = dd2 * (1.0f - kRangeMargin);
        const float outside = dd2 * (1.0f + kRangeMargin);
        const float sd      = fol.shadowDistance > 0.0f ? fol.shadowDistance : fol.drawDistance;
        const float sd2     = sd * sd;

        const Reach reach = reachOf(fol, meshLocalBounds(cm, fol.meshAssetId));

        // Pass 1: decide each bucket from its box alone. The ones the draw distance
        // cuts through are collected for pass 2, so the scratch vector can be sized once.
        std::vector<const FoliageBucket*> straddlers;
        size_t straddlerInstances = 0;
        struct Whole { const FoliageBucket* bucket; float nearSq; };
        std::vector<Whole> wholes;
        for (const FoliageBucket& b : store.buckets)
        {
            const HE::AABB wb = worldBox(b.localBounds, layer.kind, parent);
            float nearSq, farSq;
            rangeOf(wb, cam, nearSq, farSq);
            if (nearSq > outside) continue;                       // the whole bucket is out of range
            if (farSq <= inside) wholes.push_back({ &b, nearSq });   // the whole bucket is in range
            else { straddlers.push_back(&b); straddlerInstances += b.count; }
        }
        if (!straddlers.empty())
        {
            if (!scratch) scratch = std::make_shared<std::vector<glm::mat4>>();
            scratch->reserve(scratch->size() + straddlerInstances);
        }

        // The box a bucket's cluster is culled with: where its instances are, plus how far
        // the mesh reaches from each of them, in the terrain's own axes and then moved by its
        // pose. Invalid (never culled) when the mesh's bounds are unknown — see extractFoliage.
        auto clusterBounds = [&](const FoliageBucket& b) -> HE::AABB
        {
            if (!reach.known) return HE::AABB{};
            HE::AABB lb = b.localBounds;
            const float h = reach.horizontal + kWindReserve;
            lb.min.x -= h; lb.max.x += h;
            lb.min.z -= h; lb.max.z += h;
            lb.min.y += reach.below - kWindReserve;
            lb.max.y += reach.above + kWindReserve;
            return worldBox(lb, layer.kind, parent);
        };

        auto emit = [&](const InstanceBlock& block, const HE::AABB& bounds, float nearSq)
        {
            RenderObject obj;
            obj.meshAssetId     = fol.meshAssetId;
            obj.materialAssetId = fol.materialAssetId;
            obj.transform       = block.world(0);          // the first instance: what a cluster-unaware reader draws
            obj.worldBounds     = bounds;
            obj.entityId        = static_cast<uint32_t>(e);
            // A bucket beyond the shadow distance stops casting: the cheapest shadow saving
            // there is. It counts as a caster while ANY part of it is within reach.
            obj.castsShadow     = fol.castsShadow && nearSq <= sd2 * (1.0f + kRangeMargin);
            obj.contributesAO   = fol.contributesAO;
            obj.instanceBlock   = static_cast<int32_t>(out.instanceBlocks.size());
            out.objects.push_back(obj);
            out.instanceBlocks.push_back(block);
            ++t_stats.clusters;
            t_stats.clusterInstances += block.count;
        };

        for (const Whole& w : wholes)
        {
            InstanceBlock block = layer;
            block.first = w.bucket->first;
            block.count = w.bucket->count;
            emit(block, clusterBounds(*w.bucket), w.nearSq);
        }

        // Pass 2: a bucket the draw distance cuts through. Its instances are tested one by
        // one — the same test, on the same world positions, the per-instance path made — and
        // the survivors become a cluster of their own over the frame's scratch vector.
        for (const FoliageBucket* b : straddlers)
        {
            ++t_stats.straddlingBuckets;
            t_stats.instanceTests += b->count;
            const uint32_t from = static_cast<uint32_t>(scratch->size());
            for (uint32_t i = b->first; i < b->first + b->count; ++i)
            {
                // Position first: most of a straddling bucket's plants are rejected, and a
                // rejected plant costs three floats instead of a 64-byte matrix.
                const glm::vec3 wp = layer.position(i);
                const float dx = wp.x - cam.x;
                const float dz = wp.z - cam.z;
                if (dx * dx + dz * dz > dd2) continue;
                scratch->push_back(layer.world(i));
            }
            const uint32_t kept = static_cast<uint32_t>(scratch->size()) - from;
            if (kept == 0) continue;

            InstanceBlock block;
            block.matrices = scratch;       // already world space: no parent
            block.first    = from;
            block.count    = kept;
            block.kind     = InstanceBlock::Parent::Identity;

            float nearSq, farSq;
            rangeOf(worldBox(b->localBounds, layer.kind, parent), cam, nearSq, farSq);
            emit(block, clusterBounds(*b), nearSq);
        }
    }

    if (envStatsOn())
    {
        // The first extract that has a layer to report (the scene may load after frame 0), then
        // one in 240.
        static unsigned s_calls = 0;
        static bool     s_first = true;
        const bool      due     = (t_stats.layers > 0 && s_first) || ++s_calls % 240 == 0;
        if (t_stats.layers > 0) s_first = false;
        if (due)
            HE_LOG_INFO(Render, "FoliageExtract: %u layer(s), %u bucket(s) looked at, %u cluster(s) for %llu of "
                                "%llu instance(s), %u straddling bucket(s) (%llu instance test(s))%s",
                        t_stats.layers, t_stats.buckets, t_stats.clusters,
                        static_cast<unsigned long long>(t_stats.clusterInstances),
                        static_cast<unsigned long long>(t_stats.totalInstances),
                        t_stats.straddlingBuckets, static_cast<unsigned long long>(t_stats.instanceTests),
                        mode == FoliageMode::Clusters ? ""
                        : mode == FoliageMode::Ordered ? " [ordered verification mode]"
                                                       : " [clusters OFF: per-instance path]");
    }
}

} // namespace HE
