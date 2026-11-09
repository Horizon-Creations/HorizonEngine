#include <HorizonScene/RvtProducer.h>
#include <HorizonScene/LandscapeHlodBaker.h>

namespace HE::rvt
{
    uint32_t paintPages(HE::landscape::HlodBaker& baker, const std::vector<PageRender>& pages, uint32_t pagePx)
    {
        uint32_t painted = 0;
        for (const PageRender& p : pages)
        {
            HE::landscape::HlodChunkRect rect;
            rect.min  = p.rectMin;
            rect.size = p.rectMax - p.rectMin;
            if (baker.renderRvtPage(rect, pagePx, p.slot, p.layers)) ++painted;
        }
        return painted;
    }
}
