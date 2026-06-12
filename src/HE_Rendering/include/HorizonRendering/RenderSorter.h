#pragma once
#include "RenderWorld.h"
#include <vector>
#include <cstdint>

class RenderSorter {
public:
    void sort(const RenderWorld&       world,
              const std::vector<bool>& visible,
              std::vector<uint32_t>&   outSortedIndices);
};
