#pragma once
// Runtime Virtual Textures, the producer (Thema 333 step 2): paints the pages RvtManager::update() asks for into the
// renderer's page pool. A page is the three unlit views of the landscape material (HlodBaker's view graphs), seen from
// straight above over the page's world rect, border included, each copied on the GPU into its pool slice.
#include <HorizonScene/RuntimeVirtualTexture.h>
#include <cstdint>
#include <vector>

namespace HE::landscape { class HlodBaker; }

namespace HE::rvt
{
    // Returns how many of `pages` were painted (all of them unless the renderer refused one). `pagePx` is the pool's
    // page size (VolumeLayout::physicalPageSize()).
    uint32_t paintPages(HE::landscape::HlodBaker& baker, const std::vector<PageRender>& pages, uint32_t pagePx);
}
