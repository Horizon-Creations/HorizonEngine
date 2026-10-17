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
#include <string>
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

    Reach reachOf(const FoliageLayer& fol, const HE::AABB& mb)
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

    // ── Detail levels ───────────────────────────────────────────────────────
    constexpr uint8_t kUnknown = 0xFF;   // bucket or plant: no level remembered
    constexpr uint8_t kMixed   = 0xFE;   // bucket: a boundary split it, the plant bytes hold the levels
    constexpr int     kLevels  = FoliageExtractStats::kLevels;
    static_assert(FoliageLayer::kMaxLods + 1 == static_cast<size_t>(kLevels),
                  "FoliageExtractStats::kLevels is the layer's own mesh plus FoliageLayer::kMaxLods");

    // How many of a bucket's `count` plants a level of `density` keeps: the FIRST ones,
    // an even thinning because the scatter order is random. One expression for the
    // whole-bucket path and the plant-by-plant one, so a bucket that changes path from
    // one frame to the next does not change which of its plants are drawn.
    uint32_t keptOf(float density, uint32_t count)
    {
        if (density >= 1.0f) return count;
        if (!(density > 0.0f)) return 0;
        return std::min(count, static_cast<uint32_t>(static_cast<double>(density) * count + 0.5));
    }

    struct Level
    {
        HE::UUID mesh;
        HE::UUID material;
        float    from    = 0.0f;   // m, ground plane: where the level starts
        float    density = 1.0f;
        Reach    reach;
        int      group   = 0;      // the first level drawing the same mesh and material (Ordered)
    };

    // A layer's levels as this frame draws them: level 0 is the layer's own mesh from the
    // camera out, then FoliageLayer::lods by distance, the last up to drawDistance.
    struct Levels
    {
        Level lv[kLevels];
        int   n          = 1;
        float hysteresis = 0.0f;

        // By distance alone: the last level that has started.
        int plain(float d) const
        {
            int p = 0;
            for (int k = 1; k < n; ++k)
                if (lv[k].from <= d) p = k;
            return p;
        }

        // With the level the plant had (`prev`, kUnknown for none): a neighbouring level is
        // kept while the plant is within `hysteresis` of the boundary between the two.
        int choose(float d, uint8_t prev, bool& held) const
        {
            held = false;
            const int p = plain(d);
            if (prev >= n || prev == p) return p;
            const int hi = std::max<int>(prev, p);
            if (hi - std::min<int>(prev, p) != 1) return p;
            if (std::abs(d - lv[hi].from) > hysteresis) return p;
            held = true;
            return prev;
        }

        // Which level every plant of a bucket takes when its origins lie between
        // sqrt(nearSq) and sqrt(farSq) and that span is clear of every boundary's band
        // (boundary +- hysteresis, with the float margin): then `choose` gives `plain` for
        // each of them whatever they had before. -1 when a band runs through the bucket.
        int whole(float nearSq, float farSq) const
        {
            int level = 0;
            for (int k = 1; k < n; ++k)
            {
                const float lo = lv[k].from - hysteresis;
                const float hi = lv[k].from + hysteresis;
                if (lo > 0.0f && farSq < lo * lo * (1.0f - kRangeMargin)) continue;    // wholly nearer
                if (nearSq > hi * hi * (1.0f + kRangeMargin)) { level = k; continue; }  // wholly farther
                return -1;
            }
            return level;
        }
    };

    Levels levelsOf(const FoliageLayer& fol, const ContentManager* cm)
    {
        Levels L;
        L.lv[0].mesh     = fol.meshAssetId;
        L.lv[0].material = fol.materialAssetId;
        L.lv[0].reach    = reachOf(fol, meshLocalBounds(cm, fol.meshAssetId));
        L.hysteresis     = std::max(0.0f, fol.lodHysteresis);
        const size_t take = std::min(fol.lods.size(), static_cast<size_t>(kLevels - 1));
        for (size_t i = 0; i < take; ++i)
        {
            const FoliageLod& d = fol.lods[i];
            Level& l = L.lv[L.n++];
            // A level without a mesh only thins the layer's; without a material it takes
            // the LAYER's, which is what a simpler mesh of the same plant wants.
            l.mesh     = d.meshAssetId != HE::UUID{} ? d.meshAssetId : fol.meshAssetId;
            l.material = d.materialAssetId != HE::UUID{} ? d.materialAssetId : fol.materialAssetId;
            l.from     = std::max(0.0f, d.distance);
            l.density  = d.density;
            l.reach    = l.mesh == fol.meshAssetId ? L.lv[0].reach : reachOf(fol, meshLocalBounds(cm, l.mesh));
        }
        // The loader sorts the levels; the inspector may not while a distance is dragged.
        std::stable_sort(L.lv + 1, L.lv + L.n, [](const Level& a, const Level& b) { return a.from < b.from; });
        for (int k = 0; k < L.n; ++k)
        {
            L.lv[k].group = k;
            for (int g = 0; g < k; ++g)
                if (L.lv[g].mesh == L.lv[k].mesh && L.lv[g].material == L.lv[k].material) { L.lv[k].group = g; break; }
        }
        return L;
    }

    // ── The old path ────────────────────────────────────────────────────────
    // One RenderObject per instance in range, bounds left invalid for the backends
    // to refine — what extractFoliage did before clusters (HE_FOLIAGE_CLUSTERS=0).
    // Detail levels by distance alone (no hysteresis), the same thinning as the clusters.
    void extractPerInstance(const FoliageLayer& fol, const Levels& L, entt::entity e, const InstanceBlock& all,
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
                const int level = L.n > 1 ? L.plain(std::sqrt(dx * dx + dz * dz)) : 0;
                if (i - b.first >= keptOf(L.lv[level].density, b.count)) { ++t_stats.thinnedInstances; continue; }
                ++t_stats.levelInstances[level];
                const glm::mat4 inst = all.world(i);

                RenderObject obj;
                obj.meshAssetId     = L.lv[level].mesh;
                obj.materialAssetId = L.lv[level].material;
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
    // the per-instance objects (mesh first — one mesh per level here — then squared 3D distance
    // from the camera to the instance's origin, nearest first), as ONE cluster per mesh and
    // material over the frame's scratch. Levels by distance alone, as the per-instance path.
    void extractOrdered(const FoliageLayer& fol, const Levels& L, entt::entity e, const InstanceBlock& all,
                        const glm::vec3& cam, RenderWorld& out,
                        std::shared_ptr<std::vector<glm::mat4>>& scratch)
    {
        const FoliageStore& store = *fol.store;
        const float dd2 = fol.drawDistance * fol.drawDistance;
        struct Item { int group; float distSq; uint32_t index; };
        std::vector<Item> items;
        items.reserve(store.local.size());
        for (const FoliageBucket& b : store.buckets)
            for (uint32_t i = b.first; i < b.first + b.count; ++i)
            {
                const glm::vec3 wp = all.position(i);
                const float dx = wp.x - cam.x, dz = wp.z - cam.z;
                if (dx * dx + dz * dz > dd2) continue;
                const int level = L.n > 1 ? L.plain(std::sqrt(dx * dx + dz * dz)) : 0;
                if (i - b.first >= keptOf(L.lv[level].density, b.count)) { ++t_stats.thinnedInstances; continue; }
                ++t_stats.levelInstances[level];
                const glm::vec3 d = wp - cam;
                items.push_back({ L.lv[level].group, glm::dot(d, d), i });
            }
        t_stats.instanceTests += store.local.size();
        if (items.empty()) return;
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b)
        {
            if (a.group != b.group) return a.group < b.group;
            return a.distSq != b.distSq ? a.distSq < b.distSq : a.index < b.index;
        });

        if (!scratch) scratch = std::make_shared<std::vector<glm::mat4>>();
        for (size_t first = 0; first < items.size();)
        {
            size_t last = first;
            while (last < items.size() && items[last].group == items[first].group) ++last;

            InstanceBlock block;
            block.matrices = scratch;
            block.first    = static_cast<uint32_t>(scratch->size());
            block.count    = static_cast<uint32_t>(last - first);
            block.kind     = InstanceBlock::Parent::Identity;   // the scratch holds world matrices
            for (size_t k = first; k < last; ++k) scratch->push_back(all.world(items[k].index));

            const Level& lv = L.lv[items[first].group];
            RenderObject obj;
            obj.meshAssetId     = lv.mesh;
            obj.materialAssetId = lv.material;
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
            first = last;
        }
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

void extractFoliage(entt::registry& reg, RenderWorld& out, const ContentManager* cm, FoliageLodMemory* memory)
{
    // Its own scope: the foliage share of RenderExtractor::extract, which scaled with
    // every cached instance before clusters and now scales with the buckets in range.
    HE_PROFILE_SCOPE_N("ExtractFoliage");
    t_stats = FoliageExtractStats{};

    const FoliageMode mode = foliageMode();
    const HE::ActiveFilter active(reg);
    const glm::vec3 cam = out.camera.position;
    if (memory) ++memory->frame;

    // The instances of the buckets the draw distance cuts through, filtered one by one.
    // One vector for the whole frame; every such cluster is a range of it. Created on
    // first use, because most frames have no straddler.
    std::shared_ptr<std::vector<glm::mat4>> scratch;

    for (auto [entityBinding, folBinding] : reg.view<FoliageComponent>().each())
    {
        // Plain names for what the lambdas below capture: older compilers reject a lambda
        // that captures a structured binding.
        const entt::entity     e    = entityBinding;
        const FoliageComponent& comp = folBinding;
        if (!comp.visible) continue; // hidden (e.g. a preloaded zone)
        if (active.off(e)) continue;

        // The terrain's pose NOW, so a layer follows a terrain that is moved, rotated, scaled
        // or parented after the scatter (the instances are stored relative to it).
        glm::mat4 parent(1.0f);
        if (const auto* tf = reg.try_get<TransformComponent>(e)) parent = tf->worldMatrix;

        // Each layer is its own set of clusters: its own mesh, material, distances and flags.
        for (const FoliageLayer& fol : comp.layers)
        {
            if (!fol.visible) continue;   // the layer's own switch
            if (fol.meshAssetId == HE::UUID{}) continue;
            if (!fol.store || fol.store->buckets.empty()) continue;   // not scattered (yet), or nothing grew

            const FoliageStore& store = *fol.store;
            ++t_stats.layers;
            t_stats.buckets        += static_cast<uint32_t>(store.buckets.size());
            t_stats.totalInstances += store.local.size();

            InstanceBlock layer;
            layer.matrices = std::shared_ptr<const std::vector<glm::mat4>>(fol.store, &store.local);
            layer.parent   = parent;
            layer.kind     = InstanceBlock::classify(parent);

            const Levels L       = levelsOf(fol, cm);
            const bool   leveled = L.n > 1;

            if (mode == FoliageMode::PerInstance)
            {
                extractPerInstance(fol, L, e, layer, cam, out);
                continue;
            }
            if (mode == FoliageMode::Ordered)
            {
                extractOrdered(fol, L, e, layer, cam, out, scratch);
                continue;
            }

            const float dd2     = fol.drawDistance * fol.drawDistance;
            const float inside  = dd2 * (1.0f - kRangeMargin);
            const float outside = dd2 * (1.0f + kRangeMargin);
            const float sd      = fol.shadowDistance > 0.0f ? fol.shadowDistance : fol.drawDistance;
            const float sd2     = sd * sd;

            // What this view remembers of the layer's levels (only a layer with levels needs it).
            FoliageLodMemory::Layer* mem = nullptr;
            if (leveled && memory)
            {
                FoliageLodMemory::Layer& m = memory->layers[fol.store.get()];
                if (m.store.lock() != fol.store || m.bucket.size() != store.buckets.size())
                {
                    m = FoliageLodMemory::Layer{};
                    m.store = fol.store;
                    m.bucket.assign(store.buckets.size(), kUnknown);
                }
                m.frame = memory->frame;
                mem = &m;
            }

            // Pass 1: decide each bucket from its box alone. The ones the draw distance or a
            // level band cuts through are collected for pass 2, so the scratch vector can be
            // sized once.
            std::vector<uint32_t> straddlers;
            size_t straddlerInstances = 0;
            struct Whole { uint32_t bucket; float nearSq; int level; };
            std::vector<Whole> wholes;
            for (uint32_t bi = 0; bi < store.buckets.size(); ++bi)
            {
                const FoliageBucket& b = store.buckets[bi];
                const HE::AABB wb = worldBox(b.localBounds, layer.kind, parent);
                float nearSq, farSq;
                rangeOf(wb, cam, nearSq, farSq);
                if (nearSq > outside)                                  // the whole bucket is out of range
                {
                    if (mem) mem->bucket[bi] = kUnknown;
                    continue;
                }
                const int level = farSq <= inside ? L.whole(nearSq, farSq) : -1;
                if (level >= 0) wholes.push_back({ bi, nearSq, level });   // in range, and all on one level
                else { straddlers.push_back(bi); straddlerInstances += b.count; }
            }
            if (!straddlers.empty())
            {
                if (!scratch) scratch = std::make_shared<std::vector<glm::mat4>>();
                scratch->reserve(scratch->size() + straddlerInstances);
            }

            // The box a bucket's cluster is culled with: where its instances are, plus how far
            // the level's mesh reaches from each of them, in the terrain's own axes and then moved
            // by its pose. Invalid (never culled) when the mesh's bounds are unknown — see extractFoliage.
            auto clusterBounds = [&](const FoliageBucket& b, int level) -> HE::AABB
            {
                const Reach& reach = L.lv[level].reach;
                if (!reach.known) return HE::AABB{};
                HE::AABB lb = b.localBounds;
                const float h = reach.horizontal + kWindReserve;
                lb.min.x -= h; lb.max.x += h;
                lb.min.z -= h; lb.max.z += h;
                lb.min.y += reach.below - kWindReserve;
                lb.max.y += reach.above + kWindReserve;
                return worldBox(lb, layer.kind, parent);
            };

            auto emit = [&](const InstanceBlock& block, const HE::AABB& bounds, float nearSq, int level)
            {
                RenderObject obj;
                obj.meshAssetId     = L.lv[level].mesh;
                obj.materialAssetId = L.lv[level].material;
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
                t_stats.levelInstances[level] += block.count;
            };

            for (const Whole& w : wholes)
            {
                const FoliageBucket& b = store.buckets[w.bucket];
                if (mem) mem->bucket[w.bucket] = static_cast<uint8_t>(w.level);
                // A thinned level draws a prefix of the bucket: still one range, no copy.
                const uint32_t kept = keptOf(L.lv[w.level].density, b.count);
                t_stats.thinnedInstances += b.count - kept;
                if (kept == 0) continue;
                InstanceBlock block = layer;
                block.first = b.first;
                block.count = kept;
                emit(block, clusterBounds(b, w.level), w.nearSq, w.level);
            }

            // Pass 2: a bucket the draw distance or a level band cuts through. Its instances are
            // tested one by one — the same distance test, on the same world positions, the
            // per-instance path made — and the survivors of each level become a cluster of their
            // own over the frame's scratch vector.
            std::vector<uint8_t> levelOf;   // per plant of the bucket at hand; kUnknown = not drawn
            for (const uint32_t bi : straddlers)
            {
                const FoliageBucket& b = store.buckets[bi];
                ++t_stats.straddlingBuckets;
                t_stats.instanceTests += b.count;
                float nearSq, farSq;
                rangeOf(worldBox(b.localBounds, layer.kind, parent), cam, nearSq, farSq);

                if (!leveled)
                {
                    const uint32_t from = static_cast<uint32_t>(scratch->size());
                    for (uint32_t i = b.first; i < b.first + b.count; ++i)
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
                    emit(block, clusterBounds(b, 0), nearSq, 0);
                    continue;
                }

                // The levels this bucket's plants had: their own bytes when a boundary already
                // split it last time, otherwise the level the whole bucket had (or none).
                uint8_t* had = nullptr;
                if (mem)
                {
                    if (mem->plant.size() != store.local.size()) mem->plant.assign(store.local.size(), kUnknown);
                    had = mem->plant.data() + b.first;
                    if (mem->bucket[bi] != kMixed) std::fill(had, had + b.count, mem->bucket[bi]);
                    mem->bucket[bi] = kMixed;
                }
                uint32_t keep[kLevels] = {};
                for (int k = 0; k < L.n; ++k) keep[k] = keptOf(L.lv[k].density, b.count);
                uint32_t perLevel[kLevels] = {};
                levelOf.assign(b.count, kUnknown);
                for (uint32_t j = 0; j < b.count; ++j)
                {
                    const glm::vec3 wp = layer.position(b.first + j);
                    const float dx = wp.x - cam.x;
                    const float dz = wp.z - cam.z;
                    const float d2 = dx * dx + dz * dz;
                    if (d2 > dd2)                   // the draw distance stays a hard edge: no hysteresis
                    {
                        if (had) had[j] = kUnknown;
                        continue;
                    }
                    bool held = false;
                    const int level = had ? L.choose(std::sqrt(d2), had[j], held) : L.plain(std::sqrt(d2));
                    if (had) had[j] = static_cast<uint8_t>(level);
                    if (held) ++t_stats.heldInstances;
                    if (j >= keep[level]) { ++t_stats.thinnedInstances; continue; }
                    levelOf[j] = static_cast<uint8_t>(level);
                    ++perLevel[level];
                }
                for (int level = 0; level < L.n; ++level)
                {
                    if (perLevel[level] == 0) continue;
                    const uint32_t from = static_cast<uint32_t>(scratch->size());
                    for (uint32_t j = 0; j < b.count; ++j)
                        if (levelOf[j] == level) scratch->push_back(layer.world(b.first + j));

                    InstanceBlock block;
                    block.matrices = scratch;       // already world space: no parent
                    block.first    = from;
                    block.count    = perLevel[level];
                    block.kind     = InstanceBlock::Parent::Identity;
                    emit(block, clusterBounds(b, level), nearSq, level);
                }
            }
        }
    }

    // The levels of stores that were not extracted this time are forgotten: their layer was
    // hidden, removed or re-scattered (a new store), and a re-scatter every frame must not
    // make this grow.
    if (memory)
        for (auto it = memory->layers.begin(); it != memory->layers.end();)
            it = it->second.frame == memory->frame ? std::next(it) : memory->layers.erase(it);

    if (envStatsOn())
    {
        // The first extract that has a layer to report (the scene may load after frame 0), then
        // one in 240.
        static unsigned s_calls = 0;
        static bool     s_first = true;
        const bool      due     = (t_stats.layers > 0 && s_first) || ++s_calls % 240 == 0;
        if (t_stats.layers > 0) s_first = false;
        if (due)
        {
            // Instances per detail level, up to the last level any layer drew.
            std::string levels;
            int used = 0;
            for (int k = 0; k < kLevels; ++k)
                if (t_stats.levelInstances[k]) used = k + 1;
            for (int k = 0; k < used; ++k)
                levels += (k ? "/" : "") + std::to_string(t_stats.levelInstances[k]);
            HE_LOG_INFO(Render, "FoliageExtract: %u layer(s), %u bucket(s) looked at, %u cluster(s) for %llu of "
                                "%llu instance(s), %u straddling bucket(s) (%llu instance test(s)); "
                                "per level %s, %llu thinned, %llu held%s",
                        t_stats.layers, t_stats.buckets, t_stats.clusters,
                        static_cast<unsigned long long>(t_stats.clusterInstances),
                        static_cast<unsigned long long>(t_stats.totalInstances),
                        t_stats.straddlingBuckets, static_cast<unsigned long long>(t_stats.instanceTests),
                        levels.empty() ? "-" : levels.c_str(),
                        static_cast<unsigned long long>(t_stats.thinnedInstances),
                        static_cast<unsigned long long>(t_stats.heldInstances),
                        mode == FoliageMode::Clusters ? ""
                        : mode == FoliageMode::Ordered ? " [ordered verification mode]"
                                                       : " [clusters OFF: per-instance path]");
        }
    }
}

} // namespace HE
