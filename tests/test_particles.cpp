#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/DefaultAssets.h>
#include <ParticleGraph/ParticleGraph.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/TransformComponent.h>

// Builds a ParticleGraphAsset wiring Const nodes onto every Emitter Output pin —
// the graph-authored equivalent of directly setting the old inline component
// fields — and registers it, returning the UUID a ParticleSystemComponent can
// reference. Pin order matches HE::particleNodeDesc(EmitterOutput).inputs.
static HE::UUID makeConfiguredParticleAsset(
    ContentManager& cm,
    float emitRate, float lifetimeMin, float lifetimeMax, int maxParticles,
    glm::vec3 initialVelocity = {0.0f, 2.0f, 0.0f}, float velocitySpread = 0.5f,
    glm::vec3 gravity = {0.0f, -2.0f, 0.0f}, bool looping = true)
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
    wireFloat(0, emitRate);
    wireFloat(1, lifetimeMin);
    wireFloat(2, lifetimeMax);
    wireVec3 (9, initialVelocity);
    wireFloat(10, velocitySpread);
    wireVec3 (11, gravity);
    wireFloat(12, static_cast<float>(maxParticles));
    wireFloat(13, looping ? 1.0f : 0.0f);

    ParticleGraphAsset asset;
    asset.nodeGraphJson = HE::particleGraphToJson(g);
    return cm.registerParticleGraph(std::move(asset));
}

TEST_CASE("ParticleSystemComponent default values")
{
    ParticleSystemComponent ps;
    CHECK(ps.playing);
    CHECK(ps.particleAssetId == HE::UUID{});
    CHECK(ps.particles.empty());
    CHECK(ps.configDirty); // resolves to HE::ParticleGraph::makeDefault() on first update()
}

TEST_CASE("HE::ParticleGraph::makeDefault matches the old ParticleSystemComponent defaults")
{
    ContentManager cm;
    HE::ParticleGraph g = HE::ParticleGraph::makeDefault();
    std::mt19937 rng{ 1 };
    const HE::ParticleEmitterConfig cfg = HE::evaluateParticleGraph(g, rng);
    CHECK(cfg.emitRate     == doctest::Approx(10.0f));
    CHECK(cfg.lifetimeMin  == doctest::Approx(1.0f));
    CHECK(cfg.lifetimeMax  == doctest::Approx(2.0f));
    CHECK(cfg.startSize    == doctest::Approx(0.3f));
    CHECK(cfg.endSize      == doctest::Approx(0.0f));
    CHECK(cfg.maxParticles == 100);
    CHECK(cfg.looping);
    (void)cm;
}

TEST_CASE("ParticleSystem::update emits particles")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {0,0,0};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/10.0f, /*ltMin*/5.0f, /*ltMax*/5.0f, /*maxP*/50);
    reg.emplace<ParticleSystemComponent>(e, ps);

    // Advance 0.5 s → should emit ~5 particles.
    ParticleSystem::update(world, cm, 0.5f);
    const auto& psc = reg.get<ParticleSystemComponent>(e);
    CHECK(psc.particles.size() >= 4);
    CHECK(psc.particles.size() <= 6);
}

TEST_CASE("ParticleSystem::update advances positions")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {0,0,0};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/100.0f, /*ltMin*/10.0f, /*ltMax*/10.0f,
        /*maxP*/10, /*vel*/{0,1,0}, /*spread*/0.0f, /*gravity*/{0,0,0});
    reg.emplace<ParticleSystemComponent>(e, ps);

    // Emit a particle then advance 1 s.
    ParticleSystem::update(world, cm, 0.01f);
    ParticleSystem::update(world, cm, 1.0f);

    const auto& psc = reg.get<ParticleSystemComponent>(e);
    CHECK(!psc.particles.empty());
    bool anyUp = false;
    for (const auto& p : psc.particles)
        if (p.position.y > 0.5f) anyUp = true;
    CHECK(anyUp);
}

TEST_CASE("ParticleSystem::update respects maxParticles")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/10000.0f, /*ltMin*/100.0f, /*ltMax*/100.0f, /*maxP*/20);
    reg.emplace<ParticleSystemComponent>(e, ps);

    ParticleSystem::update(world, cm, 1.0f);
    const auto& psc = reg.get<ParticleSystemComponent>(e);
    CHECK(static_cast<int>(psc.particles.size()) <= 20);
}

TEST_CASE("ParticleSystem::update removes dead particles")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/50.0f, /*ltMin*/0.1f, /*ltMax*/0.1f, /*maxP*/50);
    reg.emplace<ParticleSystemComponent>(e, ps);

    // Emit for 0.05 s.
    ParticleSystem::update(world, cm, 0.05f);
    CHECK(reg.get<ParticleSystemComponent>(e).particles.size() > 0);

    // Stop emitting so the next update only integrates + removes, no new births.
    // resolvedConfig is a plain cache struct (recomputed from the graph only when
    // particleAssetId/configDirty change) — poking it directly here is the
    // equivalent of the old test's `ps.emitRate = 0`, without re-registering a
    // second asset just to silence the emitter for one step.
    reg.get<ParticleSystemComponent>(e).resolvedConfig.emitRate = 0.0f;

    // Advance 0.2 s (> lifetime 0.1) → all existing particles die, none replace them.
    ParticleSystem::update(world, cm, 0.2f);
    CHECK(reg.get<ParticleSystemComponent>(e).particles.empty());
}

TEST_CASE("ParticleSystem::update stops when not looping and exhausted")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/1.0f, /*ltMin*/0.05f, /*ltMax*/0.05f,
        /*maxP*/5, /*vel*/{0,2,0}, /*spread*/0.5f, /*gravity*/{0,-2,0}, /*looping*/false);
    reg.emplace<ParticleSystemComponent>(e, ps);

    for (int i = 0; i < 20; ++i)
        ParticleSystem::update(world, cm, 0.1f);

    CHECK(!reg.get<ParticleSystemComponent>(e).playing);
}

TEST_CASE("ParticleSystem::update paused when playing=false")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/100.0f, /*ltMin*/1.0f, /*ltMax*/2.0f, /*maxP*/50);
    ps.playing = false;
    reg.emplace<ParticleSystemComponent>(e, ps);

    ParticleSystem::update(world, cm, 1.0f);
    CHECK(reg.get<ParticleSystemComponent>(e).particles.empty());
}

TEST_CASE("ParticleSystem::update migrates a legacy inline-config entity into a real asset")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    // Simulate what SceneSerializer::load does for an old-format scene: no
    // particleAssetId, legacy config staged instead.
    ParticleSystemComponent ps;
    ps.legacy.hasData     = true;
    ps.legacy.emitRate    = 20.0f;
    ps.legacy.lifetimeMin = 3.0f;
    ps.legacy.lifetimeMax = 3.0f;
    ps.legacy.maxParticles = 40;
    reg.emplace<ParticleSystemComponent>(e, ps);

    ParticleSystem::update(world, cm, 0.5f);

    const auto& psc = reg.get<ParticleSystemComponent>(e);
    CHECK(!psc.legacy.hasData);                  // migrated exactly once
    CHECK(psc.particleAssetId != HE::UUID{});     // now points at a real asset
    CHECK(cm.getParticleGraph(psc.particleAssetId) != nullptr);
    // ~10 particles expected over 0.5s at emitRate=20.
    CHECK(psc.particles.size() >= 8);
    CHECK(psc.particles.size() <= 12);
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
