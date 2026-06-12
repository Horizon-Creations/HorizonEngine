#pragma once
#include "RenderObject.h"
#include <Math/Math.h>
#include <vector>
#include <cstdint>

struct CameraData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 position;
};

struct LightData {
    glm::vec3 position;
    glm::vec3 color;
    float     intensity;
    uint8_t   type;
};

class RenderWorld {
public:
    void clear();

    std::vector<RenderObject> objects;
    std::vector<LightData>    lights;
    CameraData                camera;
};
