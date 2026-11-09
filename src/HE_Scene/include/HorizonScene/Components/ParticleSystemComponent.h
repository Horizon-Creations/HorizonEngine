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

// Per-instance changes on top of the particle asset's own values, applied every time the
// config is resolved (ParticleSystem::resolveConfig): how a system that PLACES emitters
// (the landscape water's spray at a fall, WaterSurface.cpp) tunes each one — how strong,
// how long, which way, how wide — while the look stays the asset's. The graph's own frame
// is "forward = +X, up = +Y, across = +Z"; `heading` turns it to a world direction.
// Identity (the default) leaves the asset's config exactly as it is. Runtime, never saved:
// whoever sets them sets them again.
struct ParticleEmitterModifiers {
    float     rateScale     = 1.0f;   // emit rate (and the pool's room with it)
    float     lifetimeScale = 1.0f;   // lifetime min/max (and the pool's room with it)
    float     sizeScale     = 1.0f;   // start/end size
    float     alphaScale    = 1.0f;   // start/end alpha (clamped to 1)
    float     spreadScale   = 1.0f;   // velocity spread (radians)
    glm::vec3 velocityScale { 1.0f }; // initial velocity per graph axis (forward, up, across)
    glm::vec2 spawnScale    { 1.0f }; // spawn area: x scales the across (Z) parts, y the forward (X) parts
    glm::vec2 heading       { 0.0f }; // world XZ the graph's +X turns to; zero = the graph's own axes
    bool operator==(const ParticleEmitterModifiers&) const = default;
};
HE::ParticleEmitterConfig applyEmitterModifiers(HE::ParticleEmitterConfig c, const ParticleEmitterModifiers& m);

struct ParticleSystemComponent {
    bool     visible = true;    // extractor skips invisible (zone hiding)
    bool     playing = true;
    // A one-shot that takes its entity with it when the last particle dies.
    // What makes a hit effect spawnable: the graph creates the class at the
    // impact point and then forgets about it, instead of having to guess how
    // long to wait before destroying it by hand. Ignored while looping — an
    // emitter that never finishes would never be cleaned up either.
    bool     destroyWhenFinished = false;
    HE::UUID particleAssetId;   // references a ParticleGraphAsset authored in the
                                 // Particle Graph Editor — {} plays HE::ParticleGraph's
                                 // defaults (same values the old inline fields used).
    // Runtime (never serialized): see ParticleEmitterModifiers. Set it, then markConfigDirty.
    ParticleEmitterModifiers modifiers;

    // ── Runtime state (never serialized) ────────────────────────────────────────
    float                 emitAccumulator = 0.0f;
    // How many this run has emitted. A one-shot's whole stopping condition:
    // `maxParticles` is both the pool cap and, for a non-looping emitter, the
    // size of the burst — so a puff of thirty is thirty, however fast they leave
    // and however quickly they die. Counting live particles instead cannot say
    // it: with a short lifetime the pool never fills, and the emitter would run
    // forever.
    int                   emitted         = 0;
    // Soft stop: emit nothing more, but keep the ones already out alive until
    // they die on their own. What "stop the smoke" means — the alternative,
    // clearing playing, freezes a cloud in mid-air.
    bool                  stopping        = false;
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
