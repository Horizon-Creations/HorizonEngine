#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

void ParticleSystem::update(HorizonWorld& world, float dt)
{
    auto& reg = world.registry();

    for (auto [e, tc, ps] : reg.view<TransformComponent, ParticleSystemComponent>().each())
    {
        if (!ps.playing) continue;

        // Integrate existing particles.
        for (Particle& p : ps.particles)
        {
            p.lifetime -= dt;
            if (p.lifetime <= 0.0f) continue;
            p.velocity  += ps.gravity * dt;
            p.position  += p.velocity * dt;
        }

        // Remove dead particles (swap with back for O(1) removal).
        ps.particles.erase(
            std::remove_if(ps.particles.begin(), ps.particles.end(),
                           [](const Particle& p) { return p.lifetime <= 0.0f; }),
            ps.particles.end());

        // Emit new particles.
        ps.emitAccumulator += dt;
        const float interval = (ps.emitRate > 0.0f) ? (1.0f / ps.emitRate) : 1e30f;
        const glm::vec3 emitterPos = glm::vec3(tc.worldMatrix[3]);

        std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
        std::uniform_real_distribution<float> distAngle(0.0f, glm::two_pi<float>());

        while (ps.emitAccumulator >= interval &&
               static_cast<int>(ps.particles.size()) < ps.maxParticles)
        {
            ps.emitAccumulator -= interval;

            // Random lifetime in [lifetimeMin, lifetimeMax].
            const float lt = ps.lifetimeMin +
                             dist01(ps.rng) * (ps.lifetimeMax - ps.lifetimeMin);

            // Random velocity within a spread cone around initialVelocity.
            // Build an orthonormal frame around the initial velocity direction.
            glm::vec3 dir = ps.initialVelocity;
            const float spd = glm::length(dir);
            if (spd < 1e-5f) dir = glm::vec3(0, 1, 0);
            else              dir /= spd;

            // Random offset within spread cone.
            const float spread  = dist01(ps.rng) * ps.velocitySpread;
            const float phi     = distAngle(ps.rng);
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
            ps.particles.push_back(p);
        }

        // Clamp accumulator so we don't burst on the first tick after a pause.
        if (ps.emitAccumulator > interval)
            ps.emitAccumulator = interval;

        // When not looping and no particles remain and accumulator exhausted, stop.
        if (!ps.looping && ps.particles.empty() && ps.emitAccumulator < interval)
            ps.playing = false;
    }
}
