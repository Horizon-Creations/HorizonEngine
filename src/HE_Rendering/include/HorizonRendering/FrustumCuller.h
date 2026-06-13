#pragma once
#include "../HE_RENDERING_API.h"
#include "RenderWorld.h"
#include <Math/AABB.h>
#include <vector>

// View frustum as six inward-facing planes (Gribb/Hartmann extraction).
struct Frustum
{
    glm::vec4 planes[6]; // xyz = normal, w = distance; inside when dot >= 0

    static Frustum fromViewProj(const glm::mat4& vp);

    // Conservative AABB test (center/extents form) — never culls a visible
    // box, may keep an invisible one.
    bool intersects(const HE::AABB& box) const;
};

class HE_RENDERING_API FrustumCuller {
public:
    // outVisible[i] mirrors world.objects[i]. Objects use their world-space
    // bounds; an invalid AABB is treated as always visible.
    void cull(const RenderWorld& world, std::vector<bool>& outVisible);
};
