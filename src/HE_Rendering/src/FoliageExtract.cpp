#include "FoliageExtract.h"
#include "HorizonRendering/RenderWorld.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/Components/FoliageComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/EntityActive.h>
#include <Diagnostics/Log.h>
#include <Diagnostics/Profiler.h>
#include <JobSystem/JobSystem.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace HE
{

// What one straddler found: its survivors (world matrices, grouped by level) and the clusters
// to cut from them. `from` indexes `mats`; the main thread rebases it onto the frame's scratch.
struct StraddlerResult
{
    struct Cluster { int level; uint32_t from; uint32_t count; float nearSq; };
    std::vector<glm::mat4> mats;
    std::vector<Cluster>   clusters;
    std::vector<uint8_t>   levelOf;   // runStraddler's per-plant scratch, kept between calls
    uint64_t thinned = 0, held = 0;
};

// The straddler results of one view, kept between extracts: the vectors keep their capacity, so a
// frame that moves the camera does not allocate (and free from another thread) a matrix buffer for
// every straddler again.
struct StraddlerBuffers
{
    std::vector<StraddlerResult> results;
};

namespace
{
    // ── Switches ────────────────────────────────────────────────────────────
    int g_modeOverride = -1;   // tests; -1 = the environment decides

    // 0: the straddlers run one after the other on the calling thread (what the extract did before
    // they were pool jobs; the result is the same, bit for bit). HE_FOLIAGE_PARALLEL=0 or a test.
    int g_parallelOverride = []{
        const char* v = std::getenv("HE_FOLIAGE_PARALLEL");
        return (v && *v && std::atoi(v) == 0) ? 0 : -1;
    }();

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

    // ── The default path: clusters per bucket, straddlers on the pool ────────
    // extractFoliage decides every layer's buckets from their boxes alone (cheap, serial) and
    // gathers the buckets the draw distance or a level band cuts through. Those are tested plant
    // by plant, which is where the time goes (a straddler is hundreds of tests and matrix
    // products), and one straddler never reads or writes another's data: the plants' level bytes
    // live in disjoint ranges of FoliageLodMemory::Layer::plant. So all straddlers of the frame run
    // as pool jobs into their OWN buffers, and the main thread then appends their matrices and
    // emits the clusters in layer order, straddler order, level order — the order the serial walk
    // produced, so out.objects, out.instanceBlocks and the scratch vector come out identical.
    struct Whole { uint32_t bucket; float nearSq; int level; };

    struct LayerPlan
    {
        entt::entity           entity = entt::null;
        const FoliageLayer*    fol    = nullptr;
        Levels                 L;
        InstanceBlock          layer;          // the layer's stored matrices under the terrain's pose
        bool                   leveled = false;
        float                  dd2 = 0.0f, sd2 = 0.0f;
        FoliageLodMemory::Layer* mem = nullptr;
        std::vector<Whole>     wholes;
        std::vector<uint32_t>  straddlers;     // bucket indices
        size_t                 firstTask = 0;  // into the frame's task list
    };

    struct StraddlerTask { uint32_t plan; uint32_t bucket; };


    // The box a bucket's cluster is culled with: where its instances are, plus how far
    // the level's mesh reaches from each of them, in the terrain's own axes and then moved
    // by its pose. Invalid (never culled) when the mesh's bounds are unknown — see extractFoliage.
    HE::AABB clusterBoundsOf(const LayerPlan& P, const FoliageBucket& b, int level)
    {
        const Reach& reach = P.L.lv[level].reach;
        if (!reach.known) return HE::AABB{};
        HE::AABB lb = b.localBounds;
        const float h = reach.horizontal + kWindReserve;
        lb.min.x -= h; lb.max.x += h;
        lb.min.z -= h; lb.max.z += h;
        lb.min.y += reach.below - kWindReserve;
        lb.max.y += reach.above + kWindReserve;
        return worldBox(lb, P.layer.kind, P.layer.parent);
    }

    // Pass 1 of a layer: decide each bucket from its box alone. The ones the draw distance or a
    // level band cuts through are kept for the straddler jobs.
    void planBuckets(LayerPlan& P, const glm::vec3& cam)
    {
        const FoliageLayer& fol = *P.fol;
        const FoliageStore& store = *fol.store;
        const float dd2     = P.dd2;
        const float inside  = dd2 * (1.0f - kRangeMargin);
        const float outside = dd2 * (1.0f + kRangeMargin);
        for (uint32_t bi = 0; bi < store.buckets.size(); ++bi)
        {
            const FoliageBucket& b = store.buckets[bi];
            const HE::AABB wb = worldBox(b.localBounds, P.layer.kind, P.layer.parent);
            float nearSq, farSq;
            rangeOf(wb, cam, nearSq, farSq);
            if (nearSq > outside)                                  // the whole bucket is out of range
            {
                if (P.mem) P.mem->bucket[bi] = kUnknown;
                continue;
            }
            const int level = farSq <= inside ? P.L.whole(nearSq, farSq) : -1;
            if (level >= 0) P.wholes.push_back({ bi, nearSq, level });   // in range, and all on one level
            else P.straddlers.push_back(bi);
        }
        // The plants' level bytes are sized once, before any job writes into its slice of them.
        if (P.mem && P.leveled && !P.straddlers.empty() && P.mem->plant.size() != store.local.size())
            P.mem->plant.assign(store.local.size(), kUnknown);
    }

    // Pass 2 of one straddler: a bucket the draw distance or a level band cuts through. Its
    // instances are tested one by one — the same distance test, on the same world positions, the
    // per-instance path made. Runs on any thread: reads the store, writes its own result and the
    // level bytes of its own bucket only.
    void runStraddler(const LayerPlan& P, uint32_t bi, const glm::vec3& cam, StraddlerResult& r)
    {
        const FoliageStore&  store = *P.fol->store;
        const FoliageBucket& b     = store.buckets[bi];
        const InstanceBlock& layer = P.layer;
        const Levels&        L     = P.L;
        const float          dd2   = P.dd2;
        float nearSq, farSq;
        rangeOf(worldBox(b.localBounds, layer.kind, layer.parent), cam, nearSq, farSq);

        // At most one matrix per plant of the bucket, whatever the level: one allocation, no regrowth.
        r.mats.reserve(b.count);

        if (!P.leveled)
        {
            for (uint32_t i = b.first; i < b.first + b.count; ++i)
            {
                // Position first: most of a straddling bucket's plants are rejected, and a
                // rejected plant costs three floats instead of a 64-byte matrix.
                const glm::vec3 wp = layer.position(i);
                const float dx = wp.x - cam.x;
                const float dz = wp.z - cam.z;
                if (dx * dx + dz * dz > dd2) continue;
                r.mats.push_back(layer.world(i));
            }
            if (!r.mats.empty()) r.clusters.push_back({ 0, 0, static_cast<uint32_t>(r.mats.size()), nearSq });
            return;
        }

        // The levels this bucket's plants had: their own bytes when a boundary already
        // split it last time, otherwise the level the whole bucket had (or none).
        uint8_t* had = nullptr;
        if (P.mem)
        {
            had = P.mem->plant.data() + b.first;
            if (P.mem->bucket[bi] != kMixed) std::fill(had, had + b.count, P.mem->bucket[bi]);
            P.mem->bucket[bi] = kMixed;
        }
        uint32_t keep[kLevels] = {};
        for (int k = 0; k < L.n; ++k) keep[k] = keptOf(L.lv[k].density, b.count);
        uint32_t perLevel[kLevels] = {};
        // Per plant of the bucket; kUnknown = not drawn. Reused across calls (same `r` every frame,
        // see StraddlerBuffers) instead of a fresh heap allocation per bucket per frame.
        std::vector<uint8_t>& levelOf = r.levelOf;
        if (levelOf.size() < b.count) levelOf.resize(b.count);
        std::fill(levelOf.begin(), levelOf.begin() + b.count, kUnknown);
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
            if (held) ++r.held;
            if (j >= keep[level]) { ++r.thinned; continue; }
            levelOf[j] = static_cast<uint8_t>(level);
            ++perLevel[level];
        }
        for (int level = 0; level < L.n; ++level)
        {
            if (perLevel[level] == 0) continue;
            const uint32_t from = static_cast<uint32_t>(r.mats.size());
            for (uint32_t j = 0; j < b.count; ++j)
                if (levelOf[j] == level) r.mats.push_back(layer.world(b.first + j));
            r.clusters.push_back({ level, from, perLevel[level], nearSq });
        }
    }

    // Runs the straddler jobs, then emits every layer's clusters in order. `scratch` is the frame's
    // vector of already-world-space matrices that straddler clusters point into.
    void emitPlans(std::vector<LayerPlan>& plans, const glm::vec3& cam, RenderWorld& out,
                   std::shared_ptr<std::vector<glm::mat4>>& scratch, FoliageLodMemory* memory)
    {
        std::vector<StraddlerTask> tasks;
        for (uint32_t pi = 0; pi < plans.size(); ++pi)
        {
            plans[pi].firstTask = tasks.size();
            for (const uint32_t bi : plans[pi].straddlers) tasks.push_back({ pi, bi });
        }
        StraddlerBuffers local;
        StraddlerBuffers* buffers = &local;
        if (memory)
        {
            if (!memory->buffers) memory->buffers = std::make_shared<StraddlerBuffers>();
            buffers = memory->buffers.get();
        }
        std::vector<StraddlerResult>& results = buffers->results;
        if (results.size() < tasks.size()) results.resize(tasks.size());
        for (size_t t = 0; t < tasks.size(); ++t)
        {
            StraddlerResult& r = results[t];
            r.mats.clear(); r.clusters.clear(); r.thinned = 0; r.held = 0;
        }
        {
        HE_PROFILE_SCOPE_N("FoliageStraddlers");
        auto run = [&](size_t t) { runStraddler(plans[tasks[t].plan], tasks[t].bucket, cam, results[t]); };
        // Two layers over one store would share their level memory: in that (never authored) case the
        // second layer must see what the first wrote, so they run in order.
        bool sharedMemory = false;
        for (size_t a = 0; a < plans.size() && !sharedMemory; ++a)
            for (size_t c = a + 1; c < plans.size() && plans[a].mem; ++c)
                sharedMemory = sharedMemory || plans[a].mem == plans[c].mem;
        if (g_parallelOverride == 0 || sharedMemory) for (size_t t = 0; t < tasks.size(); ++t) run(t);
        else parallel_for(tasks.size(), run, "ExtractFoliageStraddlers", 4);
        }

        HE_PROFILE_SCOPE_N("FoliageEmitClusters");
        size_t matrices = 0;
        for (size_t t = 0; t < tasks.size(); ++t) matrices += results[t].mats.size();
        if (!tasks.empty() && !scratch) scratch = std::make_shared<std::vector<glm::mat4>>();
        if (matrices) scratch->reserve(scratch->size() + matrices);

        for (const LayerPlan& P : plans)
        {
            const FoliageLayer&  fol   = *P.fol;
            const FoliageStore&  store = *fol.store;
            const Levels&        L     = P.L;

            auto emit = [&](const InstanceBlock& block, const HE::AABB& bounds, float nearSq, int level)
            {
                RenderObject obj;
                obj.meshAssetId     = L.lv[level].mesh;
                obj.materialAssetId = L.lv[level].material;
                obj.transform       = block.world(0);          // the first instance: what a cluster-unaware reader draws
                obj.worldBounds     = bounds;
                obj.entityId        = static_cast<uint32_t>(P.entity);
                // A bucket beyond the shadow distance stops casting: the cheapest shadow saving
                // there is. It counts as a caster while ANY part of it is within reach.
                obj.castsShadow     = fol.castsShadow && nearSq <= P.sd2 * (1.0f + kRangeMargin);
                obj.contributesAO   = fol.contributesAO;
                obj.instanceBlock   = static_cast<int32_t>(out.instanceBlocks.size());
                out.objects.push_back(obj);
                out.instanceBlocks.push_back(block);
                ++t_stats.clusters;
                t_stats.clusterInstances += block.count;
                t_stats.levelInstances[level] += block.count;
            };

            for (const Whole& w : P.wholes)
            {
                const FoliageBucket& b = store.buckets[w.bucket];
                if (P.mem) P.mem->bucket[w.bucket] = static_cast<uint8_t>(w.level);
                // A thinned level draws a prefix of the bucket: still one range, no copy.
                const uint32_t kept = keptOf(L.lv[w.level].density, b.count);
                t_stats.thinnedInstances += b.count - kept;
                if (kept == 0) continue;
                InstanceBlock block = P.layer;
                block.first = b.first;
                block.count = kept;
                emit(block, clusterBoundsOf(P, b, w.level), w.nearSq, w.level);
            }

            for (size_t k = 0; k < P.straddlers.size(); ++k)
            {
                const FoliageBucket& b = store.buckets[P.straddlers[k]];
                StraddlerResult& r = results[P.firstTask + k];
                ++t_stats.straddlingBuckets;
                t_stats.instanceTests += b.count;
                t_stats.thinnedInstances += r.thinned;
                t_stats.heldInstances    += r.held;
                const uint32_t base = static_cast<uint32_t>(scratch ? scratch->size() : 0);
                if (!r.mats.empty()) scratch->insert(scratch->end(), r.mats.begin(), r.mats.end());
                for (const StraddlerResult::Cluster& c : r.clusters)
                {
                    InstanceBlock block;
                    block.matrices = scratch;       // already world space: no parent
                    block.first    = base + c.from;
                    block.count    = c.count;
                    block.kind     = InstanceBlock::Parent::Identity;
                    emit(block, clusterBoundsOf(P, b, c.level), c.nearSq, c.level);
                }
            }
        }
    }

    // ── Replaying a standing camera's extract ────────────────────────────────
    // Everything the walk of the default mode reads, as raw words (floats by their bits, so only an
    // exactly equal input matches): the camera, and per layer its entity, flags, distances, the
    // terrain's pose, the detail levels with the meshes' reach (a mesh whose bounds became known
    // changes the cluster boxes). The layer's store is compared by identity next to it. What the
    // walk reads and this does not spell out is the thing to add here.
    using Words = std::vector<uint64_t>;

    uint64_t bitsOf(float f) { uint32_t u; std::memcpy(&u, &f, sizeof u); return u; }

    void addCameraToKey(Words& key, const glm::vec3& cam)
    {
        key.push_back(bitsOf(cam.x)); key.push_back(bitsOf(cam.y)); key.push_back(bitsOf(cam.z));
    }

    void addToKey(Words& key, std::vector<std::weak_ptr<const FoliageStore>>& stores, const LayerPlan& P)
    {
        const FoliageLayer& fol = *P.fol;
        stores.push_back(fol.store);
        key.push_back(static_cast<uint32_t>(P.entity));
        key.push_back((fol.castsShadow ? 1u : 0u) | (fol.contributesAO ? 2u : 0u) | (P.leveled ? 4u : 0u));
        key.push_back(bitsOf(P.dd2)); key.push_back(bitsOf(P.sd2));
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) key.push_back(bitsOf(P.layer.parent[c][r]));
        key.push_back(static_cast<uint64_t>(P.L.n));
        key.push_back(bitsOf(P.L.hysteresis));
        for (int k = 0; k < P.L.n; ++k)
        {
            const Level& l = P.L.lv[k];
            key.push_back(l.mesh.hi); key.push_back(l.mesh.lo);
            key.push_back(l.material.hi); key.push_back(l.material.lo);
            key.push_back(bitsOf(l.from)); key.push_back(bitsOf(l.density));
            key.push_back(l.reach.known ? 1u : 0u);
            key.push_back(bitsOf(l.reach.horizontal)); key.push_back(bitsOf(l.reach.below)); key.push_back(bitsOf(l.reach.above));
        }
    }

    // What this view remembers of the layer's levels (only a layer with levels needs it).
    void attachMemory(LayerPlan& P, FoliageLodMemory* memory)
    {
        if (!P.leveled || !memory) return;
        const std::shared_ptr<const FoliageStore>& st = P.fol->store;
        FoliageLodMemory::Layer& m = memory->layers[st.get()];
        if (m.store.lock() != st || m.bucket.size() != st->buckets.size())
        {
            m = FoliageLodMemory::Layer{};
            m.store = st;
            m.bucket.assign(st->buckets.size(), kUnknown);
        }
        m.frame = memory->frame;
        P.mem = &m;
    }

    int g_replayOverride = []{
        const char* v = std::getenv("HE_FOLIAGE_MEMO");
        return (v && *v && std::atoi(v) == 0) ? 0 : -1;
    }();

    void logStats(FoliageMode mode)
    {
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
}

FoliageMode foliageMode()
{
    return g_modeOverride >= 0 ? static_cast<FoliageMode>(g_modeOverride) : envMode();
}

void setFoliageModeOverride(int mode)
{
    g_modeOverride = mode;
}

void setFoliageParallelOverride(int parallel)
{
    g_parallelOverride = parallel;
}

void setFoliageReplayOverride(int replay)
{
    g_replayOverride = replay;
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
    std::vector<LayerPlan> plans;   // default mode: the layers, emitted together once their straddlers are done
    Words key;                      // default mode with a level memory: what the walk reads (see FoliageLodMemory::Replay)
    std::vector<std::weak_ptr<const FoliageStore>> stores;
    if (mode == FoliageMode::Clusters && memory) addCameraToKey(key, cam);

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

            LayerPlan P;
            P.entity  = e;
            P.fol     = &fol;
            P.L       = L;
            P.layer   = layer;
            P.leveled = leveled;
            P.dd2     = fol.drawDistance * fol.drawDistance;
            const float sd = fol.shadowDistance > 0.0f ? fol.shadowDistance : fol.drawDistance;
            P.sd2     = sd * sd;

            if (mode == FoliageMode::Clusters && memory) addToKey(key, stores, P);
            plans.push_back(std::move(P));
        }
    }

    // A camera that stands still, over layers nobody touched: the objects of the extract before are
    // this one's. (Recorded from the second extract with the same inputs, see FoliageLodMemory::Replay.)
    FoliageLodMemory::Replay* replay = (mode == FoliageMode::Clusters && memory && g_replayOverride != 0)
                                     ? &memory->replay : nullptr;
    bool record = false;
    if (replay)
    {
        bool same = replay->key == key && replay->stores.size() == stores.size();
        for (size_t i = 0; same && i < stores.size(); ++i)
            same = !replay->stores[i].owner_before(stores[i]) && !stores[i].owner_before(replay->stores[i]);
        if (same && replay->recorded)
        {
            const int32_t base = static_cast<int32_t>(out.instanceBlocks.size());
            out.instanceBlocks.insert(out.instanceBlocks.end(), replay->blocks.begin(), replay->blocks.end());
            out.objects.reserve(out.objects.size() + replay->objects.size());
            for (RenderObject o : replay->objects)
            {
                o.instanceBlock += base;
                out.objects.push_back(std::move(o));
            }
            t_stats = replay->stats;
            t_stats.replayed = true;
            logStats(mode);
            return;
        }
        if (same) record = true;
        else
        {
            replay->key = std::move(key);
            replay->stores = std::move(stores);
            replay->recorded = false;
            replay->objects.clear();
            replay->blocks.clear();
        }
    }

    {
        HE_PROFILE_SCOPE_N("FoliagePlanBuckets");
        for (LayerPlan& P : plans)
        {
            attachMemory(P, memory);
            planBuckets(P, cam);
        }
    }

    // The straddlers of every layer on the pool, then the clusters in layer order.
    const size_t objects0 = out.objects.size(), blocks0 = out.instanceBlocks.size();
    emitPlans(plans, cam, out, scratch, memory);
    if (record)
    {
        replay->objects.assign(out.objects.begin() + static_cast<std::ptrdiff_t>(objects0), out.objects.end());
        for (RenderObject& o : replay->objects) o.instanceBlock -= static_cast<int32_t>(blocks0);
        replay->blocks.assign(out.instanceBlocks.begin() + static_cast<std::ptrdiff_t>(blocks0), out.instanceBlocks.end());
        replay->stats = t_stats;
        replay->recorded = true;
    }

    // The levels of stores that were not extracted this time are forgotten: their layer was
    // hidden, removed or re-scattered (a new store), and a re-scatter every frame must not
    // make this grow.
    if (memory)
        for (auto it = memory->layers.begin(); it != memory->layers.end();)
            it = it->second.frame == memory->frame ? std::next(it) : memory->layers.erase(it);

    logStats(mode);
}

} // namespace HE
