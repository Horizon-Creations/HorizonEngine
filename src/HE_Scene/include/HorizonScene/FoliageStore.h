#pragma once
#include <Math/Math.h>
#include <Math/AABB.h>
#include <cstdint>
#include <vector>

// ── The renderer's view of one foliage layer ────────────────────────────────
// FoliageSystem scatters a layer once (deterministic, global, exactly the
// layout it always produced) and then sorts the result into buckets: a regular
// grid over the terrain, `bucketSize` metres a side. RenderExtractor reads THIS
// instead of walking every instance every frame: it classifies the few hundred
// buckets against the camera and emits one cluster per visible bucket
// (FoliageExtract.cpp), and the instances are only touched again where the
// front end unfolds a cluster into a draw (GeometryPass / batchDepthRuns).
//
// Derived data, never serialised: it is rebuilt from the scatter parameters and
// the density mask whenever FoliageComponent::dirty is set, exactly like
// cachedInstances. It is immutable once built and shared by `shared_ptr`, so a
// RenderWorld that outlives a re-scatter (a frame copy, a retained world) keeps
// reading the matrices it was extracted from.
struct FoliageBucket {
    uint32_t first = 0;       // this bucket's instances: FoliageStore::local[first, first + count)
    uint32_t count = 0;
    HE::AABB localBounds;     // of the instance ORIGINS, terrain-local — not of the meshes
};

struct FoliageStore {
    // Instance matrices RELATIVE TO THE TERRAIN ENTITY (position and height as the
    // scatter found them, no terrain offset), grouped by bucket. Inside a bucket the
    // scatter's own order is kept, so a prefix of a bucket is an even thinning.
    std::vector<glm::mat4>     local;
    std::vector<FoliageBucket> buckets;   // non-empty buckets only, row-major (z outer, x inner)
    float                      bucketSize = 32.0f;   // what the layer ASKED for (FoliageSystem compares against it)
    float                      edge       = 32.0f;   // the grid's real cell size in metres (coarser on a huge terrain)
    int                        gridX = 0;
    int                        gridZ = 0;
};
