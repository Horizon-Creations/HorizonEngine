#pragma once
#include <cstdint>
#include <entt/entt.hpp>

class RenderWorld;
class ContentManager;

// ── Foliage → RenderWorld ───────────────────────────────────────────────────
// RenderExtractor's one call into this file (src/FoliageExtract.cpp). Internal to
// HorizonRendering: only RenderExtractor.cpp and the tests include it.
namespace HE
{

// Emits the foliage of every visible FoliageComponent into `out` — which must
// already hold the camera. By default ONE cluster RenderObject per bucket that is
// in range (RenderObject::instanceBlock, RenderWorld::instanceBlocks), so the cost
// is O(buckets) plus the instances of the few buckets the draw distance cuts
// through, not O(instances). `cm` supplies the mesh bounds the clusters are boxed
// with; null leaves the boxes invalid (never culled), exactly what an ordinary
// object gets for a mesh the extractor cannot read.
void extractFoliage(entt::registry& reg, RenderWorld& out, const ContentManager* cm);

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

// What the last extractFoliage call put into `out` (this thread). The counters the
// plan asked for: how much of the foliage the front end will unfold. Also logged
// every few hundred extracts when HE_FOLIAGE_STATS is set.
struct FoliageExtractStats
{
    uint32_t layers               = 0;   // FoliageComponents that took part
    uint32_t buckets              = 0;   // buckets looked at (all of them: the O(buckets) part)
    uint32_t clusters             = 0;   // cluster objects emitted
    uint32_t straddlingBuckets    = 0;   // buckets the draw distance cut through (instance by instance)
    uint64_t totalInstances       = 0;   // instances in the stores
    uint64_t clusterInstances     = 0;   // instances the emitted clusters stand for
    uint64_t instanceTests        = 0;   // instances actually tested one by one (the straddlers)
};
const FoliageExtractStats& lastFoliageExtractStats();

} // namespace HE
