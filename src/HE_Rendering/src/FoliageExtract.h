#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <entt/entt.hpp>
#include "HorizonRendering/RenderWorld.h"

class RenderWorld;
class ContentManager;
struct FoliageStore;

// ── Foliage → RenderWorld ───────────────────────────────────────────────────
// RenderExtractor's one call into this file (src/FoliageExtract.cpp). Internal to
// HorizonRendering: only RenderExtractor.cpp and the tests include it.
namespace HE
{

// What the last extractFoliage call put into `out` (this thread). The counters the
// plan asked for: how much of the foliage the front end will unfold. Also logged
// every few hundred extracts when HE_FOLIAGE_STATS is set.
struct FoliageExtractStats
{
    uint32_t layers               = 0;   // foliage layers that took part (visible, scattered)
    uint32_t buckets              = 0;   // buckets looked at (all of them: the O(buckets) part)
    uint32_t clusters             = 0;   // cluster objects emitted
    uint32_t straddlingBuckets    = 0;   // buckets the draw distance cut through (instance by instance)
    uint64_t totalInstances       = 0;   // instances in the stores
    uint64_t clusterInstances     = 0;   // instances the emitted clusters stand for
    uint64_t instanceTests        = 0;   // instances actually tested one by one (the straddlers)
    // Detail levels (FoliageLayer::lods); level 0 is the layer's own mesh. A layer
    // without levels counts everything as level 0.
    static constexpr int kLevels  = 8;   // FoliageLayer::kMaxLods + 1
    uint64_t levelInstances[kLevels] = {};   // instances emitted per level, after thinning
    uint64_t thinnedInstances     = 0;   // in range, but left out by a level's density
    uint64_t heldInstances        = 0;   // drawn with last frame's level although their distance says the next
    bool     replayed         = false;   // nothing changed since the last extract: its objects were appended again
};

struct StraddlerBuffers;   // FoliageExtract.cpp

// ── Detail levels: what a view remembers between frames ─────────────────────
// The hysteresis of FoliageLayer::lods needs to know which level a plant was drawn
// with last time, and "last time" belongs to a VIEW: the editor's scene view and a
// preview each look from their own camera. So the memory lives on the RenderExtractor
// (one per view) and is handed in here. Per layer store, one byte per bucket (the
// level the whole bucket had, or "mixed", or unknown) and, only once one of its
// buckets has been split by a level boundary, one byte per plant (its level).
// Entries of stores that were not extracted in a frame are dropped at its end, so a
// re-scatter every frame (a held sculpt brush) cannot make it grow.
struct FoliageLodMemory
{
    struct Layer
    {
        std::weak_ptr<const FoliageStore> store;   // a dead store's address may come back as another one
        std::vector<uint8_t> bucket;               // per FoliageStore::buckets entry
        std::vector<uint8_t> plant;                // per FoliageStore::local entry; empty until needed
        uint64_t             frame = 0;            // the extract that last used it
    };
    std::unordered_map<const FoliageStore*, Layer> layers;
    uint64_t frame = 0;
    std::shared_ptr<StraddlerBuffers> buffers;   // the pool jobs' result buffers, reused by the next extract

    // What the last extract appended to the RenderWorld, kept for the next one when nothing it read
    // has changed (a standing camera: the walk is a pure function of the camera, the terrain's pose,
    // the layers' settings and stores, and the meshes' known bounds, all of which `key` spells out
    // word by word). Replaying it appends the same objects and blocks without walking a bucket.
    // It is only recorded from the SECOND extract with a key: the level memory above moves with the
    // first one (a plant holds the level it had), and from the second on it has settled, so a replay
    // is what a walk would give, not what the walk before the memory settled gave.
    struct Replay
    {
        std::vector<uint64_t>                          key;       // every input of the walk, as raw words
        std::vector<std::weak_ptr<const FoliageStore>> stores;    // the layers' stores, in walk order (identity, not address)
        bool                                           recorded = false;
        FoliageExtractStats                            stats;     // what that extract counted
        std::vector<RenderObject>                      objects;   // RenderObject::instanceBlock relative to `blocks`
        std::vector<InstanceBlock>                     blocks;
    } replay;
};

// Emits every visible layer of every visible FoliageComponent into `out` — which must
// already hold the camera. By default ONE cluster RenderObject per bucket and detail
// level in range (RenderObject::instanceBlock, RenderWorld::instanceBlocks), so the
// cost is O(buckets) plus the instances of the few buckets the draw distance or a
// level boundary cuts through, not O(instances). `cm` supplies the mesh bounds the
// clusters are boxed with; null leaves the boxes invalid (never culled), exactly what
// an ordinary object gets for a mesh the extractor cannot read. `memory` carries the
// level hysteresis from one call to the next; null chooses every plant's level by its
// distance alone (what a first frame does anyway).
void extractFoliage(entt::registry& reg, RenderWorld& out, const ContentManager* cm,
                    FoliageLodMemory* memory = nullptr);

// How the foliage reaches the RenderWorld.
//   Clusters     the default: one cluster object per bucket in range (above).
//   PerInstance  one RenderObject per instance in range, as before the cluster path existed.
//                HE_FOLIAGE_CLUSTERS=0 turns a process to it: the A/B for a picture or a
//                measurement against the previous behaviour, and the way out if a cluster
//                misbehaves on a backend nobody could try.
//   Ordered      ONE cluster per layer holding every instance in range in the order the
//                per-instance path's RenderSorter hands them to a draw (front to back by 3D
//                distance, camera-relative). Costs O(instances log instances) per extract and
//                culls nothing: a verification mode (HE_FOLIAGE_CLUSTERS=ordered). Opaque
//                geometry that intersects (cube meets cube, a cube meets the ground) resolves
//                depth ties by draw order, so Clusters and PerInstance may differ in the odd
//                pixel along such an intersection; Ordered draws in PerInstance's order and
//                the picture is bit for bit the same, which is how the store, the unfolding
//                and the depth-only passes are shown to be exact on a real device. (It culls
//                nothing, so the bucket boxes are the unit tests' job, not its.)
enum class FoliageMode { PerInstance = 0, Clusters = 1, Ordered = 2 };
FoliageMode foliageMode();
// For tests: overrides the environment; pass -1 to go back to it.
void setFoliageModeOverride(int mode);
// For tests (and HE_FOLIAGE_PARALLEL=0): 0 runs the straddler buckets of the default mode on the
// calling thread instead of the pool; the output is the same. -1 = pool.
void setFoliageParallelOverride(int parallel);
// For tests (and HE_FOLIAGE_MEMO=0): 0 never replays a previous extract, 1 may; -1 = the environment (on).
void setFoliageReplayOverride(int replay);

const FoliageExtractStats& lastFoliageExtractStats();

} // namespace HE
