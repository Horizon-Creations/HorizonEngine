#pragma once
#include <ParticleGraph/ParticleGraph.h>
#include <glm/glm.hpp>
#include <random>
#include <vector>
class HorizonWorld;
class ContentManager;
class PhysicsWorld;
struct ParticleSystemComponent;
struct Particle;

namespace ParticleSystem {
    // Advance all ParticleSystemComponents by dt seconds.
    // Emits new particles, integrates velocities, removes dead particles. Resolves
    // each component's referenced ParticleGraphAsset into resolvedConfig on first
    // use / whenever particleAssetId or configDirty changes (not every frame — see
    // ParticleSystemComponent's comment on why). `physics` enables per-particle
    // collision (config.collisionEnabled) — nullptr (no physics running, e.g. in a
    // preview) behaves exactly like collisionEnabled=false.
    // Emitters are entity-bound only — the camera-following precipitation volume
    // lives in WeatherSystem, which simulates its own pool and never comes through
    // here, so this needs no camera position.
    void update(HorizonWorld& world, ContentManager& cm, float dt,
               const PhysicsWorld* physics = nullptr);

    // Resolve this component's referenced graph into resolvedConfig if it is not
    // already (a no-op otherwise, and it also runs the legacy inline-config
    // migration). update() calls it per emitter; it is public because anything
    // acting on an emitter BEFORE its first update — a Burst from a Begin Play,
    // say — would otherwise read a default-constructed config and produce an
    // effect nobody authored.
    void resolveConfig(ParticleSystemComponent& ps, ContentManager& cm);

    // Force a re-resolve on the next update() — call after editing the graph of the
    // ParticleGraphAsset this component references (e.g. from the Particle Graph
    // Editor's live preview, or after any runtime particleAssetId re-point).
    void markConfigDirty(ParticleSystemComponent& ps);

    // The mutable half of one emitter, gathered so stepPool keeps a readable
    // signature as the state grows (it grew three members for one-shots alone).
    // References, not copies: both callers own their state elsewhere — the ECS
    // component and the Particle Graph Editor's preview — and both must see the
    // step's writes.
    struct PoolState
    {
        std::vector<Particle>& particles;
        float&                 emitAccumulator;
        int&                   emitted;    // this run's total, the one-shot's stop condition
        std::mt19937&          rng;
        bool                   stopping = false;  // emit no more, let the live ones finish
    };

    // The actual per-pool emit/integrate/cull step, extracted so both the ECS loop
    // (update(), above) and standalone previews (e.g. the Particle Graph Editor's
    // live preview, which has no entity/TransformComponent to hang a component off
    // of) share one simulation instead of two copies drifting apart.
    //
    // Returns true when the emitter is FINISHED: it will emit nothing more and
    // nothing it emitted is still alive. That is `stopping`, or a non-looping
    // emitter that has emitted its config.maxParticles — plus an empty pool in
    // both cases. update() uses it to clear ParticleSystemComponent::playing (and
    // to take the entity with it, if it asked for that); standalone callers may
    // ignore it.
    //
    // `physics` (optional): when config.collisionEnabled, each particle raycasts
    // along its motion this step (PhysicsWorld::raycast, Forts. 41 — no new
    // physics code) and either dies (config.killOnCollision) or bounces, scaled
    // by config.restitution. Left null, particles never collide (same behavior
    // as before this parameter existed).
    bool stepPool(PoolState state, const HE::ParticleEmitterConfig& config,
                 const glm::vec3& emitterPos, float dt,
                 const PhysicsWorld* physics = nullptr);

    // `count` particles at once, right now, ignoring the emit rate entirely.
    // A muzzle flash, an impact, a footstep puff: those are N at a moment, and
    // the rate-driven loop cannot express a moment — asking it to would mean an
    // emitRate of N/dt, which depends on the frame rate.
    //
    // Counts towards the run's emitted total, so a burst on a one-shot brings its
    // end nearer rather than extending it, and is clamped by config.maxParticles
    // like any other emission.
    void burst(PoolState state, const HE::ParticleEmitterConfig& config,
              const glm::vec3& emitterPos, int count);
}
