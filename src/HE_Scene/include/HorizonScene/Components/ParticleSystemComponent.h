#pragma once
#include <Types/UUID.h>
#include <Math/Math.h>
#include <ParticleGraph/ParticleGraph.h>
#include <vector>
#include <random>

struct Particle {
    glm::vec3 position  = {};
    glm::vec3 velocity  = {};
    float     lifetime    = 0.0f;  // remaining seconds
    float     maxLifetime = 1.0f;  // initial lifetime for interpolation
};

struct ParticleSystemComponent {
    bool     visible = true;    // extractor skips invisible (zone hiding)
    bool     playing = true;
    HE::UUID particleAssetId;   // references a ParticleGraphAsset authored in the
                                 // Particle Graph Editor — {} plays HE::ParticleGraph's
                                 // defaults (same values the old inline fields used).

    // ── Runtime state (never serialized) ────────────────────────────────────────
    float                 emitAccumulator = 0.0f;
    std::vector<Particle> particles;
    std::mt19937          rng { 42 };

    // Resolved-config cache — ParticleSystem::update (re)computes this whenever
    // particleAssetId changes or configDirty is set, NOT every frame: RandomRange
    // nodes in the graph would otherwise re-roll every frame and flicker. Call
    // ParticleSystem::markConfigDirty(component) after editing the referenced
    // asset's graph to force a re-resolve. RenderExtractor also reads from here.
    HE::ParticleEmitterConfig resolvedConfig;
    HE::UUID                  resolvedFromAssetId;
    bool                       configDirty = true;

    // ── Legacy migration staging (SceneSerializer only) ─────────────────────────
    // Old scenes serialized the emitter config INLINE on this component (no asset
    // reference). SceneSerializer::load populates this verbatim when the JSON has
    // no "particleAsset" key — the serializer has no ContentManager dependency and
    // adding one just for this migration isn't worth it. ParticleSystem::update
    // (which already has ContentManager access) converts it into a real
    // ParticleGraphAsset on the first tick and clears hasData so it runs once.
    struct LegacyConfig
    {
        bool      hasData = false;
        HE::UUID  meshAssetId, materialAssetId;
        float     emitRate = 10.0f, lifetimeMin = 1.0f, lifetimeMax = 2.0f;
        float     startSize = 0.3f, endSize = 0.0f;
        glm::vec3 startColor{1.0f, 1.0f, 1.0f}, endColor{1.0f, 1.0f, 1.0f};
        float     startAlpha = 1.0f, endAlpha = 0.0f;
        glm::vec3 initialVelocity{0.0f, 2.0f, 0.0f};
        float     velocitySpread = 0.5f;
        glm::vec3 gravity{0.0f, -2.0f, 0.0f};
        int       maxParticles = 100;
        bool      looping = true;
    } legacy;
};
