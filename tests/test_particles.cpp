#include "doctest.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <ContentManager/DefaultAssets.h>
#include <ParticleGraph/ParticleGraph.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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

// This test used to pass BECAUSE the emitter was broken. It asserted only
// `!playing`, and a non-looping emitter used to declare itself finished on its
// very first step — before a single particle existed — whenever its emit
// interval was longer than a frame, which at emitRate 1 it was by a factor of
// ten. Zero particles, test green. So it now asserts the half that was missing:
// that something came out at all, and that maxParticles is how many.
//
// MUTATION: in ParticleSystem::stepPool, drop `state.emitted < config.maxParticles`
// from mayEmit() — the one-shot then runs forever and never finishes.
TEST_CASE("ParticleSystem::update: a one-shot emits its burst and then finishes")
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
    // One per 0.1 s, five of them, each alive half a second.
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/10.0f, /*ltMin*/0.5f, /*ltMax*/0.5f,
        /*maxP*/5, /*vel*/{0,2,0}, /*spread*/0.5f, /*gravity*/{0,-2,0}, /*looping*/false);
    reg.emplace<ParticleSystemComponent>(e, ps);

    // Half a second in: emitting, and visibly so.
    for (int i = 0; i < 5; ++i) ParticleSystem::update(world, cm, 0.1f);
    CHECK(reg.get<ParticleSystemComponent>(e).particles.size() >= 4);
    CHECK(reg.get<ParticleSystemComponent>(e).emitted == 5);
    CHECK(reg.get<ParticleSystemComponent>(e).playing);   // its last particles are still in the air

    // Long enough for the burst to be spent and the last particle to die.
    for (int i = 0; i < 20; ++i) ParticleSystem::update(world, cm, 0.1f);
    CHECK(!reg.get<ParticleSystemComponent>(e).playing);
    CHECK(reg.get<ParticleSystemComponent>(e).particles.empty());
    // The count did not keep climbing after the burst was spent.
    CHECK(reg.get<ParticleSystemComponent>(e).emitted == 5);
}

// The other direction of the same old bug: an emit interval SHORTER than a frame
// used to mean the pool never emptied, so the emitter never finished and ran for
// the rest of the session.
//
// MUTATION: same as above — without the emitted cap this never stops.
TEST_CASE("ParticleSystem::update: a one-shot faster than the frame rate still ends")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    // 1000 per second at 60 Hz — the burst shape: everything out in two frames.
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/1000.0f, /*ltMin*/0.1f, /*ltMax*/0.1f,
        /*maxP*/12, /*vel*/{0,2,0}, /*spread*/0.5f, /*gravity*/{0,-2,0}, /*looping*/false);
    reg.emplace<ParticleSystemComponent>(e, ps);

    ParticleSystem::update(world, cm, 1.0f / 60.0f);
    CHECK(reg.get<ParticleSystemComponent>(e).emitted == 12);

    for (int i = 0; i < 30; ++i) ParticleSystem::update(world, cm, 1.0f / 60.0f);
    CHECK(!reg.get<ParticleSystemComponent>(e).playing);
}

// MUTATION: in ParticleSystem::update, use `tc.worldMatrix[3]` again instead of
// worldMatrixOf — every particle of a just-spawned effect appears at the origin.
TEST_CASE("ParticleSystem::update: a freshly placed emitter emits where it stands")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("effect");
    TransformComponent t;
    t.position = { 30.0f, 4.0f, -12.0f };
    // worldMatrix left at its default IDENTITY on purpose: that is the state an
    // entity spawned this frame is in, because transform propagation has not run
    // yet. The emitter must not believe it.
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/100.0f, /*ltMin*/5.0f, /*ltMax*/5.0f,
        /*maxP*/20);
    reg.emplace<ParticleSystemComponent>(e, ps);

    ParticleSystem::update(world, cm, 0.1f);
    const auto& psc = reg.get<ParticleSystemComponent>(e);
    REQUIRE(!psc.particles.empty());
    CHECK(psc.particles[0].position.x == doctest::Approx(30.0f));
    CHECK(psc.particles[0].position.z == doctest::Approx(-12.0f));
}

// MUTATION: in ParticleSystem::update, drop the finishedAndDone loop — the spent
// effect entity stays in the world for the rest of the session.
TEST_CASE("ParticleSystem::update: a one-shot that asked to be cleaned up takes its entity with it")
{
    ContentManager cm;
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("hit effect");
    TransformComponent t;
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    ps.destroyWhenFinished = true;
    ps.particleAssetId = makeConfiguredParticleAsset(cm, /*emitRate*/60.0f, /*ltMin*/0.1f, /*ltMax*/0.1f,
        /*maxP*/6, /*vel*/{0,2,0}, /*spread*/0.5f, /*gravity*/{0,-2,0}, /*looping*/false);
    reg.emplace<ParticleSystemComponent>(e, ps);

    // It survives while it is still playing — an effect deleted mid-puff would be
    // worse than one that lingers.
    ParticleSystem::update(world, cm, 1.0f / 60.0f);
    CHECK(reg.valid(e));

    for (int i = 0; i < 60; ++i) ParticleSystem::update(world, cm, 1.0f / 60.0f);
    CHECK(!reg.valid(e));
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

// ─────────────────────────────────────────────────────────────────────────────
//  RenderExtractor — ParticleSystemComponent particles → RenderWorld::particleBatches
// ─────────────────────────────────────────────────────────────────────────────
// Color/alpha-over-life is no longer CPU-lerped per particle here (that moved to
// the GPU-instanced draw path, see HE::generateParticleShaderSource) — the
// extractor now hands the backend raw position/size/t01 plus the resolved
// config's color/alpha endpoints (once per batch, not per particle).

TEST_CASE("RenderExtractor batches a ParticleSystemComponent's particles with raw position/size/t01")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {0, 0, 0};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    reg.emplace<ParticleSystemComponent>(e, ps);
    auto& psc = reg.get<ParticleSystemComponent>(e);
    // Bypass graph resolution entirely — set the resolved config directly, the
    // same way ParticleSystem::update would after evaluating the graph.
    psc.resolvedConfig.startColor[0] = 1.0f; psc.resolvedConfig.startColor[1] = 0.0f; psc.resolvedConfig.startColor[2] = 0.0f;
    psc.resolvedConfig.endColor[0]   = 0.0f; psc.resolvedConfig.endColor[1]   = 0.0f; psc.resolvedConfig.endColor[2]   = 1.0f;
    psc.resolvedConfig.startAlpha = 1.0f;
    psc.resolvedConfig.endAlpha   = 0.0f;
    psc.resolvedConfig.startSize  = 1.0f;
    psc.resolvedConfig.endSize    = 3.0f;

    Particle p;
    p.position    = {2, 0, 0};
    p.maxLifetime = 10.0f;
    p.lifetime    = 5.0f; // t01 = 1 - lifetime/maxLifetime = 0.5 → halfway through its life
    psc.particles.push_back(p);

    RenderWorld rw;
    RenderExtractor extractor;
    extractor.setParticleSpritesCapable(true);   // a backend with the instanced sprite pass (GL, Metal)
    extractor.extract(world, rw, 16.0f / 9.0f);

    CHECK(rw.objects.empty()); // no more one-RenderObject-per-particle
    REQUIRE(rw.particleBatches.size() == 1);
    const ParticleBatch& batch = rw.particleBatches[0];
    REQUIRE(batch.instances.size() == 1);
    CHECK(batch.instances[0].position == glm::vec3(2, 0, 0));
    CHECK(batch.instances[0].t01  == doctest::Approx(0.5f));
    CHECK(batch.instances[0].size == doctest::Approx(2.0f)); // lerp(1,3,0.5) — still CPU-lerped
    // Color/alpha endpoints travel once per batch, not pre-lerped per particle.
    CHECK(batch.config.startColor[0] == doctest::Approx(1.0f));
    CHECK(batch.config.endColor[2]   == doctest::Approx(1.0f));
    CHECK(batch.config.startAlpha    == doctest::Approx(1.0f));
    CHECK(batch.config.endAlpha      == doctest::Approx(0.0f));
}

TEST_CASE("RenderExtractor: a fresh particle (lifetime==maxLifetime) gets t01=0")
{
    HorizonWorld world;
    auto& reg = world.registry();

    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.position    = {0, 0, 0};
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);

    ParticleSystemComponent ps;
    reg.emplace<ParticleSystemComponent>(e, ps);
    auto& psc = reg.get<ParticleSystemComponent>(e);
    psc.resolvedConfig.startSize  = 1.0f;
    psc.resolvedConfig.endSize    = 1.0f;

    Particle p;
    p.position    = {0, 0, 0};
    p.maxLifetime = 4.0f;
    p.lifetime    = 4.0f; // just born → t01 = 0
    psc.particles.push_back(p);

    RenderWorld rw;
    RenderExtractor extractor;
    extractor.setParticleSpritesCapable(true);
    extractor.extract(world, rw, 16.0f / 9.0f);

    REQUIRE(rw.particleBatches.size() == 1);
    REQUIRE(rw.particleBatches[0].instances.size() == 1);
    CHECK(rw.particleBatches[0].instances[0].t01 == doctest::Approx(0.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  RenderExtractor — particle billboards (RenderObjects every backend draws)
// ─────────────────────────────────────────────────────────────────────────────
// A Streak/Puff emitter, or a Sprite on a backend without the sprite pass (D3D11,
// D3D12, Vulkan, Software), comes out as one RenderObject per particle with the
// emitter's material, grouped by tint step so GeometryPass instances them.

namespace
{
void billboardEmitter(HorizonWorld& world, HE::ParticleBillboard kind, int count)
{
    auto& reg = world.registry();
    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);
    ParticleSystemComponent ps;
    ps.resolvedConfig.billboard       = static_cast<uint8_t>(kind);
    ps.resolvedConfig.materialAssetId = HE::kEngineWaterSprayMaterialId;
    ps.resolvedConfig.startSize = 0.2f;  ps.resolvedConfig.endSize = 0.2f;
    ps.resolvedConfig.startAlpha = 1.0f; ps.resolvedConfig.endAlpha = 0.0f;
    ps.resolvedConfig.stretch = 0.1f;
    for (int i = 0; i < count; ++i)
    {
        Particle p;
        p.position    = { static_cast<float>(i) * 0.5f, 0.0f, -10.0f };     // in front of the default camera
        p.velocity    = { 0.0f, -10.0f, 0.0f };                     // falling
        p.maxLifetime = 1.0f + 0.01f * static_cast<float>(i);
        // Alternating young and old: written out of order, they must come out grouped.
        p.lifetime    = (i % 2 == 0 ? 0.95f : 0.15f) * p.maxLifetime;
        ps.particles.push_back(p);
    }
    reg.emplace<ParticleSystemComponent>(e, ps);
}
}

TEST_CASE("RenderExtractor: a Streak emitter is ONE particle band on every backend, its quads stretched along the fall")
{
    HorizonWorld world;
    billboardEmitter(world, HE::ParticleBillboard::Streak, 10);

    for (const bool sprites : { false, true })   // D3D/Vulkan and GL/Metal alike
    {
        RenderWorld rw;
        RenderExtractor extractor;
        extractor.setParticleSpritesCapable(sprites);
        extractor.extract(world, rw, 16.0f / 9.0f);
        CHECK(rw.particleBatches.empty());
        CHECK(rw.objects.empty());                     // not a RenderObject per particle: one draw per emitter
        REQUIRE(rw.ribbonBatches.size() == 1u);
        const RibbonBatch& rb = rw.ribbonBatches[0];
        CHECK(rb.materialAssetId == HE::kEngineWaterSprayMaterialId);
        CHECK(rb.worldBounds.isValid());
        REQUIRE(rb.vertices.size() == 10u * 4u * 8u);  // a quad per particle, pos3 + norm3 + uv2
        REQUIRE(rb.indices.size() == 10u * 6u);
        auto pos = [&](size_t v) { return glm::vec3(rb.vertices[v * 8], rb.vertices[v * 8 + 1], rb.vertices[v * 8 + 2]); };
        auto uv  = [&](size_t v) { return glm::vec2(rb.vertices[v * 8 + 6], rb.vertices[v * 8 + 7]); };
        // The fade rides in floor(uv.y): young drops (alpha ~0.95) high, old ones (~0.15) low, and the quad's own UV
        // in fract(uv) spans the whole quad.
        int young = 0, old = 0;
        for (size_t q = 0; q < 10; ++q)
        {
            const float level = std::floor(uv(q * 4).y);
            CHECK(std::floor(uv(q * 4 + 2).y) == level);                      // one level across the quad
            const float fade = level / kParticleBandLevels;
            if (fade > 0.8f) ++young;
            if (fade < 0.25f) ++old;
            CHECK(uv(q * 4).y - level < 0.01f);
            CHECK(uv(q * 4 + 2).y - level > 0.99f);
        }
        CHECK(young == 5);
        CHECK(old == 5);
        // Back to front: the first quad is the farthest from the camera.
        const glm::vec3 cam = rw.camera.position;
        const glm::vec3 c0 = 0.5f * (pos(0) + pos(2)), cLast = 0.5f * (pos(36) + pos(38));
        CHECK(glm::length(c0 - cam) >= glm::length(cLast - cam));
        // The streak's long axis is the fall (world -Y seen from a camera looking along Z), its length size + speed *
        // stretch = 0.2 + 10 * 0.1 = 1.2 m, its width the size.
        const glm::vec3 along = pos(3) - pos(0), across = pos(1) - pos(0);
        CHECK(std::abs(glm::normalize(along).y) > 0.9f);
        CHECK(glm::length(along) == doctest::Approx(1.2f).epsilon(0.05));
        CHECK(glm::length(across) == doctest::Approx(0.2f).epsilon(0.05));
    }
}

TEST_CASE("RenderExtractor: a big band is back to front over every quad, inside its bounds, and the same from recycled memory")
{
    // 3000 drops scattered through a 20 m box in front of the camera: enough that the band is sorted by the radix
    // sort's every pass and written in parallel chunks, and big enough that its buffers are handed to the next walk.
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);
    ParticleSystemComponent ps;
    ps.resolvedConfig.billboard       = static_cast<uint8_t>(HE::ParticleBillboard::Streak);
    ps.resolvedConfig.materialAssetId = HE::kEngineWaterSprayMaterialId;
    ps.resolvedConfig.startSize = 0.2f;  ps.resolvedConfig.endSize = 0.2f;
    ps.resolvedConfig.startAlpha = 1.0f; ps.resolvedConfig.endAlpha = 0.5f;
    ps.resolvedConfig.stretch = 0.1f;
    uint32_t s = 12345u;
    auto rnd = [&] { s = s * 1664525u + 1013904223u; return static_cast<float>(s >> 8) / 16777216.0f; };
    const size_t kCount = 3000;
    for (size_t i = 0; i < kCount; ++i)
    {
        Particle p;
        p.position    = { -10.0f + 20.0f * rnd(), -10.0f + 20.0f * rnd(), -30.0f + 20.0f * rnd() };
        p.velocity    = { 0.0f, -2.0f - 8.0f * rnd(), 0.0f };
        p.maxLifetime = 1.0f + rnd();
        p.lifetime    = p.maxLifetime * (0.1f + 0.8f * rnd());
        ps.particles.push_back(p);
    }
    reg.emplace<ParticleSystemComponent>(e, ps);

    RenderWorld rw;
    RenderExtractor extractor;
    std::vector<float> first;
    for (int walk = 0; walk < 2; ++walk)
    {
        extractor.extract(world, rw, 16.0f / 9.0f);
        REQUIRE(rw.ribbonBatches.size() == 1u);
        const RibbonBatch& rb = rw.ribbonBatches[0];
        REQUIRE(rb.vertices.size() == kCount * 4u * 8u);
        REQUIRE(rb.indices.size() == kCount * 6u);
        const glm::vec3 cam = rw.camera.position;
        // A quad's centre is its particle: no farther than the quad before it.
        float before = std::numeric_limits<float>::max();
        size_t outOfOrder = 0, outside = 0;
        for (size_t q = 0; q < kCount; ++q)
        {
            const size_t v0 = q * 32, v2 = q * 32 + 16;
            const glm::vec3 c = 0.5f * glm::vec3(rb.vertices[v0] + rb.vertices[v2], rb.vertices[v0 + 1] + rb.vertices[v2 + 1],
                                                 rb.vertices[v0 + 2] + rb.vertices[v2 + 2]);
            const glm::vec3 d = c - cam;
            const float dd = glm::dot(d, d);
            if (dd > before * (1.0f + 1e-5f)) ++outOfOrder;
            before = dd;
            for (size_t k = 0; k < 4; ++k)
            {
                const glm::vec3 p(rb.vertices[q * 32 + k * 8], rb.vertices[q * 32 + k * 8 + 1], rb.vertices[q * 32 + k * 8 + 2]);
                if (glm::any(glm::lessThan(p, rb.worldBounds.min - glm::vec3(1e-4f))) ||
                    glm::any(glm::greaterThan(p, rb.worldBounds.max + glm::vec3(1e-4f)))) ++outside;
            }
            // Each quad's own two triangles, on its own four vertices.
            CHECK(rb.indices[q * 6] == static_cast<uint32_t>(q * 4));
            CHECK(rb.indices[q * 6 + 5] == static_cast<uint32_t>(q * 4 + 3));
        }
        CHECK(outOfOrder == 0u);
        CHECK(outside == 0u);
        // The second walk writes into the first one's memory (handed over when `rw` is cleared): the same band.
        if (walk == 0) first = rb.vertices;
        else           CHECK(rb.vertices == first);
    }
}

TEST_CASE("RenderExtractor: a Sprite emitter is a sprite batch where the backend has the pass, billboards where it has not")
{
    HorizonWorld world;
    billboardEmitter(world, HE::ParticleBillboard::Sprite, 4);
    {
        RenderWorld rw;
        RenderExtractor extractor;
        extractor.setParticleSpritesCapable(true);
        extractor.extract(world, rw, 16.0f / 9.0f);
        CHECK(rw.objects.empty());
        REQUIRE(rw.particleBatches.size() == 1u);
        CHECK(rw.particleBatches[0].instances.size() == 4u);
    }
    {
        RenderWorld rw;
        RenderExtractor extractor;                     // D3D11, D3D12, Vulkan, Software
        extractor.extract(world, rw, 16.0f / 9.0f);
        CHECK(rw.particleBatches.empty());
        REQUIRE(rw.objects.size() == 4u);
        // A sprite faces the camera square: as wide as high.
        const glm::mat4& m = rw.objects[0].transform;
        CHECK(glm::length(glm::vec3(m[0])) == doctest::Approx(glm::length(glm::vec3(m[1]))));
    }
}

TEST_CASE("Emitter modifiers: a placed emitter tunes the asset's config — scaled, turned to a heading, the pool grown with it")
{
    HE::ParticleEmitterConfig c;
    c.emitRate = 100.0f; c.lifetimeMin = 1.0f; c.lifetimeMax = 2.0f; c.maxParticles = 200;
    c.startSize = 0.2f; c.endSize = 0.4f; c.startAlpha = 0.5f; c.endAlpha = 0.25f; c.velocitySpread = 0.1f;
    c.initialVelocity[0] = 1.0f; c.initialVelocity[1] = 2.0f; c.initialVelocity[2] = 0.0f;   // forward 1, up 2
    c.spawnHalfA[2] = 0.75f;                                                                // across the graph's +Z
    // Identity: untouched.
    const HE::ParticleEmitterConfig same = applyEmitterModifiers(c, ParticleEmitterModifiers{});
    CHECK(same.emitRate == c.emitRate);
    CHECK(same.initialVelocity[0] == 1.0f);
    CHECK(same.spawnHalfA[2] == 0.75f);

    ParticleEmitterModifiers m;
    m.rateScale = 2.0f; m.lifetimeScale = 1.5f; m.sizeScale = 2.0f; m.alphaScale = 4.0f; m.spreadScale = 3.0f;
    m.velocityScale = glm::vec3(3.0f, 0.5f, 1.0f);
    m.spawnScale = glm::vec2(2.0f, 1.0f);
    m.heading = glm::vec2(0.0f, 1.0f);   // the graph's +X turns to world +Z
    const HE::ParticleEmitterConfig t = applyEmitterModifiers(c, m);
    CHECK(t.emitRate == doctest::Approx(200.0f));
    CHECK(t.lifetimeMax == doctest::Approx(3.0f));
    CHECK(t.maxParticles >= 600);                          // room grows with rate x life
    CHECK(t.startSize == doctest::Approx(0.4f));
    CHECK(t.startAlpha == doctest::Approx(1.0f));          // clamped
    CHECK(t.velocitySpread == doctest::Approx(0.3f));
    CHECK(t.initialVelocity[0] == doctest::Approx(0.0f).epsilon(1e-5).scale(1.0f));
    CHECK(t.initialVelocity[1] == doctest::Approx(1.0f));  // up 2 x 0.5
    CHECK(t.initialVelocity[2] == doctest::Approx(3.0f));  // forward 1 x 3, now along +Z
    // The graph's across (+Z) turns with it: a quarter turn to world -X, and is twice as wide.
    CHECK(t.spawnHalfA[0] == doctest::Approx(-1.5f));
    CHECK(std::abs(t.spawnHalfA[2]) < 1e-5f);
}

TEST_CASE("ParticleSystem: a spawn strip spreads the births across it, a point emitter keeps its old draws")
{
    ContentManager cm{ "." };
    HE::ParticleEmitterConfig c;
    c.emitRate = 2000.0f; c.lifetimeMin = c.lifetimeMax = 5.0f; c.maxParticles = 4000;
    c.initialVelocity[0] = 0.0f; c.initialVelocity[1] = 0.0f; c.initialVelocity[2] = 0.0f;
    c.velocitySpread = 0.0f;
    c.gravity[1] = 0.0f;
    c.spawnHalfA[2] = 1.0f;              // a strip 2 m long along Z
    HorizonWorld world;
    auto& reg = world.registry();
    auto e = world.createEntity("emitter");
    TransformComponent t;
    t.worldMatrix = glm::mat4(1.0f);
    reg.emplace<TransformComponent>(e, t);
    ParticleSystemComponent ps;
    ps.configDirty = false;
    ps.resolvedConfig = c;
    reg.emplace<ParticleSystemComponent>(e, ps);
    ParticleSystem::update(world, cm, 0.25f);
    const auto& live = reg.get<ParticleSystemComponent>(e).particles;
    REQUIRE(live.size() > 100u);
    float lo = 1e9f, hi = -1e9f, xMax = 0.0f;
    for (const Particle& p : live) { lo = std::min(lo, p.position.z); hi = std::max(hi, p.position.z); xMax = std::max(xMax, std::abs(p.position.x)); }
    CHECK(lo < -0.8f);
    CHECK(hi > 0.8f);
    CHECK(lo >= -1.0001f);
    CHECK(hi <= 1.0001f);
    CHECK(xMax < 1e-4f);                  // only along the strip
}

// ─────────────────────────────────────────────────────────────────────────────
//  ParticleSystem::stepPool — physics collision (config.collisionEnabled)
// ─────────────────────────────────────────────────────────────────────────────

// A static 1×1×1 box at the origin (top surface at y=0.5) — same fixture shape
// as tests/test_raycast.cpp's buildBoxWorld.
static Entity buildFloorWorld(HorizonWorld& world)
{
    Entity floor = world.createEntity("Floor");
    TransformComponent t;
    t.position = { 0.0f, 0.0f, 0.0f };
    t.scale    = { 1.0f, 1.0f, 1.0f };
    world.addComponent(floor, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Static;
    world.addComponent(floor, rb);
    ColliderComponent col;
    col.shape       = ColliderShape::Box;
    col.halfExtents = { 0.5f, 0.5f, 0.5f };
    world.addComponent(floor, col);
    return floor;
}

TEST_CASE("ParticleSystem::stepPool: killOnCollision removes the particle on hit")
{
    HorizonWorld world;
    buildFloorWorld(world);
    PhysicsWorld phys;
    phys.initialize(world);

    HE::ParticleEmitterConfig config;
    config.emitRate          = 0.0f; // no new spawns — simulate only the one we push
    config.gravity[0] = config.gravity[1] = config.gravity[2] = 0.0f;
    config.collisionEnabled  = true;
    config.killOnCollision   = true;

    std::vector<Particle> particles;
    Particle p; p.position = { 0.0f, 2.0f, 0.0f }; p.velocity = { 0.0f, -10.0f, 0.0f };
    p.lifetime = 10.0f; p.maxLifetime = 10.0f;
    particles.push_back(p);

    float acc = 0.0f;
    std::mt19937 rng{ 1 };
    int emitted = 0;
    ParticleSystem::stepPool({ particles, acc, emitted, rng }, config, glm::vec3(0.0f), 1.0f, &phys);

    CHECK(particles.empty());
}

TEST_CASE("ParticleSystem::stepPool: bounces and scales velocity by restitution on hit")
{
    HorizonWorld world;
    buildFloorWorld(world);
    PhysicsWorld phys;
    phys.initialize(world);

    HE::ParticleEmitterConfig config;
    config.emitRate          = 0.0f;
    config.gravity[0] = config.gravity[1] = config.gravity[2] = 0.0f;
    config.collisionEnabled  = true;
    config.killOnCollision   = false;
    config.restitution       = 0.5f;

    std::vector<Particle> particles;
    Particle p; p.position = { 0.0f, 2.0f, 0.0f }; p.velocity = { 0.0f, -10.0f, 0.0f };
    p.lifetime = 10.0f; p.maxLifetime = 10.0f;
    particles.push_back(p);

    float acc = 0.0f;
    std::mt19937 rng{ 1 };
    int emitted = 0;
    ParticleSystem::stepPool({ particles, acc, emitted, rng }, config, glm::vec3(0.0f), 1.0f, &phys);

    REQUIRE(particles.size() == 1);
    // Reflected off the upward-facing floor normal → velocity.y flips positive,
    // scaled by restitution (0.5 × the 10 m/s incoming speed).
    CHECK(particles[0].velocity.y > 0.0f);
    CHECK(particles[0].velocity.y == doctest::Approx(5.0f).epsilon(0.2f));
    // Snapped to just above the hit surface, not still tunneling downward.
    CHECK(particles[0].position.y >= 0.5f);
}

TEST_CASE("ParticleSystem::stepPool: collisionEnabled=false ignores the physics world entirely")
{
    HorizonWorld world;
    buildFloorWorld(world);
    PhysicsWorld phys;
    phys.initialize(world);

    HE::ParticleEmitterConfig config;
    config.emitRate          = 0.0f;
    config.gravity[0] = config.gravity[1] = config.gravity[2] = 0.0f;
    config.collisionEnabled  = false; // default

    std::vector<Particle> particles;
    Particle p; p.position = { 0.0f, 2.0f, 0.0f }; p.velocity = { 0.0f, -10.0f, 0.0f };
    p.lifetime = 10.0f; p.maxLifetime = 10.0f;
    particles.push_back(p);

    float acc = 0.0f;
    std::mt19937 rng{ 1 };
    int emitted = 0;
    ParticleSystem::stepPool({ particles, acc, emitted, rng }, config, glm::vec3(0.0f), 1.0f, &phys);

    REQUIRE(particles.size() == 1);
    // Passed straight through the floor — no collision check performed.
    CHECK(particles[0].position.y == doctest::Approx(-8.0f));
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
