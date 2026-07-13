#pragma once
#include "RenderObject.h"
#include <Renderer/UIRenderObject.h>
#include <ParticleGraph/ParticleGraph.h>
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
    uint8_t   envRole      = 0;       // 0 none, 1 = environment sun, 2 = environment moon
};

// Directional-light shadow info, computed by the extractor.
//   viewProj   — single whole-scene light clip transform. Used by the backends that
//                are still on a single shadow map (OpenGL / D3D / Vulkan).
//   cascade*   — Cascaded Shadow Maps: `cascadeCount` tight light frusta fit to
//                successive camera-distance slices (sharp near, coarse far), used by
//                the Metal backend. cascadeSplit[i] = the cascade's far distance in
//                view space (camera-forward metres) for per-fragment cascade pick.
struct ShadowData {
    glm::mat4 viewProj   = glm::mat4(1.0f);
    glm::vec3 direction  = glm::vec3(0.0f, -1.0f, 0.0f);
    bool      enabled    = false; // true when a directional light is present

    static constexpr int kMaxCascades = 4;
    int       cascadeCount = 0;
    glm::mat4 cascadeViewProj[kMaxCascades] = { glm::mat4(1.0f), glm::mat4(1.0f),
                                                glm::mat4(1.0f), glm::mat4(1.0f) };
    float     cascadeSplit[kMaxCascades]    = { 0.0f, 0.0f, 0.0f, 0.0f };
};

// One live particle's raw GPU-instanced draw data — position/size (still CPU-lerped,
// a single scalar isn't worth moving to the GPU) plus t01 (0=born..1=dead), which the
// backend's baked/on-demand-compiled ParticleGraph shader turns into color+alpha via
// HE::generateParticleShaderSource's heParticleColor/heParticleAlpha (see
// HorizonRendering::ParticleShaderTemplates). Replaces the old one-RenderObject-per-
// particle path (RenderObject::instanceTint) for camera-billboard ParticleSystemComponent
// particles specifically — that field/path stays valid for everything else (weather
// precipitation billboards, foliage, future per-instance-varying-appearance needs).
struct ParticleInstance {
    glm::vec3 position;
    float     size;
    float     t01;
};

// One ParticleSystemComponent's live particles, batched for ONE GPU-instanced draw
// call. `config` carries the resolved color/alpha-over-life endpoints the backend
// hashes/bakes a shader from (see ContentManager::getParticleGraph's resolved
// config) — NOT re-evaluated from the graph, so it always matches what the CPU
// simulation (ParticleSystem::stepPool) already committed to.
struct ParticleBatch {
    HE::UUID                      meshAssetId;     // usually the default quad
    HE::UUID                      materialAssetId; // base texture (heTex0), optional
    HE::ParticleEmitterConfig     config;
    uint32_t                      entityId = 0;
    std::vector<ParticleInstance> instances;
};

class RenderWorld {
public:
    void clear();

    std::vector<RenderObject>        objects;
    std::vector<SkinnedRenderObject> skinnedObjects;
    std::vector<LightData>           lights;
    std::vector<UIRenderObject>      uiObjects;
    std::vector<ParticleBatch>       particleBatches;
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
