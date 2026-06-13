#pragma once
#include "RenderObject.h"
#include <Math/Math.h>
#include <vector>
#include <cstdint>

struct CameraData {
    glm::mat4 view       = glm::mat4(1.0f);
    glm::mat4 projection = glm::mat4(1.0f);
    glm::vec3 position   = glm::vec3(0.0f);
};

struct LightData {
    glm::vec3 position     = glm::vec3(0.0f);
    glm::vec3 direction    = glm::vec3(0.0f, -1.0f, 0.0f); // directional/spot: -Z of the light's world matrix
    glm::vec3 color        = glm::vec3(1.0f);
    float     intensity    = 1.0f;
    float     range        = 10.0f;   // point/spot attenuation radius
    float     spotAngleCos = 0.0f;    // cos(half angle), spot only
    uint8_t   type         = 0;       // HE::LightType
};

class RenderWorld {
public:
    void clear();

    std::vector<RenderObject> objects;
    std::vector<LightData>    lights;
    CameraData                camera;
};
