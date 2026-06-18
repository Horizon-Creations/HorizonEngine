#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/DefaultAssets.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

TEST_CASE("ParticleSystemComponent default values")
{
    ParticleSystemComponent ps;
    CHECK(ps.emitRate     == doctest::Approx(10.0f));
    CHECK(ps.lifetimeMin  == doctest::Approx(1.0f));
    CHECK(ps.lifetimeMax  == doctest::Approx(2.0f));
    CHECK(ps.startSize    == doctest::Approx(0.3f));
    CHECK(ps.endSize      == doctest::Approx(0.0f));
    CHECK(ps.maxParticles == 100);
    CHECK(ps.playing);
    CHECK(ps.looping);
    CHECK(ps.particles.empty());
}

TEST_CASE("ParticleSystem::update emits particles")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {0,0,0};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.emitRate     = 10.0f;
    ps.lifetimeMin  = 5.0f;
    ps.lifetimeMax  = 5.0f;
    ps.maxParticles = 50;
    reg.emplace<ParticleSystemComponent>(e, ps);

    // Advance 0.5 s → should emit ~5 particles.
    ParticleSystem::update(world, 0.5f);
    const auto& psc = reg.get<ParticleSystemComponent>(e);
    CHECK(psc.particles.size() >= 4);
    CHECK(psc.particles.size() <= 6);
}

TEST_CASE("ParticleSystem::update advances positions")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {0,0,0};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.emitRate        = 100.0f;
    ps.lifetimeMin     = 10.0f;
    ps.lifetimeMax     = 10.0f;
    ps.initialVelocity = {0, 1, 0};
    ps.velocitySpread  = 0.0f;  // no spread → exact direction
    ps.gravity         = {0, 0, 0};
    ps.maxParticles    = 10;
    reg.emplace<ParticleSystemComponent>(e, ps);

    // Emit a particle then advance 1 s.
    ParticleSystem::update(world, 0.01f);
    ParticleSystem::update(world, 1.0f);

    const auto& psc = reg.get<ParticleSystemComponent>(e);
    CHECK(!psc.particles.empty());
    bool anyUp = false;
    for (const auto& p : psc.particles)
        if (p.position.y > 0.5f) anyUp = true;
    CHECK(anyUp);
}

TEST_CASE("ParticleSystem::update respects maxParticles")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.emitRate     = 10000.0f;
    ps.lifetimeMin  = 100.0f;
    ps.lifetimeMax  = 100.0f;
    ps.maxParticles = 20;
    reg.emplace<ParticleSystemComponent>(e, ps);

    ParticleSystem::update(world, 1.0f);
    const auto& psc = reg.get<ParticleSystemComponent>(e);
    CHECK(static_cast<int>(psc.particles.size()) <= 20);
}

TEST_CASE("ParticleSystem::update removes dead particles")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.emitRate     = 50.0f;
    ps.lifetimeMin  = 0.1f;
    ps.lifetimeMax  = 0.1f;
    ps.maxParticles = 50;
    reg.emplace<ParticleSystemComponent>(e, ps);

    // Emit for 0.05 s.
    ParticleSystem::update(world, 0.05f);
    CHECK(reg.get<ParticleSystemComponent>(e).particles.size() > 0);

    // Stop emitting so the next update only integrates + removes, no new births.
    reg.get<ParticleSystemComponent>(e).emitRate = 0.0f;

    // Advance 0.2 s (> lifetime 0.1) → all existing particles die.
    ParticleSystem::update(world, 0.2f);
    CHECK(reg.get<ParticleSystemComponent>(e).particles.empty());
}

TEST_CASE("ParticleSystem::update stops when not looping and exhausted")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.emitRate     = 1.0f;
    ps.lifetimeMin  = 0.05f;
    ps.lifetimeMax  = 0.05f;
    ps.maxParticles = 5;
    ps.looping      = false;
    reg.emplace<ParticleSystemComponent>(e, ps);

    for (int i = 0; i < 20; ++i)
        ParticleSystem::update(world, 0.1f);

    CHECK(!reg.get<ParticleSystemComponent>(e).playing);
}

TEST_CASE("ParticleSystem::update paused when playing=false")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.emitRate     = 100.0f;
    ps.playing      = false;
    ps.maxParticles = 50;
    reg.emplace<ParticleSystemComponent>(e, ps);

    ParticleSystem::update(world, 1.0f);
    CHECK(reg.get<ParticleSystemComponent>(e).particles.empty());
}

TEST_CASE("kDefaultQuadMeshId is registered in ContentManager")
{
    ContentManager cm;
    const StaticMeshAsset* quad = cm.getStaticMesh(HE::kDefaultQuadMeshId);
    REQUIRE(quad != nullptr);
    CHECK(quad->vertices.size() == 12);  // 4 verts × 3 floats
    CHECK(quad->indices.size()  == 6);   // 2 triangles
    CHECK(quad->normals.size()  == 12);
    CHECK(quad->uvs.size()      == 8);   // 4 verts × 2 floats
}
