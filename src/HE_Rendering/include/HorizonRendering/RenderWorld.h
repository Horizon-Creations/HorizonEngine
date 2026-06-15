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

// Directional-light shadow info, computed by the extractor. viewProj transforms
// world space into the light's clip space (orthographic) for shadow mapping.
struct ShadowData {
    glm::mat4 viewProj   = glm::mat4(1.0f);
    glm::vec3 direction  = glm::vec3(0.0f, -1.0f, 0.0f);
    bool      enabled    = false; // true when a directional light is present
};

class RenderWorld {
public:
    void clear();

    std::vector<RenderObject> objects;
    std::vector<LightData>    lights;
    CameraData                camera;
    ShadowData                shadow;

    // Direction *toward* the sun (normalized), set by the extractor: from the
    // first directional light, or driven by the day-night cycle when enabled.
    // Backends use it for the procedural sky + image-based ambient.
    glm::vec3 sunDirection = glm::vec3(0.45f, 0.80f, 0.55f);

    // Flat ambient fill added to every lit surface, set by the extractor. A weak
    // floor is always present (so the scene is never fully black); under heavy
    // cloud cover it grows to replace the switched-off sun/moon directional light.
    glm::vec3 ambient = glm::vec3(0.03f, 0.035f, 0.05f);
};
