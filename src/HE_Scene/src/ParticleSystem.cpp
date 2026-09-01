#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/TransformHierarchy.h>   // worldMatrixOf — see update()
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>
#include <ContentManager/Assets.h>
#include <glm/gtc/constants.hpp>
#include <glm/geometric.hpp>
#include <algorithm>
#include <cmath>

namespace
{
// One-off migration for scenes saved before ParticleSystemComponent referenced a
// ParticleGraphAsset: bake the legacy inline values into a real asset (a single
// EmitterOutput fed by Const nodes) so the component has something real to point
// at — same "asset instead of inline fields" move Material made for
// MaterialComponent, just resolved lazily here instead of in SceneSerializer.
HE::UUID migrateLegacyConfig(const ParticleSystemComponent::LegacyConfig& legacy, ContentManager& cm)
{
    HE::ParticleGraph g;
    const int out = g.addNode(HE::ParticleNodeType::EmitterOutput);

    auto wireFloat = [&](int pin, float v) {
        const int c = g.addNode(HE::ParticleNodeType::ConstFloat);
        g.findNode(c)->p[0] = v;
        g.connect(c, 0, out, pin);
    };
    auto wireVec3 = [&](int pin, const glm::vec3& v) {
        const int c = g.addNode(HE::ParticleNodeType::ConstVec3);
        HE::ParticleGraphNode* n = g.findNode(c);
        n->p[0] = v.x; n->p[1] = v.y; n->p[2] = v.z;
        g.connect(c, 0, out, pin);
    };
    wireFloat(0, legacy.emitRate);
    wireFloat(1, legacy.lifetimeMin);
    wireFloat(2, legacy.lifetimeMax);
    wireFloat(3, legacy.startSize);
    wireFloat(4, legacy.endSize);
    wireVec3 (5, legacy.startColor);
    wireVec3 (6, legacy.endColor);
    wireFloat(7, legacy.startAlpha);
    wireFloat(8, legacy.endAlpha);
    wireVec3 (9, legacy.initialVelocity);
    wireFloat(10, legacy.velocitySpread);
    wireVec3 (11, legacy.gravity);
    wireFloat(12, static_cast<float>(legacy.maxParticles));
    wireFloat(13, legacy.looping ? 1.0f : 0.0f);
    g.findNode(out)->meshAssetId     = legacy.meshAssetId;
    g.findNode(out)->materialAssetId = legacy.materialAssetId;

    ParticleGraphAsset asset;
    asset.name          = "Migrated Particle System";
    asset.nodeGraphJson  = HE::particleGraphToJson(g);
    return cm.registerParticleGraph(std::move(asset));
}

} // namespace

void ParticleSystem::resolveConfig(ParticleSystemComponent& ps, ContentManager& cm)
{
    if (ps.legacy.hasData)
    {
        ps.particleAssetId = migrateLegacyConfig(ps.legacy, cm);
        ps.legacy.hasData   = false;
        ps.configDirty      = true;
    }

    if (!ps.configDirty && ps.resolvedFromAssetId == ps.particleAssetId) return;

    HE::ParticleGraph graph = HE::ParticleGraph::makeDefault();
    if (const ParticleGraphAsset* asset = cm.getParticleGraph(ps.particleAssetId);
        asset && !asset->nodeGraphJson.empty())
    {
        HE::ParticleGraph parsed;
        if (HE::particleGraphFromJson(asset->nodeGraphJson, parsed))
            graph = std::move(parsed);
        else
            // Falls back to makeDefault(), so the emitter runs but looks nothing
            // like what was authored — worth an error rather than a shrug.
            HE_LOG_ERROR(Particle, "Particle graph asset '%s' has unparsable JSON — "
                                   "falling back to the default emitter", asset->name.c_str());
    }
    else if (ps.particleAssetId != HE::UUID{})
    {
        HE_LOG_WARN(Particle, "Particle graph asset %016llx%016llx is missing or empty — "
                              "using the default emitter",
                    static_cast<unsigned long long>(ps.particleAssetId.hi),
                    static_cast<unsigned long long>(ps.particleAssetId.lo));
    }

    ps.resolvedConfig      = HE::evaluateParticleGraph(graph, ps.rng);
    ps.resolvedFromAssetId = ps.particleAssetId;
    ps.configDirty         = false;
}

void ParticleSystem::markConfigDirty(ParticleSystemComponent& ps) { ps.configDirty = true; }

namespace
{
// One particle, born at the emitter. Lifted out of the emission loop because a
// burst needs exactly this and nothing else — before, the only way to make a
// particle was to let enough time pass.
Particle spawnParticle(std::mt19937& rng, const HE::ParticleEmitterConfig& config,
                       const glm::vec3& emitterPos)
{
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distAngle(0.0f, glm::two_pi<float>());

    // Random lifetime in [lifetimeMin, lifetimeMax].
    const float lt = config.lifetimeMin +
                     dist01(rng) * (config.lifetimeMax - config.lifetimeMin);

    // Random velocity within a spread cone around initialVelocity.
    // Build an orthonormal frame around the initial velocity direction.
    glm::vec3 dir(config.initialVelocity[0], config.initialVelocity[1], config.initialVelocity[2]);
    const float spd = glm::length(dir);
    if (spd < 1e-5f) dir = glm::vec3(0, 1, 0);
    else              dir /= spd;

    // Random offset within spread cone.
    const float spread  = dist01(rng) * config.velocitySpread;
    const float phi     = distAngle(rng);
    // Tangent frame.
    glm::vec3 t1 = (std::abs(dir.x) < 0.9f)
        ? glm::normalize(glm::cross(dir, glm::vec3(1,0,0)))
        : glm::normalize(glm::cross(dir, glm::vec3(0,1,0)));
    glm::vec3 t2 = glm::cross(dir, t1);
    const float sinS = std::sin(spread);

    Particle p;
    p.position    = emitterPos;
    p.velocity    = (dir * std::cos(spread)
                   + t1 * (sinS * std::cos(phi))
                   + t2 * (sinS * std::sin(phi))) * spd;
    p.lifetime    = lt;
    p.maxLifetime = lt;
    return p;
}
} // namespace

void ParticleSystem::burst(PoolState state, const HE::ParticleEmitterConfig& config,
                           const glm::vec3& emitterPos, int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (static_cast<int>(state.particles.size()) >= config.maxParticles) break;
        state.particles.push_back(spawnParticle(state.rng, config, emitterPos));
        ++state.emitted;
    }
}

bool ParticleSystem::stepPool(PoolState state, const HE::ParticleEmitterConfig& config,
                              const glm::vec3& emitterPos, float dt,
                              const PhysicsWorld* physics)
{
    std::vector<Particle>& particles      = state.particles;
    float&                 emitAccumulator = state.emitAccumulator;
    std::mt19937&          rng             = state.rng;
    // Integrate existing particles.
    const glm::vec3 gravity(config.gravity[0], config.gravity[1], config.gravity[2]);
    const bool collide = physics && config.collisionEnabled;
    for (Particle& p : particles)
    {
        p.lifetime -= dt;
        if (p.lifetime <= 0.0f) continue;
        p.velocity += gravity * dt;

        const glm::vec3 oldPos = p.position;
        glm::vec3       newPos = oldPos + p.velocity * dt;

        if (collide)
        {
            const glm::vec3 delta = newPos - oldPos;
            const float     dist  = glm::length(delta);
            if (dist > 1e-6f)
            {
                const PhysicsWorld::RaycastHit hit = physics->raycast(oldPos, delta, dist);
                if (hit.hit)
                {
                    if (config.killOnCollision) { p.lifetime = 0.0f; continue; }
                    // Reflect the velocity around the surface normal, scaled by
                    // restitution, and snap to the hit point (nudged along the
                    // normal) so the particle doesn't tunnel through next step.
                    p.velocity = glm::reflect(p.velocity, hit.normal) * config.restitution;
                    newPos     = hit.point + hit.normal * 0.001f;
                }
            }
        }
        p.position = newPos;
    }

    // Remove dead particles (swap with back for O(1) removal).
    particles.erase(
        std::remove_if(particles.begin(), particles.end(),
                       [](const Particle& p) { return p.lifetime <= 0.0f; }),
        particles.end());

    // Emit new particles.
    //
    // Two things decide whether this emitter is still allowed to emit at all. A
    // stopped one is not, whatever its graph says. And a NON-LOOPING one is a
    // burst of config.maxParticles, spread over time by the emit rate — once it
    // has produced that many it is done, however many are still in the air.
    //
    // Both were missing. `looping` was read in exactly one place, the "finished"
    // test below, and that test used to ask whether the accumulator had reached
    // the interval — so a one-shot whose interval was longer than a frame (an
    // emit rate under 60, which is most of them) declared itself finished on its
    // FIRST step, before a single particle existed, and one whose interval was
    // shorter never finished at all because it kept refilling its own pool. Both
    // directions wrong, and the test covering it passed on the first of them.
    emitAccumulator += dt;
    const float interval = (config.emitRate > 0.0f) ? (1.0f / config.emitRate) : 1e30f;
    // Asked again after the loop, not captured before it: the loop is what makes
    // the answer change, and a stale copy would report "still emitting" forever.
    const auto mayEmit = [&] {
        return !state.stopping &&
               (config.looping || state.emitted < config.maxParticles);
    };

    while (mayEmit() && emitAccumulator >= interval &&
           static_cast<int>(particles.size()) < config.maxParticles)
    {
        emitAccumulator -= interval;
        particles.push_back(spawnParticle(rng, config, emitterPos));
        ++state.emitted;
    }

    // Clamp accumulator so we don't burst on the first tick after a pause.
    if (emitAccumulator > interval)
        emitAccumulator = interval;

    // "Finished": nothing more will come, and nothing that came is still alive.
    // Note it is the pool being empty that ends it, not the emission — a one-shot
    // stays playing while its last particles fade out, which is what keeps the
    // entity around long enough to see them.
    return !mayEmit() && particles.empty();
}

void ParticleSystem::update(HorizonWorld& world, ContentManager& cm, float dt,
                            const PhysicsWorld* physics)
{
    auto& reg = world.registry();

    // Entities whose one-shot ended and asked to be taken with it. Collected and
    // destroyed AFTER the view — destroying inside an entt view iteration
    // invalidates it, and a spawned hit effect is exactly the case that makes
    // this happen every few frames rather than never.
    std::vector<Entity> finishedAndDone;

    for (auto [e, tc, ps] : reg.view<TransformComponent, ParticleSystemComponent>().each())
    {
        resolveConfig(ps, cm);
        if (!ps.playing) continue;

        // Composed from the hierarchy, NOT read out of tc.worldMatrix. The
        // matrix is only refreshed by the transform propagation that runs later
        // in the frame, so on the first step of a JUST-SPAWNED effect it is still
        // identity — and every impact puff would put its first particles at the
        // world origin before jumping to the impact point on frame two. The
        // effect spawned at the moment of the hit is precisely the case B6
        // exists for, so it is precisely the one that must not do that.
        const glm::vec3 emitterPos = glm::vec3(HE::worldMatrixOf(world, e)[3]);
        const size_t before = ps.particles.size();
        const bool finished = stepPool({ ps.particles, ps.emitAccumulator, ps.emitted,
                                         ps.rng, ps.stopping },
                                       ps.resolvedConfig, emitterPos, dt, physics);
        if (finished)
        {
            HE_LOG_TRACE(Particle, "Entity %u: one-shot emitter finished",
                         static_cast<uint32_t>(e));
            ps.playing = false;
            if (ps.destroyWhenFinished) finishedAndDone.push_back(e);
        }
        // Hitting maxParticles every frame means the emit rate outruns the pool
        // and particles die early — visible as a flickering, truncated effect.
        // Only for a LOOPING emitter: a one-shot's maxParticles is the size of
        // its burst, so filling the pool is it working, not it drowning.
        else if (ps.resolvedConfig.looping &&
                 before >= static_cast<size_t>(ps.resolvedConfig.maxParticles) &&
                 ps.particles.size() >= static_cast<size_t>(ps.resolvedConfig.maxParticles))
        {
            HE_LOG_THROTTLE(Particle, Warning, 10.0,
                            "Entity %u: particle pool saturated at maxParticles=%d — "
                            "raise the cap or lower the emit rate",
                            static_cast<uint32_t>(e), ps.resolvedConfig.maxParticles);
        }
    }

    for (const Entity e : finishedAndDone)
        world.destroyEntity(e);
}
