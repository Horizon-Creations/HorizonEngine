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
    // bounds; an invalid AABB is treated as always visible. Culls against the
    // scene camera.
    // outVisible[i] mirrors world.objects[i]. uint8_t avoids the packed-bit
    // representation of vector<bool>, which is not safe for concurrent element
    // writes (different indices may share the same storage word).
    void cull(const RenderWorld& world, std::vector<uint8_t>& outVisible);

    // Cull against an explicit view-projection (e.g. a shadow light's frustum)
    // rather than the scene camera, so a caster outside the camera view but
    // inside the given frustum is kept. Used by the shadow pass so off-screen
    // objects keep casting into the visible scene.
    void cull(const RenderWorld& world, const glm::mat4& viewProj,
              std::vector<uint8_t>& outVisible);
};
