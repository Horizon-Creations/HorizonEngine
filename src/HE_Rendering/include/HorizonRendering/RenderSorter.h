#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <vector>
#include <cstdint>

class HE_RENDERING_API RenderSorter {
public:
    void sort(const RenderWorld&          world,
              const std::vector<uint8_t>& visible,
              std::vector<uint32_t>&      outSortedIndices);

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
