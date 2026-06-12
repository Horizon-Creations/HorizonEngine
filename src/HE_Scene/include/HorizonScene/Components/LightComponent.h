#pragma once
#include <Math/Math.h>
#include <Types/Enums.h>

using LightType = HE::LightType;

struct LightComponent {
    LightType type        = LightType::Point;
    glm::vec3 color       = glm::vec3(1.0f);
    float     intensity   = 1.0f;
    float     range       = 10.0f;    // point/spot only
    float     spotAngle   = 30.0f;    // spot only, degrees
    bool      castsShadow = false;
};
