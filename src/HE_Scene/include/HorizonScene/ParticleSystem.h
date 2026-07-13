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
    // cameraPos positions camera-following volume emitters (e.g. precipitation).
    void update(HorizonWorld& world, ContentManager& cm, float dt, const glm::vec3& cameraPos = glm::vec3(0.0f),
               const PhysicsWorld* physics = nullptr);

    // Force a re-resolve on the next update() — call after editing the graph of the
    // ParticleGraphAsset this component references (e.g. from the Particle Graph
    // Editor's live preview, or after any runtime particleAssetId re-point).
    void markConfigDirty(ParticleSystemComponent& ps);

    // The actual per-pool emit/integrate/cull step, extracted so both the ECS loop
    // (update(), above) and standalone previews (e.g. the Particle Graph Editor's
    // live preview, which has no entity/TransformComponent to hang a component off
    // of) share one simulation instead of two copies drifting apart. Returns true
    // if the emitter should now be considered "finished" (config.looping == false,
    // no particles left, accumulator exhausted) — update() uses this to flip
    // ParticleSystemComponent::playing; standalone callers may ignore it.
    // `physics` (optional): when config.collisionEnabled, each particle raycasts
    // along its motion this step (PhysicsWorld::raycast, Forts. 41 — no new
    // physics code) and either dies (config.killOnCollision) or bounces, scaled
    // by config.restitution. Left null, particles never collide (same behavior
    // as before this parameter existed).
    bool stepPool(std::vector<Particle>& particles, float& emitAccumulator, std::mt19937& rng,
                 const HE::ParticleEmitterConfig& config, const glm::vec3& emitterPos, float dt,
                 const PhysicsWorld* physics = nullptr);
}
