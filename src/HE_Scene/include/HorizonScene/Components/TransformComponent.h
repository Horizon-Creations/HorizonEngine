#pragma once
#include <Math/Math.h>

struct TransformComponent {
    glm::vec3 position    = glm::vec3(0.0f);
    glm::vec3 rotation    = glm::vec3(0.0f);   // Euler angles in degrees
    glm::vec3 scale       = glm::vec3(1.0f);
    bool      dirty       = true;              // set when changed, cleared by RenderExtractor

    glm::mat4 worldMatrix = glm::mat4(1.0f);   // computed, not serialized
};
