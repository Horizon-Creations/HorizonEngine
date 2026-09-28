#pragma once
#include <Math/Math.h>
#include <Types/Enums.h>

using LightType = HE::LightType;

struct LightComponent {
    LightType type         = LightType::Point;
    glm::vec3 color        = glm::vec3(1.0f);
    float     intensity    = 1.0f;
    float     range        = 10.0f;   // point/spot only
    float     spotAngle    = 30.0f;   // spot only, degrees
    float     cullDistance = 0.0f;    // point/spot only: deactivated beyond this camera distance (0 = never)
    bool      castsShadow  = false;
    bool      visible      = true;  // extractor skips invisible (zone hiding)
};

namespace HE
{
    // The colour the EDITOR shows a light in: its icon and, when selected, its
    // range sphere / spot cone. The hue of `color` with its brightest channel
    // raised to 1 — brightness is intensity's job, and a dim red lamp drawn at
    // its own 0.1 would be a black icon and invisible lines. A colour with no
    // brightness to scale (black, or near it) has no hue either and shows white.
    inline glm::vec3 lightDisplayColor(const glm::vec3& color)
    {
        const float peak = glm::max(color.r, glm::max(color.g, color.b));
        if (!(peak > 1e-4f)) return glm::vec3(1.0f);
        return glm::max(color, glm::vec3(0.0f)) / peak;
    }
}
