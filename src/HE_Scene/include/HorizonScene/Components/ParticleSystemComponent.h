#pragma once
#include <Types/UUID.h>
#include <Math/Math.h>
#include <vector>
#include <random>

struct Particle {
    glm::vec3 position  = {};
    glm::vec3 velocity  = {};
    float     lifetime    = 0.0f;  // remaining seconds
    float     maxLifetime = 1.0f;  // initial lifetime for interpolation
};

struct ParticleSystemComponent {
    bool      visible = true;   // extractor skips invisible (zone hiding)
    // ── Emitter config ──────────────────────────────────────────────────────────
    HE::UUID  meshAssetId;              // kDefaultQuadMeshId when null
    HE::UUID  materialAssetId;
    float     emitRate        = 10.0f;  // particles emitted per second
    float     lifetimeMin     = 1.0f;
    float     lifetimeMax     = 2.0f;
    float     startSize       = 0.3f;
    float     endSize         = 0.0f;   // shrink to zero by end of life
    glm::vec3 startColor      = {1.0f, 1.0f, 1.0f};
    glm::vec3 endColor        = {1.0f, 1.0f, 1.0f};
    float     startAlpha      = 1.0f;
    float     endAlpha        = 0.0f;
    glm::vec3 initialVelocity = {0.0f, 2.0f, 0.0f};
    float     velocitySpread  = 0.5f;  // random cone spread in radians
    glm::vec3 gravity         = {0.0f, -2.0f, 0.0f};
    int       maxParticles    = 100;
    bool      playing         = true;
    bool      looping         = true;

    // ── Runtime state (never serialized) ────────────────────────────────────────
    float                 emitAccumulator = 0.0f;
    std::vector<Particle> particles;
    std::mt19937          rng { 42 };
};
