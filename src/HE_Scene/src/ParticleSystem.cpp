#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
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

void resolveConfigIfNeeded(ParticleSystemComponent& ps, ContentManager& cm)
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
        if (HE::particleGraphFromJson(asset->nodeGraphJson, parsed)) graph = std::move(parsed);
    }

    ps.resolvedConfig      = HE::evaluateParticleGraph(graph, ps.rng);
    ps.resolvedFromAssetId = ps.particleAssetId;
    ps.configDirty         = false;
}
} // namespace

void ParticleSystem::markConfigDirty(ParticleSystemComponent& ps) { ps.configDirty = true; }

bool ParticleSystem::stepPool(std::vector<Particle>& particles, float& emitAccumulator, std::mt19937& rng,
                              const HE::ParticleEmitterConfig& config, const glm::vec3& emitterPos, float dt,
                              const PhysicsWorld* physics)
{
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
    emitAccumulator += dt;
    const float interval = (config.emitRate > 0.0f) ? (1.0f / config.emitRate) : 1e30f;

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distAngle(0.0f, glm::two_pi<float>());

    while (emitAccumulator >= interval &&
           static_cast<int>(particles.size()) < config.maxParticles)
    {
        emitAccumulator -= interval;

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
        const glm::vec3 vel = (dir * std::cos(spread)
                             + t1 * (sinS * std::cos(phi))
                             + t2 * (sinS * std::sin(phi))) * spd;

        Particle p;
        p.position    = emitterPos;
        p.velocity    = vel;
        p.lifetime    = lt;
        p.maxLifetime = lt;
        particles.push_back(p);
    }

    // Clamp accumulator so we don't burst on the first tick after a pause.
    if (emitAccumulator > interval)
        emitAccumulator = interval;

    // "Finished": not looping, nothing left alive, nothing queued to spawn.
    return !config.looping && particles.empty() && emitAccumulator < interval;
}

void ParticleSystem::update(HorizonWorld& world, ContentManager& cm, float dt, const glm::vec3& cameraPos,
                            const PhysicsWorld* physics)
{
    (void)cameraPos; // used by camera-following volume emitters (Phase 2)
    auto& reg = world.registry();

    for (auto [e, tc, ps] : reg.view<TransformComponent, ParticleSystemComponent>().each())
    {
        resolveConfigIfNeeded(ps, cm);
        if (!ps.playing) continue;

        const glm::vec3 emitterPos = glm::vec3(tc.worldMatrix[3]);
        const bool finished = stepPool(ps.particles, ps.emitAccumulator, ps.rng, ps.resolvedConfig, emitterPos, dt, physics);
        if (finished) ps.playing = false;
    }
}
