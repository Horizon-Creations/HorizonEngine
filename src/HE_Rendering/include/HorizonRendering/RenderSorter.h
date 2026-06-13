#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <vector>
#include <cstdint>

class HE_RENDERING_API RenderSorter {
public:
    void sort(const RenderWorld&       world,
              const std::vector<bool>& visible,
              std::vector<uint32_t>&   outSortedIndices);
};
