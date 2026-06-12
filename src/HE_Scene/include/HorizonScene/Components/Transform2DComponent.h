#pragma once
#include <Math/Math.h>

struct Transform2DComponent {
    glm::vec2 position    = glm::vec2(0.0f);
    float     rotation    = 0.0f;             // degrees
    glm::vec2 scale       = glm::vec2(1.0f);
    bool      dirty       = true;

    glm::mat3 worldMatrix = glm::mat3(1.0f);
};
