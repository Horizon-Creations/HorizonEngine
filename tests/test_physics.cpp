#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/JointComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/TransformHierarchy.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <Physics/CollisionLayers.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

static constexpr float kDt      = 1.0f / 60.0f;
static constexpr int   kSteps2s = 120; // 2 seconds at 60 Hz

// ─── Basic init ───────────────────────────────────────────────────────────────

TEST_CASE("PhysicsWorld: initializes without crash on empty world")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);
    // step should be a no-op for empty world
    phys.step(world, kDt);
}

TEST_CASE("PhysicsWorld: step on uninitialised world is safe")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.step(world, kDt); // not initialised — should not crash
}

// ─── Dynamic body falls under gravity ─────────────────────────────────────────

TEST_CASE("PhysicsWorld: dynamic body falls under gravity")
{
    HorizonWorld world;
    Entity box = world.createEntity("Box");

    TransformComponent t;
    t.position = { 0.0f, 10.0f, 0.0f };
    t.scale    = { 1.0f,  1.0f, 1.0f };
    world.addComponent(box, t);

    RigidBodyComponent rb;
    rb.type = RigidBodyType::Dynamic;
    rb.mass = 1.0f;
    world.addComponent(box, rb);

    PhysicsWorld phys;
    phys.initialize(world);

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    const auto& tr = world.registry().get<TransformComponent>(box);
    // Free fall 2 s: Δy ≈ ½·9.81·4 ≈ 19.6 m — body well below start
    CHECK(tr.position.y < 5.0f);
}

// ─── Static body stays put ────────────────────────────────────────────────────

TEST_CASE("PhysicsWorld: static body does not move")
{
    HorizonWorld world;
    Entity floor = world.createEntity("Floor");

    TransformComponent t;
    t.position = { 0.0f, 0.0f, 0.0f };
    t.scale    = { 10.0f, 0.2f, 10.0f };
    world.addComponent(floor, t);

    RigidBodyComponent rb;
    rb.type = RigidBodyType::Static;
    world.addComponent(floor, rb);

    PhysicsWorld phys;
    phys.initialize(world);

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    const auto& tr = world.registry().get<TransformComponent>(floor);
    CHECK(tr.position.y == doctest::Approx(0.0f));
    CHECK(tr.position.x == doctest::Approx(0.0f));
    CHECK(tr.position.z == doctest::Approx(0.0f));
}

// ─── Dynamic falls, static stays (two bodies together) ───────────────────────

TEST_CASE("PhysicsWorld: dynamic falls while static stays")
{
    HorizonWorld world;

    Entity floor = world.createEntity("Floor");
    {
        TransformComponent t;
        t.position = { 0.0f, -5.0f, 0.0f };
        t.scale    = { 20.0f, 0.2f, 20.0f };
        world.addComponent(floor, t);
        RigidBodyComponent rb;
        rb.type = RigidBodyType::Static;
        world.addComponent(floor, rb);
    }

    Entity box = world.createEntity("Box");
    {
        TransformComponent t;
        t.position = { 0.0f, 5.0f, 0.0f };
        t.scale    = { 1.0f, 1.0f, 1.0f };
        world.addComponent(box, t);
        RigidBodyComponent rb;
        rb.type = RigidBodyType::Dynamic;
        rb.mass = 1.0f;
        world.addComponent(box, rb);
    }

    PhysicsWorld phys;
    phys.initialize(world);

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    const auto& floorTr = world.registry().get<TransformComponent>(floor);
    const auto& boxTr   = world.registry().get<TransformComponent>(box);

    CHECK(floorTr.position.y == doctest::Approx(-5.0f)); // static: unchanged
    CHECK(boxTr.position.y < 4.0f);                       // dynamic: fell
    // Box landed on floor (floor top = -5 + 0.1 = -4.9, box half = 0.5 → rest at -4.4)
    CHECK(boxTr.position.y > -6.0f);                      // didn't pass through floor
}

// ─── Kinematic body does not respond to gravity ───────────────────────────────

TEST_CASE("PhysicsWorld: kinematic body is not driven by gravity")
{
    HorizonWorld world;
    Entity obj = world.createEntity("Kinematic");

    TransformComponent t;
    t.position = { 0.0f, 5.0f, 0.0f };
    t.scale    = { 1.0f, 1.0f, 1.0f };
    world.addComponent(obj, t);

    RigidBodyComponent rb;
    rb.type = RigidBodyType::Kinematic;
    world.addComponent(obj, rb);

    PhysicsWorld phys;
    phys.initialize(world);

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    const auto& tr = world.registry().get<TransformComponent>(obj);
    // Kinematic bodies are not affected by gravity — position unchanged
    CHECK(tr.position.y == doctest::Approx(5.0f).epsilon(0.01));
}

// ─── clear() is safe to call multiple times ───────────────────────────────────

TEST_CASE("PhysicsWorld: clear is idempotent")
{
    HorizonWorld world;
    Entity e = world.createEntity("E");
    TransformComponent t; t.scale = { 1, 1, 1 };
    world.addComponent(e, t);
    world.addComponent(e, RigidBodyComponent{});

    PhysicsWorld phys;
    phys.initialize(world);
    phys.clear();
    phys.clear(); // second clear must not crash
}

// ─── Re-initialize after clear ────────────────────────────────────────────────

TEST_CASE("PhysicsWorld: can re-initialize after clear")
{
    HorizonWorld world;
    Entity e = world.createEntity("E");
    TransformComponent t; t.position = { 0, 10, 0 }; t.scale = { 1, 1, 1 };
    world.addComponent(e, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
    world.addComponent(e, rb);

    PhysicsWorld phys;
    phys.initialize(world);
    phys.clear();

    // Reset entity position (simulate a restart)
    world.registry().get<TransformComponent>(e).position.y = 10.0f;

    phys.initialize(world);
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    CHECK(world.registry().get<TransformComponent>(e).position.y < 5.0f);
}

// ─── Collision events ─────────────────────────────────────────────────────────

TEST_CASE("PhysicsWorld: pollCollisionEnter returns empty before any step")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);
    CHECK(phys.pollCollisionEnter().empty());
    CHECK(phys.pollCollisionExit().empty());
}

TEST_CASE("PhysicsWorld: pollCollisionEnter returns empty on uninitialised world")
{
    PhysicsWorld phys;
    CHECK(phys.pollCollisionEnter().empty());
    CHECK(phys.pollCollisionExit().empty());
}

TEST_CASE("PhysicsWorld: pollCollisionEnter is idempotent after drain")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);
    phys.step(world, kDt);
    // drain twice — second call must return empty
    phys.pollCollisionEnter();
    CHECK(phys.pollCollisionEnter().empty());
}

TEST_CASE("PhysicsWorld: collision detected between falling body and floor")
{
    HorizonWorld world;

    Entity floor = world.createEntity("Floor");
    {
        TransformComponent t; t.position = {0, 0, 0}; t.scale = {20, 0.5f, 20};
        world.addComponent(floor, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Static;
        world.addComponent(floor, rb);
    }

    Entity box = world.createEntity("Box");
    {
        TransformComponent t; t.position = {0, 2, 0}; t.scale = {1, 1, 1};
        world.addComponent(box, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
        world.addComponent(box, rb);
    }

    PhysicsWorld phys;
    phys.initialize(world);

    bool gotEnter = false;
    for (int i = 0; i < kSteps2s && !gotEnter; ++i)
    {
        phys.step(world, kDt);
        auto evts = phys.pollCollisionEnter();
        if (!evts.empty()) gotEnter = true;
    }

    CHECK(gotEnter);
}

// ─── Overlap query ────────────────────────────────────────────────────────────

namespace
{
    // A one-metre static box — enough to be found by a query, and it stays where
    // it was put, so a test can name the expected answer.
    Entity makeStaticBox(HorizonWorld& world, const char* name, const glm::vec3& pos)
    {
        Entity e = world.createEntity(name);
        TransformComponent t; t.position = pos; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(e, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Static;
        world.addComponent(e, rb);
        return e;
    }

    Entity makeDynamicBox(HorizonWorld& world, const char* name, const glm::vec3& pos)
    {
        Entity e = world.createEntity(name);
        TransformComponent t; t.position = pos; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(e, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
        world.addComponent(e, rb);
        return e;
    }
}

TEST_CASE("PhysicsWorld: overlapSphere reports what is in range and nothing else")
{
    HorizonWorld world;
    Entity inside  = makeStaticBox(world, "Inside",  { 0.0f,  0.0f, 0.0f });
    Entity outside = makeStaticBox(world, "Outside", { 20.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    const auto close = phys.overlapSphere({ 0.0f, 0.0f, 0.0f }, 2.0f);
    REQUIRE(close.size() == 1);
    CHECK(close[0] == static_cast<uint32_t>(inside));

    CHECK(phys.overlapSphere({ 0.0f, 0.0f, 0.0f }, 30.0f).size() == 2);

    // The ignore parameter is what keeps an explosion from damaging the thing
    // that set it off.
    const auto others = phys.overlapSphere({ 0.0f, 0.0f, 0.0f }, 30.0f,
                                           static_cast<uint32_t>(inside));
    REQUIRE(others.size() == 1);
    CHECK(others[0] == static_cast<uint32_t>(outside));

    // Empty space is an empty list, not a failure.
    CHECK(phys.overlapSphere({ 0.0f, 100.0f, 0.0f }, 1.0f).empty());
    CHECK(phys.overlapSphere({ 0.0f, 0.0f, 0.0f }, 0.0f).empty());   // degenerate radius
}

// ─── Forces and velocity on rigid bodies ──────────────────────────────────────

TEST_CASE("PhysicsWorld: addImpulse wakes a resting body and moves it")
{
    HorizonWorld world;

    Entity floor = world.createEntity("Floor");
    {
        TransformComponent t; t.position = {0, 0, 0}; t.scale = {20, 0.5f, 20};
        world.addComponent(floor, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Static;
        world.addComponent(floor, rb);
    }
    Entity box = makeDynamicBox(world, "Box", { 0.0f, 2.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    // Two seconds is well past Jolt's sleep threshold, so the box is not merely
    // resting on the floor, it is asleep — which is the state an impulse has to
    // survive, and the one that makes the feature look broken when it does not.
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    const float settled = world.registry().get<TransformComponent>(box).position.y;

    CHECK(phys.addImpulse(static_cast<uint32_t>(box), { 0.0f, 10.0f, 0.0f }));
    for (int i = 0; i < 10; ++i)
        phys.step(world, kDt);

    CHECK(world.registry().get<TransformComponent>(box).position.y > settled + 0.5f);
}

TEST_CASE("PhysicsWorld: forces refuse instead of doing nothing quietly")
{
    HorizonWorld world;
    Entity wall  = makeStaticBox(world, "Wall", { 0.0f, 0.0f, 0.0f });
    Entity ghost = world.createEntity("NoBody");   // transform only — never gets a body
    { TransformComponent t; world.addComponent(ghost, t); }

    PhysicsWorld phys;
    phys.initialize(world);

    // A static body has no solver state to push …
    CHECK_FALSE(phys.addForce(static_cast<uint32_t>(wall),   { 1.0f, 0.0f, 0.0f }));
    CHECK_FALSE(phys.addImpulse(static_cast<uint32_t>(wall), { 1.0f, 0.0f, 0.0f }));
    CHECK_FALSE(phys.addTorque(static_cast<uint32_t>(wall),  { 1.0f, 0.0f, 0.0f }));
    CHECK_FALSE(phys.setVelocity(static_cast<uint32_t>(wall), { 1.0f, 0.0f, 0.0f }));

    // … and an entity without a RigidBodyComponent has nothing at all.
    CHECK_FALSE(phys.addForce(static_cast<uint32_t>(ghost), { 1.0f, 0.0f, 0.0f }));
    CHECK_FALSE(phys.setVelocity(static_cast<uint32_t>(ghost), { 1.0f, 0.0f, 0.0f }));
    CHECK(phys.getVelocity(static_cast<uint32_t>(ghost)) == glm::vec3(0.0f));

    // An id no allocator ever handed out must be as safe as a real one.
    CHECK_FALSE(phys.addImpulse(99999u, { 1.0f, 0.0f, 0.0f }));
    CHECK(phys.getVelocity(99999u) == glm::vec3(0.0f));
}

TEST_CASE("PhysicsWorld: setVelocity drives a rigid body and reads back")
{
    HorizonWorld world;
    Entity box = makeDynamicBox(world, "Box", { 0.0f, 10.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    phys.setGravity({ 0.0f, 0.0f, 0.0f });   // isolate the push from the fall

    CHECK(phys.setVelocity(static_cast<uint32_t>(box), { 5.0f, 0.0f, 0.0f }));
    CHECK(phys.getVelocity(static_cast<uint32_t>(box)).x == doctest::Approx(5.0f));

    for (int i = 0; i < 60; ++i)
        phys.step(world, kDt);

    const auto& tr = world.registry().get<TransformComponent>(box);
    CHECK(tr.position.x > 3.0f);                                  // damping eats a little
    CHECK(tr.position.y == doctest::Approx(10.0f).epsilon(0.01));
}

// ─── World gravity ────────────────────────────────────────────────────────────

TEST_CASE("PhysicsWorld: gravity round-trips and wakes what had settled")
{
    HorizonWorld world;
    Entity box = makeDynamicBox(world, "Box", { 0.0f, 10.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    CHECK(phys.gravity().y == doctest::Approx(-9.81f));

    // Weightless: the box holds its height instead of falling.
    phys.setGravity({ 0.0f, 0.0f, 0.0f });
    CHECK(phys.gravity() == glm::vec3(0.0f));
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    CHECK(world.registry().get<TransformComponent>(box).position.y
          == doctest::Approx(10.0f).epsilon(0.01));

    // Two seconds of nothing put the box to sleep, and a sleeping body is not
    // stepped — so turning gravity back on has to wake it, or it hangs there.
    phys.setGravity({ 0.0f, -9.81f, 0.0f });
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    CHECK(world.registry().get<TransformComponent>(box).position.y < 5.0f);
}

// ─── Runtime composition (B1: the physics world used to freeze at scene start) ─
//
// Everything below this line is the audit's first blocker. entityToBody and
// entityToCharacter were filled ONLY by initialize(), CreateAndAddBody appeared
// only there, and RemoveBody/DestroyBody only in clear(). So the set of things
// that could collide was decided at scene start and never changed again: no
// spawn got a body, no deletion gave one back, and no script could move one.
//
// Each case below names what it did BEFORE the change, because the whole reason
// these blockers survived is that nothing ever asked.

TEST_CASE("PhysicsWorld: an entity created after initialize gets a body and falls")
{
    HorizonWorld world;
    PhysicsWorld phys;

    // The scene starts EMPTY and is initialised in that state — this is the
    // ordering that matters, because initialize() calls clear() first and would
    // discard anything added before it. Everything after this line is a runtime
    // spawn: a projectile, a pickup, a dropped crate.
    phys.initialize(world);

    Entity spawned = makeDynamicBox(world, "SpawnedCrate", { 0.0f, 10.0f, 0.0f });

    // BEFORE THE CHANGE: there was no addEntity at all, so this entity stayed
    // bodiless for the rest of the session. hasPhysics would have been false and
    // the box would have hung at y = 10 forever — a spawned object that falls
    // through nothing because it is part of nothing.
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(spawned)));
    CHECK(phys.addEntity(world, static_cast<uint32_t>(spawned)));
    CHECK(phys.hasPhysics(static_cast<uint32_t>(spawned)));

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    // Free fall for 2 s is ≈19.6 m. "It moved at all" is the real assertion:
    // the old behaviour was a body that did not exist, so y never changed.
    const auto& tr = world.registry().get<TransformComponent>(spawned);
    CHECK(tr.position.y < 5.0f);

    // And it is simulated, not merely teleported by the write-back: a body that
    // fell for two seconds carries the speed of that fall.
    CHECK(phys.getVelocity(static_cast<uint32_t>(spawned)).y < -5.0f);
}

TEST_CASE("PhysicsWorld: addEntity refuses an entity with nothing to build")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);

    // Transform only: no rigid body, no character, no terrain. There is nothing
    // to make a collider out of, and saying so beats inventing a default one.
    Entity bare = world.createEntity("JustATransform");
    { TransformComponent t; world.addComponent(bare, t); }

    CHECK_FALSE(phys.addEntity(world, static_cast<uint32_t>(bare)));
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(bare)));

    // An id no allocator handed out must be as safe as a real one.
    CHECK_FALSE(phys.addEntity(world, 99999u));
    CHECK_FALSE(phys.hasPhysics(99999u));
}

TEST_CASE("PhysicsWorld: removeEntity takes the body away and the raycast follows")
{
    HorizonWorld world;
    Entity wall = makeStaticBox(world, "Wall", { 0.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    // A one-metre box centred on the origin: the ray from above hits its top
    // face at y = 0.5.
    const glm::vec3 from{ 0.0f, 10.0f, 0.0f };
    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };

    auto before = phys.raycast(from, down, 100.0f);
    REQUIRE(before.hit);
    CHECK(before.entityId == static_cast<uint32_t>(wall));
    CHECK(phys.hasPhysics(static_cast<uint32_t>(wall)));

    phys.removeEntity(static_cast<uint32_t>(wall));

    // BEFORE THE CHANGE: there was no removeEntity, and the only teardown in the
    // class was clear(), which destroys EVERYTHING. A single destroyed entity
    // therefore kept its Jolt body forever — an invisible wall that still
    // blocked movement and still answered raycasts with a dead entity id. This
    // is the ghost collider from the audit; both assertions below would have
    // been the opposite.
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(wall)));
    CHECK_FALSE(phys.raycast(from, down, 100.0f).hit);

    // Removing twice is a no-op, not a double free.
    phys.removeEntity(static_cast<uint32_t>(wall));
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(wall)));
}

TEST_CASE("PhysicsWorld: a removed entity produces no event naming the dead id")
{
    HorizonWorld world;

    Entity floor = makeStaticBox(world, "Floor", { 0.0f, 0.0f, 0.0f });
    // Sits just above the floor so the two are in contact within a few steps.
    Entity crate = makeDynamicBox(world, "Crate", { 0.0f, 1.2f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    // Establish a real contact first — the point of the test is what happens to
    // a contact whose body is destroyed, so there has to be one. Step only until
    // the enter event actually arrives and REQUIRE that it did: a crate that
    // never touched the floor would make every assertion below pass for the
    // wrong reason. Stopping here also matters because the contact must still be
    // live and the body awake — Jolt drops a sleeping body's manifolds on its
    // own, so a test that let the crate settle first would prove nothing.
    bool touched = false;
    for (int i = 0; i < 60 && !touched; ++i)
    {
        phys.step(world, kDt);
        touched = !phys.pollCollisionEnter().empty();
    }
    REQUIRE(touched);
    phys.pollCollisionExit();
    phys.pollOverlapEnter();
    phys.pollOverlapExit();

    const uint32_t dead = static_cast<uint32_t>(crate);
    phys.removeEntity(dead);

    // Jolt fires OnContactRemoved for a destroyed body's cached contacts during
    // the NEXT Update(). Without destroyBodyFor()'s purgeEntity() the listener
    // would resolve those from its cache and hand game code an exit event for an
    // entity that no longer exists — and the code reacting to an exit ("stop
    // standing on the platform", "re-enable the trigger") almost never survives
    // being given a dead id.
    for (int i = 0; i < 5; ++i)
        phys.step(world, kDt);

    const auto mentions = [dead](const std::vector<PhysicsWorld::CollisionEvent>& evs) {
        return std::any_of(evs.begin(), evs.end(), [dead](const PhysicsWorld::CollisionEvent& e) {
            return e.entityA == dead || e.entityB == dead;
        });
    };
    CHECK_FALSE(mentions(phys.pollCollisionEnter()));
    CHECK_FALSE(mentions(phys.pollCollisionExit()));
    CHECK_FALSE(mentions(phys.pollOverlapEnter()));
    CHECK_FALSE(mentions(phys.pollOverlapExit()));
}

TEST_CASE("PhysicsWorld: step reaps the body of an entity destroyed behind its back")
{
    HorizonWorld world;
    Entity wall = makeStaticBox(world, "Wall", { 0.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 from{ 0.0f, 10.0f, 0.0f };
    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    REQUIRE(phys.raycast(from, down, 100.0f).hit);

    // Deleted through the ECS, which is what the outliner, a script's
    // entity.destroy and every other caller actually do. Nothing tells
    // PhysicsWorld — there is no hook, no observer, no notification.
    world.destroyEntity(wall);

    // BEFORE THE CHANGE: nothing would ever have looked at this body again. It
    // is static, so the write-back loop skipped it; the entity is gone, so no
    // caller could name it. The reap in step() is the only cleanup that cannot
    // be forgotten, and it costs at most one frame of ghost collider.
    phys.step(world, kDt);

    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(wall)));
    CHECK_FALSE(phys.raycast(from, down, 100.0f).hit);
}

TEST_CASE("PhysicsWorld: setPosition teleports and the move survives the next step")
{
    HorizonWorld world;
    Entity box = makeDynamicBox(world, "Respawner", { 0.0f, 10.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    // Let it fall for a while, so the body's own idea of where it is has drifted
    // a long way from where the teleport wants it.
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    REQUIRE(world.registry().get<TransformComponent>(box).position.y < 0.0f);

    // BEFORE THE CHANGE: the physics loop wrote Jolt's pose INTO
    // TransformComponent every step and never read it back, so a position set by
    // a script was overwritten within the same frame. There was no way to move a
    // physics entity at all — this is the missing respawn from the audit. A
    // plain `transform.position = ...` here would be undone by the step below.
    CHECK(phys.setPosition(static_cast<uint32_t>(box), { 3.0f, 50.0f, -4.0f }));

    // The ECS side is updated in the same call, not at the next step: between a
    // teleport and the next step sit the camera and render extraction, and a
    // stale transform draws the respawned player where they died.
    {
        const auto& tr = world.registry().get<TransformComponent>(box);
        CHECK(tr.position.x == doctest::Approx(3.0f));
        CHECK(tr.position.y == doctest::Approx(50.0f));
        CHECK(tr.position.z == doctest::Approx(-4.0f));
    }

    phys.step(world, kDt);

    // One step of simulation moves it a little (it kept the speed of the fall —
    // see the resetVelocity case below), but it must still be up at the new
    // place rather than back down where it was.
    const auto& tr = world.registry().get<TransformComponent>(box);
    CHECK(tr.position.y > 49.0f);
    CHECK(tr.position.x == doctest::Approx(3.0f).epsilon(0.02));
    CHECK(tr.position.z == doctest::Approx(-4.0f).epsilon(0.02));
}

TEST_CASE("PhysicsWorld: setPosition keeps velocity, resetVelocity zeroes it")
{
    HorizonWorld world;
    Entity keeper  = makeDynamicBox(world, "Keeper",  { 0.0f, 10.0f, 0.0f });
    Entity stopper = makeDynamicBox(world, "Stopper", { 8.0f, 10.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    // Two seconds of free fall — both are moving fast downward.
    REQUIRE(phys.getVelocity(static_cast<uint32_t>(keeper)).y  < -10.0f);
    REQUIRE(phys.getVelocity(static_cast<uint32_t>(stopper)).y < -10.0f);

    CHECK(phys.setPosition(static_cast<uint32_t>(keeper),  { 0.0f, 50.0f, 0.0f }, false));
    CHECK(phys.setPosition(static_cast<uint32_t>(stopper), { 8.0f, 50.0f, 0.0f }, true));

    // Without the flag the body keeps the fall it was in. This is deliberate —
    // a teleport is not automatically a stop — and it is exactly why the flag
    // has to exist: a player put back at a checkpoint with the speed that killed
    // them arrives already falling to their death again.
    CHECK(phys.getVelocity(static_cast<uint32_t>(keeper)).y < -10.0f);
    CHECK(phys.getVelocity(static_cast<uint32_t>(stopper)).y == doctest::Approx(0.0f));

    // And it shows in the motion: after the same handful of steps the one that
    // kept its speed has dropped much further than the one that was stopped.
    for (int i = 0; i < 10; ++i)
        phys.step(world, kDt);

    const auto& kept    = world.registry().get<TransformComponent>(keeper);
    const auto& stopped = world.registry().get<TransformComponent>(stopper);
    CHECK(kept.position.y < stopped.position.y - 1.0f);
    CHECK(stopped.position.y > 49.0f);
}

TEST_CASE("PhysicsWorld: setPosition respawns a character and leaves its facing alone")
{
    HorizonWorld world;

    // The PlayerCharacter shape exactly as EntityHost::defaultComponents builds
    // it: a character controller AND a kinematic rigid body, the body being the
    // collision proxy everything else sees. This is what a respawn actually
    // operates on — the audit's missing respawn is about the PLAYER, and the
    // player is not a crate. The character branch of setPosition (SetPosition +
    // RefreshContacts, and character-wins precedence when an entity has both)
    // has no other coverage.
    Entity player = world.createEntity("Player");
    {
        TransformComponent t;
        t.position = { 0.0f, 20.0f, 0.0f };
        t.rotation = { 0.0f, 45.0f, 0.0f };   // facing somewhere specific
        t.scale    = { 1.0f, 1.0f, 1.0f };
        world.addComponent(player, t);
        CharacterControllerComponent cc;
        world.addComponent(player, cc);
        RigidBodyComponent rb; rb.type = RigidBodyType::Kinematic;
        world.addComponent(player, rb);
    }

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(player)));

    // Fall a while, the way a player dies.
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    REQUIRE(world.registry().get<TransformComponent>(player).position.y < 15.0f);

    const glm::vec3 checkpoint{ -7.0f, 60.0f, 12.0f };
    CHECK(phys.setPosition(static_cast<uint32_t>(player), checkpoint, true));

    {
        const auto& tr = world.registry().get<TransformComponent>(player);
        CHECK(tr.position.x == doctest::Approx(checkpoint.x));
        CHECK(tr.position.y == doctest::Approx(checkpoint.y));
        CHECK(tr.position.z == doctest::Approx(checkpoint.z));
    }

    phys.step(world, kDt);

    // The character owns the transform, so the write-back must agree with the
    // teleport rather than undo it. One step of character gravity moves it a
    // couple of millimetres, hence the tolerance rather than exact equality.
    const auto& tr = world.registry().get<TransformComponent>(player);
    CHECK(tr.position.y > checkpoint.y - 0.5f);
    CHECK(tr.position.x == doctest::Approx(checkpoint.x).epsilon(0.02));
    CHECK(tr.position.z == doctest::Approx(checkpoint.z).epsilon(0.02));

    // ROTATION IS NOT TOUCHED — the header's promise, and it matters here more
    // than anywhere: a CharacterVirtual's Jolt rotation is never written by
    // anything, so reading it back and applying it would spin a respawning
    // player round to the direction they faced when the level started.
    CHECK(tr.rotation.y == doctest::Approx(45.0f).epsilon(0.01));
}

TEST_CASE("PhysicsWorld: setPosition refuses an entity with no physics")
{
    HorizonWorld world;
    Entity bare = world.createEntity("NoBody");
    { TransformComponent t; t.position = { 1.0f, 1.0f, 1.0f }; world.addComponent(bare, t); }

    PhysicsWorld phys;
    phys.initialize(world);

    // A refusal, not a silent no-op, and it must not half-apply: the transform
    // is untouched, because a caller that gets `false` needs the plain
    // transform write and would otherwise be looking at a moved entity that
    // physics disagrees with.
    CHECK_FALSE(phys.setPosition(static_cast<uint32_t>(bare), { 9.0f, 9.0f, 9.0f }));
    CHECK(world.registry().get<TransformComponent>(bare).position
          == glm::vec3(1.0f, 1.0f, 1.0f));

    CHECK_FALSE(phys.setPosition(99999u, { 0.0f, 0.0f, 0.0f }));
}

TEST_CASE("PhysicsWorld: addEntityTree reaches child entities, addEntity does not")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);

    // A spawn is a subtree, not an entity: a PlayerCharacter prefab brings child
    // entities whose colliders are as much part of "the thing that spawned" as
    // the root's.
    Entity root  = makeDynamicBox(world, "PrefabRoot",  { 0.0f, 10.0f, 0.0f });
    Entity child = makeDynamicBox(world, "PrefabChild", { 2.0f, 10.0f, 0.0f });
    REQUIRE(world.reparentEntity(child, root));

    // The single-entity call is honest about its scope: it builds the root and
    // nothing else. That is what makes the tree variant necessary rather than
    // decorative — wiring a spawn to addEntity alone would give a prefab a
    // collider on its root and nothing on its arms.
    CHECK(phys.addEntity(world, static_cast<uint32_t>(root)));
    CHECK(phys.hasPhysics(static_cast<uint32_t>(root)));
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(child)));

    // BEFORE THE CHANGE: neither existed, and a spawned prefab was bodiless from
    // root to leaf.
    CHECK(phys.addEntityTree(world, static_cast<uint32_t>(root)) == 2);
    CHECK(phys.hasPhysics(static_cast<uint32_t>(root)));
    CHECK(phys.hasPhysics(static_cast<uint32_t>(child)));

    // Both simulate, not just the one that was named.
    //
    // The child is asked in WORLD space on purpose. Its TransformComponent is a
    // LOCAL offset from the root, and both bodies fall at the same rate, so that
    // offset stays ~(2,10,0) for the whole two seconds no matter how far they
    // drop — a local-space assertion here would be testing that gravity does
    // nothing. The root is top-level, where local IS world, so it reads directly.
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    CHECK(world.registry().get<TransformComponent>(root).position.y < 5.0f);
    CHECK(HE::worldPositionOf(world, child).y < 5.0f);

    // And the whole subtree comes back out again.
    CHECK(phys.removeEntityTree(world, static_cast<uint32_t>(root)) == 2);
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(root)));
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(child)));
}

TEST_CASE("PhysicsWorld: adding the same entity twice replaces, never duplicates")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);

    Entity wall = makeStaticBox(world, "Wall", { 0.0f, 0.0f, 0.0f });

    CHECK(phys.addEntity(world, static_cast<uint32_t>(wall)));
    CHECK(phys.addEntity(world, static_cast<uint32_t>(wall)));

    const glm::vec3 from{ 0.0f, 10.0f, 0.0f };
    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    REQUIRE(phys.raycast(from, down, 100.0f).hit);

    // ONE removal is the whole test. The entity→body map holds a single BodyID,
    // so a second CreateAndAddBody that merely overwrote the entry would leak
    // the first body: unreachable by id, undestroyable, and still solid. After
    // removing once the ray has to pass through — if it still hits, the leaked
    // body is what it is hitting.
    phys.removeEntity(static_cast<uint32_t>(wall));
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(wall)));
    CHECK_FALSE(phys.raycast(from, down, 100.0f).hit);
}

TEST_CASE("PhysicsWorld: addEntity rebuilds a body after its collider changed")
{
    HorizonWorld world;
    Entity ball = world.createEntity("Ball");
    {
        TransformComponent t; t.position = { 0.0f, 0.0f, 0.0f }; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(ball, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Static;
        world.addComponent(ball, rb);
        ColliderComponent col; col.shape = ColliderShape::Sphere; col.radius = 0.5f;
        world.addComponent(ball, col);
    }

    PhysicsWorld phys;
    phys.initialize(world);

    // A ray 3 m off-axis misses a half-metre sphere.
    const glm::vec3 offAxis{ 3.0f, 10.0f, 0.0f };
    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    CHECK_FALSE(phys.raycast(offAxis, down, 100.0f).hit);

    // Grow the collider the way the inspector would, then ask for a rebuild.
    // addEntity being idempotent is what makes it usable as "this entity's
    // components changed" — and it is the same call the terrain tick uses after
    // a sculpt stroke.
    world.registry().get<ColliderComponent>(ball).halfExtents = { 5.0f, 0.5f, 5.0f };
    world.registry().get<ColliderComponent>(ball).shape       = ColliderShape::Box;
    CHECK(phys.addEntity(world, static_cast<uint32_t>(ball)));

    auto hit = phys.raycast(offAxis, down, 100.0f);
    CHECK(hit.hit);
    CHECK(hit.entityId == static_cast<uint32_t>(ball));
}

// ─── Geometry colliders: Mesh and Convex Hull ─────────────────────────────────
//
// The case the readiness audit named: an imported glTF house that collides as a
// crate. Every test below is built so that a BOUNDING BOX gives a measurably
// different answer from the real geometry — a mesh collider that quietly fell
// back to a box has to fail here, not pass with a shrug.
//
// Each test names the source mutation that was applied, built and observed to
// turn it red, so that "it passes" means something. The mutations, by the label
// the tests refer to:
//
//   MUT-BOX      buildColliderShape's Mesh/ConvexHull branch builds authoredBox()
//                instead of buildMeshShape/buildConvexHullShape.
//   MUT-STATIC   the `build.mustBeStatic && motionType != Static` downgrade in
//                buildBodyFor is disabled.
//   MUT-NOMESH   the "no mesh to build from" branch returns a fixed 0.5 m cube
//                instead of authoredBox().
//   MUT-LOCAL    buildBodyFor uses the entity's LOCAL TransformComponent as its
//                pose instead of worldPoseOf() — the blocker, restored.

namespace
{
    // A solid triangular prism: the ground runs from x=-2 to x=+2 at y=0 and the
    // roof slopes from (-2, 0) up to (+2, 4), extruded from z=-2 to z=+2.
    //
    // So the surface height IS `x + 2`, everywhere, while the bounding box is
    // flat-topped at y=4. At x=-1.5 that is 0.5 against 4.0 — the whole point of
    // the shape, and the reason no assertion below needs a tolerance argument to
    // tell the two apart.
    StaticMeshAsset wedgeHouseMesh()
    {
        StaticMeshAsset m;
        m.type = HE::AssetType::StaticMesh;
        m.name = "WedgeHouse";
        m.path = "mem://wedge_house";
        m.vertices = {
            -2.0f, 0.0f, -2.0f,   // 0  A
             2.0f, 0.0f, -2.0f,   // 1  B
             2.0f, 4.0f, -2.0f,   // 2  C
            -2.0f, 0.0f,  2.0f,   // 3  D
             2.0f, 0.0f,  2.0f,   // 4  E
             2.0f, 4.0f,  2.0f,   // 5  F
        };
        // Wound counter-clockwise seen from OUTSIDE, on every face. Jolt's mesh
        // triangles are single sided, so the mirror of this list is a house rays
        // fall straight through — which would make these tests fail for a reason
        // that has nothing to do with what they are checking.
        m.indices = {
            0, 2, 1,   3, 4, 5,   // the triangular ends  (-Z, +Z)
            0, 1, 4,   0, 4, 3,   // the floor            (-Y)
            1, 2, 5,   1, 5, 4,   // the tall wall        (+X)
            0, 5, 2,   0, 3, 5,   // the sloping roof
        };
        return m;
    }

    // The height the wedge's roof stands at, for a given x.
    float wedgeRoofY(float x) { return x + 2.0f; }

    // A UV sphere of radius 1. Every one of its vertices lies ON the sphere, so
    // every one of them is a hull vertex — which is what makes this exceed
    // Jolt's 256-point limit for real instead of just being a big mesh.
    StaticMeshAsset denseSphereMesh(uint32_t stacks = 16, uint32_t slices = 24)
    {
        StaticMeshAsset m;
        m.type = HE::AssetType::StaticMesh;
        m.name = "DenseSphere";
        m.path = "mem://dense_sphere";
        for (uint32_t i = 0; i <= stacks; ++i)
        {
            const float theta = 3.14159265f * static_cast<float>(i) / static_cast<float>(stacks);
            for (uint32_t j = 0; j < slices; ++j)
            {
                const float phi = 6.28318531f * static_cast<float>(j) / static_cast<float>(slices);
                m.vertices.push_back(std::sin(theta) * std::cos(phi));
                m.vertices.push_back(std::cos(theta));
                m.vertices.push_back(std::sin(theta) * std::sin(phi));
            }
        }
        return m;
    }

    // The entity an importer produces: a transform, a static body, a mesh asset
    // and a collider that says "use that mesh".
    Entity makeMeshCollider(HorizonWorld& world, const char* name, HE::UUID meshId,
                            ColliderShape shape, RigidBodyType type,
                            const glm::vec3& pos, const glm::vec3& scale = { 1.0f, 1.0f, 1.0f })
    {
        Entity e = world.createEntity(name);
        TransformComponent t; t.position = pos; t.scale = scale;
        world.addComponent(e, t);
        RigidBodyComponent rb; rb.type = type; rb.mass = 1.0f;
        world.addComponent(e, rb);
        MeshComponent mc; mc.meshAssetId = meshId;
        world.addComponent(e, mc);
        ColliderComponent col; col.shape = shape;
        world.addComponent(e, col);
        return e;
    }
}

// Red under MUT-BOX: every raycast below stops hitting anything at all,
// because a 0.5 m authored box is nowhere near where the roof was.
TEST_CASE("PhysicsWorld: a Mesh collider is the imported geometry, not its bounding box")
{
    HorizonWorld   world;
    ContentManager cm;
    const HE::UUID meshId = cm.registerStaticMesh(wedgeHouseMesh());

    Entity house = makeMeshCollider(world, "House", meshId,
                                    ColliderShape::Mesh, RigidBodyType::Static,
                                    { 0.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.setContentManager(&cm);   // without it Mesh has nowhere to get triangles
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(house)));

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };

    // BEFORE THE CHANGE: ColliderShape had no Mesh at all and this entity was a
    // 1 m crate. The low end of the roof is the assertion that separates the two
    // — 0.5 m on the real house, 4.0 m on any box drawn around it.
    const auto low = phys.raycast({ -1.5f, 50.0f, 0.0f }, down, 100.0f);
    REQUIRE(low.hit);
    CHECK(low.entityId == static_cast<uint32_t>(house));
    CHECK(low.point.y == doctest::Approx(wedgeRoofY(-1.5f)).epsilon(0.02));
    CHECK(low.point.y < 1.0f);   // a bounding box would have answered 4.0

    // The far end of the same roof, 3 m higher. A box is flat: it answers the
    // same height at both ends, so the DIFFERENCE is the shape itself.
    const auto high = phys.raycast({ 1.5f, 50.0f, 0.0f }, down, 100.0f);
    REQUIRE(high.hit);
    CHECK(high.point.y == doctest::Approx(wedgeRoofY(1.5f)).epsilon(0.02));
    CHECK(high.point.y - low.point.y == doctest::Approx(3.0f).epsilon(0.05));

    // And the surface leans. No face of an axis-aligned box has this normal, so
    // it cannot be produced by any fallback in the switch.
    CHECK(low.normal.x == doctest::Approx(-0.7071f).epsilon(0.02));
    CHECK(low.normal.y == doctest::Approx( 0.7071f).epsilon(0.02));
}

// Red under MUT-BOX.
TEST_CASE("PhysicsWorld: a Mesh collider is built at the entity's composed world scale")
{
    // A house dropped into a zone that is scaled up is DRAWN at the composed
    // scale, so that is the size its collision has to be. Scale is applied to the
    // vertices as they are handed to Jolt, which is why this is worth pinning
    // separately from the shape itself.
    HorizonWorld   world;
    ContentManager cm;
    const HE::UUID meshId = cm.registerStaticMesh(wedgeHouseMesh());

    Entity house = makeMeshCollider(world, "BigHouse", meshId,
                                    ColliderShape::Mesh, RigidBodyType::Static,
                                    { 0.0f, 0.0f, 0.0f }, { 3.0f, 3.0f, 3.0f });

    PhysicsWorld phys;
    phys.setContentManager(&cm);
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(house)));

    // Everything triples: the roof at x=-4.5 is the unscaled roof at x=-1.5.
    const auto hit = phys.raycast({ -4.5f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
    REQUIRE(hit.hit);
    CHECK(hit.point.y == doctest::Approx(wedgeRoofY(-1.5f) * 3.0f).epsilon(0.02));

    // The unscaled house ended at x=2; the scaled one reaches x=6, so a ray that
    // used to miss now lands on the far wall's roof edge.
    const auto beyond = phys.raycast({ 5.0f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
    CHECK(beyond.hit);
}

// Red under MUT-BOX.
TEST_CASE("PhysicsWorld: a Convex Hull is the mesh's hull, and unlike Mesh it can move")
{
    HorizonWorld   world;
    ContentManager cm;
    const HE::UUID meshId = cm.registerStaticMesh(wedgeHouseMesh());

    // The wedge is already convex, so its hull IS the wedge — the same sloping
    // roof the Mesh case measures, reached through the other branch.
    Entity ramp = makeMeshCollider(world, "Ramp", meshId,
                                   ColliderShape::ConvexHull, RigidBodyType::Static,
                                   { 0.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.setContentManager(&cm);
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(ramp)));

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    const auto low = phys.raycast({ -1.5f, 50.0f, 0.0f }, down, 100.0f);
    REQUIRE(low.hit);
    CHECK(low.point.y == doctest::Approx(wedgeRoofY(-1.5f)).epsilon(0.02));
    CHECK(low.point.y < 1.0f);   // not the bounding box

    // The reason Convex Hull exists next to Mesh: a hull is a solid, so Jolt can
    // give it mass and it is allowed to stay Dynamic. A crate, a rock, debris.
    Entity boulder = makeMeshCollider(world, "Boulder", meshId,
                                      ColliderShape::ConvexHull, RigidBodyType::Dynamic,
                                      { 0.0f, 30.0f, 0.0f });
    REQUIRE(phys.addEntity(world, static_cast<uint32_t>(boulder)));
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    // It fell — it was NOT downgraded to static the way a triangle mesh is.
    CHECK(world.registry().get<TransformComponent>(boulder).position.y < 25.0f);
}

// Red under MUT-BOX. NOT red under a truncated tolerance ladder — see the
// comment inside; that is measured, not assumed.
TEST_CASE("PhysicsWorld: a Convex Hull too detailed for Jolt is coarsened, not abandoned")
{
    HorizonWorld   world;
    ContentManager cm;

    const StaticMeshAsset sphere = denseSphereMesh();
    // Every vertex lies ON the sphere, so every one of them is a hull vertex and
    // the count really is past Jolt's 256-point ceiling — not merely a big mesh
    // whose hull happens to be small.
    REQUIRE(sphere.vertices.size() / 3 > 256u);
    const HE::UUID meshId = cm.registerStaticMesh(sphere);

    Entity ball = makeMeshCollider(world, "DetailedRock", meshId,
                                   ColliderShape::ConvexHull, RigidBodyType::Static,
                                   { 0.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.setContentManager(&cm);
    phys.initialize(world);

    // What this pins is the OUTCOME, not the mechanism: a mesh past the ceiling
    // still ends up with a rounded hull rather than the bounding box that every
    // failing branch in buildColliderShape falls back to.
    //
    // Deliberately NOT a test of the tolerance ladder above it, and the ladder is
    // not what rescues this case. Measured, not assumed: truncating the ladder to
    // its first rung leaves every assertion here green, because
    // ConvexHullShape.cpp:61 accepts ConvexHullBuilder::EResult::MaxVerticesReached
    // as SUCCESS — the builder stops adding points and returns the hull it has.
    // The ladder's own comment says Jolt "reports that as an ERROR rather than
    // simplifying", and against the vendored Jolt that is not true. Writing an
    // assertion here that only the ladder could satisfy would mean writing one
    // that passes for a reason the test does not name.
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(ball)));

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    const auto top  = phys.raycast({ 0.0f, 10.0f, 0.0f }, down, 20.0f);
    const auto side = phys.raycast({ 0.7f, 10.0f, 0.0f }, down, 20.0f);
    REQUIRE(top.hit);
    REQUIRE(side.hit);
    CHECK(top.entityId == static_cast<uint32_t>(ball));

    // The discriminator is CURVATURE, which survives however coarse the hull came
    // out: a box fallback is flat and answers the same height at x=0 and x=0.7,
    // a hull of a sphere drops away. Deliberately loose about the exact figure —
    // how many points Jolt keeps is its call, and pinning that would make this
    // test fail on a Jolt upgrade that is not a regression.
    CHECK(top.point.y == doctest::Approx(1.0f).epsilon(0.3));
    CHECK(side.point.y < top.point.y - 0.15f);
}

// Red under MUT-STATIC (the house falls instead of holding still) and under
// MUT-BOX (it holds still, but at the wrong shape).
TEST_CASE("PhysicsWorld: a Mesh collider on a dynamic body is downgraded, never dropped")
{
    HorizonWorld   world;
    ContentManager cm;
    const HE::UUID meshId = cm.registerStaticMesh(wedgeHouseMesh());

    // The mistake an author makes once: a triangle mesh on something that is
    // supposed to move. Jolt has no solver state for a surface, so it cannot be
    // Dynamic — but the answer has to be a body that is the right SHAPE and does
    // not move, not an entity with no collision at all.
    Entity house = makeMeshCollider(world, "MovingHouse", meshId,
                                    ColliderShape::Mesh, RigidBodyType::Dynamic,
                                    { 0.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.setContentManager(&cm);
    phys.initialize(world);

    // Not bodiless. This is the half of the contract that "fall back to something
    // usable" is about.
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(house)));

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    // Forced to Static: it did not fall, and it did not integrate garbage either.
    CHECK(world.registry().get<TransformComponent>(house).position.y
          == doctest::Approx(0.0f).epsilon(0.01));

    // And it kept the real geometry while being downgraded — the downgrade is
    // about the MOTION type, not about giving up on the shape.
    const auto low = phys.raycast({ -1.5f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
    REQUIRE(low.hit);
    CHECK(low.entityId == static_cast<uint32_t>(house));
    CHECK(low.point.y == doctest::Approx(wedgeRoofY(-1.5f)).epsilon(0.02));
}

// Red under MUT-NOMESH: the collider comes back as a default cube and both the
// height and the 1.4 m edge probe disagree.
TEST_CASE("PhysicsWorld: a Mesh collider with no mesh to build from falls back to a box")
{
    // The other failure an author can produce: the shape is set to Mesh but the
    // entity has no mesh asset (or physics was never given a ContentManager, which
    // is the packaged-build version of the same thing). A box is a lie about the
    // shape; NO body is a hole in the floor. The engine picks the lie and logs it.
    HorizonWorld world;

    Entity e = world.createEntity("MeshlessHouse");
    TransformComponent t; t.position = { 0.0f, 0.0f, 0.0f }; t.scale = { 1.0f, 1.0f, 1.0f };
    world.addComponent(e, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Static;
    world.addComponent(e, rb);
    ColliderComponent col; col.shape = ColliderShape::Mesh;
    col.halfExtents = { 1.5f, 0.25f, 1.5f };   // what the inspector still shows
    world.addComponent(e, col);

    PhysicsWorld phys;   // no setContentManager on purpose
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(e)));

    // The AUTHORED half extents, so the fallback is at least the size the user
    // drew rather than a default cube.
    const auto hit = phys.raycast({ 0.0f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
    REQUIRE(hit.hit);
    CHECK(hit.point.y == doctest::Approx(0.25f).epsilon(0.02));
    CHECK(phys.raycast({ 1.4f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f).hit);
    CHECK_FALSE(phys.raycast({ 1.6f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f).hit);
}

// ─── Hierarchy: a collider belongs where the mesh is drawn ────────────────────
//
// The blocker. PhysicsWorld built every pose out of the LOCAL TransformComponent
// and treated it as a world pose, while the renderer and NavigationSystem
// composed the parent chain — so a nested entity was drawn in one place and
// collided in another. These two cases are the shapes that bug takes in a real
// project: a prefab with child parts, and a zone streamed in at an offset.

// Red under MUT-LOCAL: the collider is built at the local offset, so the ray at
// the world position finds nothing.
TEST_CASE("PhysicsWorld: a prefab child's collider stands at its WORLD position")
{
    HorizonWorld world;

    // A prefab standing well away from the origin, with a part offset inside it.
    Entity root  = makeStaticBox(world, "PrefabRoot",  { 30.0f, 0.0f, -12.0f });
    Entity child = makeStaticBox(world, "PrefabChild", {  2.0f, 0.0f,   0.0f });
    REQUIRE(world.reparentEntity(child, root));

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(child)));

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };

    // Where the child is DRAWN: root + local offset.
    const auto atWorld = phys.raycast({ 32.0f, 50.0f, -12.0f }, down, 100.0f);
    REQUIRE(atWorld.hit);
    CHECK(atWorld.entityId == static_cast<uint32_t>(child));

    // BEFORE THE FIX the collider was built from the local transform and sat
    // HERE — 30 m from its mesh, in the middle of wherever the level's origin
    // happens to be. Nothing is at the local coordinates any more.
    CHECK_FALSE(phys.raycast({ 2.0f, 50.0f, 0.0f }, down, 100.0f).hit);

    // The root is top-level, where local IS world: unchanged by all of this, and
    // asserted so a fix that moved everything by the parent offset twice fails.
    const auto atRoot = phys.raycast({ 30.0f, 50.0f, -12.0f }, down, 100.0f);
    REQUIRE(atRoot.hit);
    CHECK(atRoot.entityId == static_cast<uint32_t>(root));
}

// Red under MUT-LOCAL, on both counts: the colliders sit at the authored origin
// and they are built at local scale, so the crate top is at 0.5 and not 1.0.
TEST_CASE("PhysicsWorld: an additively loaded zone gets its colliders where its meshes are")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);   // empty: addEntityTree below is the only way in

    // A zone file authors its contents around its OWN origin; the streamer then
    // puts the whole subtree down at the slot the level gives it and hands it to
    // physics. The scale is part of the case on purpose — a zone placed at half
    // or double size is how a level re-uses one.
    Entity zone = world.createEntity("Zone_Docks");
    TransformComponent zt;
    zt.position = { 100.0f, 0.0f, 200.0f };
    zt.scale    = { 2.0f, 2.0f, 2.0f };
    world.addComponent(zone, zt);

    Entity crate  = makeStaticBox(world, "Crate",  {  4.0f, 0.0f, 0.0f });
    Entity barrel = makeStaticBox(world, "Barrel", { -4.0f, 0.0f, 6.0f });
    REQUIRE(world.reparentEntity(crate,  zone));
    REQUIRE(world.reparentEntity(barrel, zone));

    // The zone root carries no rigid body of its own — it is a folder. Two
    // children get bodies, and the count says so.
    CHECK(phys.addEntityTree(world, static_cast<uint32_t>(zone)) == 2);

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };

    // World position = zone origin + zone scale * local offset.
    const auto onCrate = phys.raycast({ 108.0f, 50.0f, 200.0f }, down, 100.0f);
    REQUIRE(onCrate.hit);
    CHECK(onCrate.entityId == static_cast<uint32_t>(crate));

    const auto onBarrel = phys.raycast({ 92.0f, 50.0f, 212.0f }, down, 100.0f);
    REQUIRE(onBarrel.hit);
    CHECK(onBarrel.entityId == static_cast<uint32_t>(barrel));

    // The composed SCALE reaches the shape too: a 1 m box inside a zone scaled
    // by two is a 2 m box, so its top is at 1.0 and not at 0.5.
    CHECK(onCrate.point.y == doctest::Approx(1.0f).epsilon(0.02));

    // BEFORE THE FIX every collider in the zone sat at the authored origin,
    // 100 m away from the geometry the player can see.
    CHECK_FALSE(phys.raycast({ 4.0f, 50.0f, 0.0f }, down, 100.0f).hit);
    CHECK_FALSE(phys.raycast({ -4.0f, 50.0f, 6.0f }, down, 100.0f).hit);
}

// ─── Jumping ──────────────────────────────────────────────────────────────────

namespace
{
    // A character standing on solid ground, settled. Everything about a jump is
    // a question about the frame it is asked in, so every case below needs the
    // same starting point: feet down, vertical velocity gone, isGrounded true.
    struct StandingCharacter
    {
        Entity floor;
        Entity character;
        uint32_t id;
    };

    StandingCharacter makeStandingCharacter(HorizonWorld& world, PhysicsWorld& phys)
    {
        Entity floor = world.createEntity("Floor");
        {
            TransformComponent t;
            t.position = { 0.0f, 0.0f, 0.0f };
            t.scale    = { 40.0f, 0.5f, 40.0f };
            world.addComponent(floor, t);
            RigidBodyComponent rb; rb.type = RigidBodyType::Static;
            world.addComponent(floor, rb);
        }

        // The PlayerCharacter shape as EntityHost::defaultComponents builds it:
        // the CharacterVirtual that walks, plus the kinematic body everything
        // else collides with.
        Entity character = world.createEntity("Jumper");
        {
            TransformComponent t;
            t.position = { 0.0f, 4.0f, 0.0f };
            t.scale    = { 1.0f, 1.0f, 1.0f };
            world.addComponent(character, t);
            world.addComponent(character, CharacterControllerComponent{});
            RigidBodyComponent rb; rb.type = RigidBodyType::Kinematic;
            world.addComponent(character, rb);
        }

        phys.initialize(world);
        // Long enough to fall the four metres and for the ground contact to
        // settle — a character that is still resolving its landing has a
        // non-zero downward velocity and would make the rise below ambiguous.
        for (int i = 0; i < kSteps2s; ++i)
            phys.step(world, kDt);

        return { floor, character, static_cast<uint32_t>(character) };
    }
}

// MUTATION: in PhysicsWorld::jumpCharacter, invert the ground gate to
// `if (cc->isGrounded && cc->airTime < kCoyoteWindow) return false;` — the
// grounded jump is refused and the CHECK on the rise fails.
TEST_CASE("PhysicsWorld: a character on the ground rises after a jump")
{
    HorizonWorld world;
    PhysicsWorld phys;
    const auto c = makeStandingCharacter(world, phys);

    const auto& cc = world.registry().get<CharacterControllerComponent>(c.character);
    REQUIRE(cc.isGrounded);
    REQUIRE(cc.airTime == doctest::Approx(0.0f));

    const float restY = world.registry().get<TransformComponent>(c.character).position.y;

    // The return value is the whole point of the row: a script writes
    // `if (jump()) playSound()`, so a jump that happened must say so.
    CHECK(phys.jumpCharacter(c.id));

    // Rise for a quarter second — well short of the apex of a 5 m/s jump, so
    // this is unambiguously the way up.
    for (int i = 0; i < 15; ++i)
        phys.step(world, kDt);

    const float peakY = world.registry().get<TransformComponent>(c.character).position.y;
    CHECK(peakY > restY + 0.3f);

    // And it comes back down: a jump that left the character floating would
    // pass the check above just as well.
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    const auto& landed = world.registry().get<CharacterControllerComponent>(c.character);
    CHECK(landed.isGrounded);
    CHECK(landed.airTime == doctest::Approx(0.0f));
    CHECK(world.registry().get<TransformComponent>(c.character).position.y
          == doctest::Approx(restY).epsilon(0.05));
}

// MUTATION: in PhysicsWorld::jumpCharacter, drop the airborne gate entirely
// (`if (false) return false;`) — the second jump is granted and both the
// CHECK_FALSE and the "no second rise" check fail.
TEST_CASE("PhysicsWorld: a character in the air is refused a jump, and says so")
{
    HorizonWorld world;
    PhysicsWorld phys;
    const auto c = makeStandingCharacter(world, phys);

    REQUIRE(phys.jumpCharacter(c.id));

    // Immediately: the jump spent the coyote credit, so holding the button
    // cannot turn the grace period into a second jump.
    CHECK_FALSE(phys.jumpCharacter(c.id));

    // And properly airborne, a good way into the arc.
    for (int i = 0; i < 15; ++i)
        phys.step(world, kDt);
    const auto& cc = world.registry().get<CharacterControllerComponent>(c.character);
    REQUIRE_FALSE(cc.isGrounded);
    REQUIRE(cc.airTime > 0.12f);

    const float beforeY = world.registry().get<TransformComponent>(c.character).position.y;
    const float beforeVy = cc.velocity.y;
    CHECK_FALSE(phys.jumpCharacter(c.id));
    // A refusal is not a partial jump: nothing was written.
    CHECK(world.registry().get<CharacterControllerComponent>(c.character).velocity.y
          == doctest::Approx(beforeVy));
    CHECK(world.registry().get<TransformComponent>(c.character).position.y
          == doctest::Approx(beforeY));
}

// MUTATION: set kCoyoteWindow to 0.0f — the grace vanishes and the jump below is
// refused. Without this case the constant could be zeroed and every other test
// would stay green: the three cases around it only ever prove that a jump is
// REFUSED, so they pass with the grace switched off.
//
// The scene is the one the grace exists for, built without any level geometry:
// the ground is taken away, so the character is airborne having never jumped —
// exactly the state of someone who has just walked off a ledge. One step at
// 1/60 s puts airTime well inside the 0.12 s window.
TEST_CASE("PhysicsWorld: a character just off the ground still gets its jump")
{
    HorizonWorld world;
    PhysicsWorld phys;
    const auto c = makeStandingCharacter(world, phys);
    REQUIRE(world.registry().get<CharacterControllerComponent>(c.character).isGrounded);

    // The ledge, removed rather than walked off: same resulting state, no level
    // geometry to build. (removeEntity is what B1 added; before it there was no
    // way to take a body out of a running world at all.)
    phys.removeEntity(static_cast<uint32_t>(c.floor));
    phys.step(world, kDt);

    const auto& cc = world.registry().get<CharacterControllerComponent>(c.character);
    REQUIRE_FALSE(cc.isGrounded);          // genuinely in the air …
    REQUIRE(cc.airTime > 0.0f);            // … having never jumped …
    REQUIRE(cc.airTime < 0.12f);           // … and still inside the window.

    const float beforeVy = cc.velocity.y;
    CHECK(phys.jumpCharacter(c.id));       // the grace: granted.
    CHECK(world.registry().get<CharacterControllerComponent>(c.character).velocity.y
          > beforeVy);

    // And it is spent, not standing: the same grace cannot pay for a second jump.
    CHECK_FALSE(phys.jumpCharacter(c.id));
}

// MUTATION: in PhysicsWorld::jumpCharacter, delete the `cc->velocity = {...}`
// mirror write — Jolt still gets the upward velocity, but the MovementSystem
// rebuild below hands back the stale pre-jump Y and erases it before it is ever
// stepped. The rise check fails.
//
// This is the case the jump would quietly lose without the mirror, and it is not
// exotic: MovementSystem rebuilds the character's velocity as
// (planar.x, cc.velocity.y, planar.z) on EVERY tick, which is what the two lines
// marked "as MovementSystem does" reproduce.
TEST_CASE("PhysicsWorld: a jump survives the next physics step")
{
    HorizonWorld world;
    PhysicsWorld phys;
    const auto c = makeStandingCharacter(world, phys);

    auto& cc = world.registry().get<CharacterControllerComponent>(c.character);
    REQUIRE(cc.isGrounded);
    const float restY = world.registry().get<TransformComponent>(c.character).position.y;

    REQUIRE(phys.jumpCharacter(c.id));

    // The component half of the jump, which is the half MovementSystem reads.
    CHECK(cc.velocity.y > 0.0f);
    // isGrounded goes false in the same call, so a state machine reading it this
    // frame already sees the jump rather than a frame of "standing".
    CHECK_FALSE(cc.isGrounded);

    // …as MovementSystem does, before the very next step: walking must not erase
    // the fall, so it rebuilds the velocity from the component's Y every tick.
    phys.setCharacterVelocity(c.id, glm::vec3(0.0f, cc.velocity.y, 0.0f));
    phys.step(world, kDt);

    CHECK(world.registry().get<TransformComponent>(c.character).position.y > restY + 0.02f);

    // Ten more ticks of the same round trip: the jump keeps climbing rather than
    // being flattened by the rebuild.
    for (int i = 0; i < 10; ++i)
    {
        const auto& live = world.registry().get<CharacterControllerComponent>(c.character);
        phys.setCharacterVelocity(c.id, glm::vec3(0.0f, live.velocity.y, 0.0f));
        phys.step(world, kDt);
    }
    CHECK(world.registry().get<TransformComponent>(c.character).position.y > restY + 0.3f);
}

// MUTATION: in PhysicsWorld::jumpCharacter(uint32_t, float), remove the
// `if (!(speed > 0.0f))` guard — a zero-speed jump reports success.
TEST_CASE("PhysicsWorld: jumpWith overrides the authored speed, and refuses a useless one")
{
    HorizonWorld world;
    PhysicsWorld phys;
    const auto c = makeStandingCharacter(world, phys);

    const float restY = world.registry().get<TransformComponent>(c.character).position.y;

    // A speed of zero is not a jump, and reporting success for it would send a
    // script off playing a jump sound for a character that never left the floor.
    CHECK_FALSE(phys.jumpCharacter(c.id, 0.0f));
    CHECK_FALSE(phys.jumpCharacter(c.id, -3.0f));
    CHECK(world.registry().get<CharacterControllerComponent>(c.character).isGrounded);

    // A low hop through a gap: the component says 5 m/s, this call says 2, and
    // the arc has to be visibly shorter than the authored one.
    auto peakAfter = [&](float speed) {
        REQUIRE(phys.jumpCharacter(c.id, speed));
        float peak = restY;
        for (int i = 0; i < 60; ++i)
        {
            phys.step(world, kDt);
            peak = std::max(peak, world.registry().get<TransformComponent>(c.character).position.y);
        }
        // Back to standing before the next measurement.
        for (int i = 0; i < kSteps2s; ++i)
            phys.step(world, kDt);
        REQUIRE(world.registry().get<CharacterControllerComponent>(c.character).isGrounded);
        return peak - restY;
    };

    const float lowHop  = peakAfter(2.0f);
    const float highHop = peakAfter(8.0f);
    CHECK(lowHop  > 0.05f);
    CHECK(highHop > lowHop + 0.5f);
}

// ─── Collision layers ─────────────────────────────────────────────────────────
// The matrix decides which of the sixteen named channels may touch which. What
// these guard is that (a) it actually separates bodies, (b) the default config
// is the simulation this class had before channels existed, and (c) nothing an
// author can type into the uint8_t can reach Jolt as an out-of-range layer.

namespace
{
    // A dynamic box dropped over a static floor, both in named channels. Returns
    // the height the box ends up at after two seconds: resting on the floor
    // (≈ 1) or somewhere below it (a fall through nothing).
    struct LayerDrop
    {
        HorizonWorld world;
        Entity       floor{};
        Entity       box{};
        PhysicsWorld phys;

        LayerDrop(uint8_t floorLayer, uint8_t boxLayer)
        {
            floor = world.createEntity("Floor");
            {
                TransformComponent t;
                t.position = { 0.0f, 0.0f, 0.0f };
                t.scale    = { 40.0f, 0.5f, 40.0f };
                world.addComponent(floor, t);
                RigidBodyComponent rb;
                rb.type           = RigidBodyType::Static;
                rb.collisionLayer = floorLayer;
                world.addComponent(floor, rb);
            }
            box = world.createEntity("Box");
            {
                TransformComponent t;
                t.position = { 0.0f, 5.0f, 0.0f };
                t.scale    = { 1.0f, 1.0f, 1.0f };
                world.addComponent(box, t);
                RigidBodyComponent rb;
                rb.type           = RigidBodyType::Dynamic;
                rb.mass           = 1.0f;
                rb.collisionLayer = boxLayer;
                world.addComponent(box, rb);
            }
        }

        float settleY(int steps = kSteps2s)
        {
            for (int i = 0; i < steps; ++i)
                phys.step(world, kDt);
            return world.registry().get<TransformComponent>(box).position.y;
        }
    };

    constexpr uint8_t kChanA = 5;   // two channels nothing else uses, so a test
    constexpr uint8_t kChanB = 6;   // cannot be confused by a preset's meaning
}

TEST_CASE("CollisionLayerConfig: the default is everything collides")
{
    HE::CollisionLayerConfig cfg;
    CHECK(cfg.isDefault());
    for (int a = 0; a < HE::CollisionLayerConfig::kCount; ++a)
        for (int b = 0; b < HE::CollisionLayerConfig::kCount; ++b)
            CHECK(cfg.collides(a, b));

    // The presets are named, everything past them is not.
    CHECK(cfg.layerName(HE::CollisionLayerConfig::kDefault)   == "Default");
    CHECK(cfg.layerName(HE::CollisionLayerConfig::kTerrain)   == "Terrain");
    CHECK(cfg.layerName(HE::CollisionLayerConfig::kCharacter) == "Character");
    CHECK(cfg.layerName(9)  == "Layer 9");
    CHECK(cfg.layerName(-1) == "Layer -1");    // out of range answers, never throws
    CHECK(cfg.layerName(99) == "Layer 99");
}

TEST_CASE("CollisionLayerConfig: a cell is written on both sides of the diagonal")
{
    HE::CollisionLayerConfig cfg;
    cfg.setCollides(kChanA, kChanB, false);

    // Jolt asks in whichever order the broadphase reached the two bodies in, so
    // a half-written matrix would let a pair collide on some frames only.
    CHECK_FALSE(cfg.collides(kChanA, kChanB));
    CHECK_FALSE(cfg.collides(kChanB, kChanA));
    CHECK_FALSE(cfg.isDefault());

    // Nothing else moved.
    CHECK(cfg.collides(kChanA, kChanA));
    CHECK(cfg.collides(kChanA, 0));

    // Out of range is a no-op, not a write past the array.
    cfg.setCollides(99, 0, false);
    cfg.setCollides(-3, 0, false);
    CHECK(cfg.collides(0, 0));
}

TEST_CASE("CollisionLayerConfig: json round trip keeps names and blocked pairs")
{
    HE::CollisionLayerConfig cfg;
    cfg.setLayerName(kChanA, "Bullets");
    cfg.setCollides(kChanA, kChanB, false);
    cfg.setCollides(0, 0, false);

    nlohmann::json j;
    cfg.toJson(j);

    HE::CollisionLayerConfig back;
    back.fromJson(j);
    CHECK(back.layerName(kChanA) == "Bullets");
    CHECK_FALSE(back.collides(kChanA, kChanB));
    CHECK_FALSE(back.collides(kChanB, kChanA));
    CHECK_FALSE(back.collides(0, 0));
    CHECK(back.collides(1, 2));

    // A block written by an older engine — or none at all — is the default, and
    // loading twice does not accumulate the first load's blocked pairs.
    back.fromJson(nlohmann::json::object());
    CHECK(back.isDefault());
    back.fromJson(nlohmann::json("not an object"));
    CHECK(back.isDefault());

    // A file that names only one half of a pair still loads symmetric: the
    // reader goes through the same setter the editor does.
    nlohmann::json half = { { "blocked", nlohmann::json::array(
        { nlohmann::json::array({ kChanB, kChanA }) }) } };
    back.fromJson(half);
    CHECK_FALSE(back.collides(kChanA, kChanB));
    CHECK_FALSE(back.collides(kChanB, kChanA));

    // A fresh config carries nothing but the presets, so it must not grow a
    // 136-entry blob in every .heproj.
    nlohmann::json empty;
    HE::CollisionLayerConfig{}.toJson(empty);
    CHECK(empty.empty());
}

// MUTATION: drop the matrix lookup from ObjectLayerPairFilterImpl::ShouldCollide
// (return true once the moving check has passed) — the box lands on the floor
// and this fails.
TEST_CASE("PhysicsWorld: a matrix cell set to false separates two bodies")
{
    // Same scene twice. The only difference is one cell.
    float restingY = 0.0f;
    {
        LayerDrop drop(kChanA, kChanB);
        drop.phys.initialize(drop.world);
        restingY = drop.settleY();
        // Sanity: with the default config this is the floor, at 0.25 + 0.5.
        CHECK(restingY > 0.0f);
    }
    {
        LayerDrop drop(kChanA, kChanB);
        HE::CollisionLayerConfig cfg;
        cfg.setCollides(kChanA, kChanB, false);
        drop.phys.setCollisionLayers(cfg);
        drop.phys.initialize(drop.world);
        // Two seconds of free fall from 5 m is about -14 m; anything at or above
        // the resting height means the floor is still stopping it.
        CHECK(drop.settleY() < restingY - 5.0f);
    }
}

TEST_CASE("PhysicsWorld: the matrix may be changed while the simulation runs")
{
    LayerDrop drop(kChanA, kChanB);
    drop.phys.initialize(drop.world);
    // Four seconds, not two: the box has to be ASLEEP, not merely resting. A
    // sleeping body is out of Jolt's active set, so nothing re-asks the
    // broadphase about it — and that is exactly the case an author hits when
    // they edit the matrix during play, with the scene already settled.
    const float resting = drop.settleY(4 * kSteps2s);
    REQUIRE(resting > 0.0f);          // it landed
    REQUIRE(drop.phys.getVelocity(static_cast<uint32_t>(drop.box)).y
            == doctest::Approx(0.0f).epsilon(0.01));

    // The floor is switched off underneath a body that is already asleep on it.
    // Bodies keep the object layer they were built with — only the answer to
    // "may these two touch" changes, and it has to change without a rebuild.
    HE::CollisionLayerConfig cfg;
    cfg.setCollides(kChanA, kChanB, false);
    drop.phys.setCollisionLayers(cfg);

    CHECK(drop.settleY() < resting - 3.0f);
}

TEST_CASE("PhysicsWorld: two channels that block each other leave a third alone")
{
    HorizonWorld world;
    Entity floor = world.createEntity("Floor");
    {
        TransformComponent t;
        t.position = { 0.0f, 0.0f, 0.0f };
        t.scale    = { 40.0f, 0.5f, 40.0f };
        world.addComponent(floor, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Static; rb.collisionLayer = kChanA;
        world.addComponent(floor, rb);
    }
    Entity ghost = world.createEntity("Ghost");     // blocked from the floor
    {
        TransformComponent t; t.position = { 0.0f, 5.0f, 0.0f }; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(ghost, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.collisionLayer = kChanB;
        world.addComponent(ghost, rb);
    }
    Entity solid = world.createEntity("Solid");     // Default — untouched
    {
        TransformComponent t; t.position = { 6.0f, 5.0f, 0.0f }; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(solid, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic;
        world.addComponent(solid, rb);
    }

    PhysicsWorld phys;
    HE::CollisionLayerConfig cfg;
    cfg.setCollides(kChanA, kChanB, false);
    phys.setCollisionLayers(cfg);
    phys.initialize(world);
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    CHECK(world.registry().get<TransformComponent>(ghost).position.y < -5.0f);
    CHECK(world.registry().get<TransformComponent>(solid).position.y >  0.0f);
}

// MUTATION: delete the `if (!isMoving(a) && !isMoving(b)) return false;` line
// from the pair filter — two overlapping statics start reporting contacts and
// this fails.
TEST_CASE("PhysicsWorld: static bodies still never collide with each other")
{
    HorizonWorld world;
    // Two statics in the SAME channel, overlapping outright. The matrix says
    // yes; the simulation must still say no, because that is what it always did
    // and because a pair of immovable bodies has no contact to resolve.
    makeStaticBox(world, "A", { 0.0f, 0.0f, 0.0f });
    makeStaticBox(world, "B", { 0.2f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    for (int i = 0; i < 30; ++i)
        phys.step(world, kDt);

    CHECK(phys.pollCollisionEnter().empty());
}

TEST_CASE("PhysicsWorld: an out-of-range collision layer falls back to Default")
{
    HorizonWorld world;
    Entity floor = world.createEntity("Floor");
    {
        TransformComponent t; t.position = { 0.0f, 0.0f, 0.0f }; t.scale = { 40.0f, 0.5f, 40.0f };
        world.addComponent(floor, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Static;
        rb.collisionLayer = 200;    // nothing validates this on the way in
        world.addComponent(floor, rb);
    }
    Entity box = world.createEntity("Box");
    {
        TransformComponent t; t.position = { 0.0f, 5.0f, 0.0f }; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(box, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic;
        rb.collisionLayer = 255;
        world.addComponent(box, rb);
    }

    PhysicsWorld phys;
    phys.initialize(world);
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    // Both bodies exist and behave as Default: the box is resting on the floor,
    // not falling through it and not tripping an assert inside Jolt.
    CHECK(world.registry().get<TransformComponent>(box).position.y > 0.0f);
}

// MUTATION: hand CharacterFilters the Default-moving layer instead of the
// character's own — the character keeps standing and this fails.
TEST_CASE("PhysicsWorld: a character walks in its own collision channel")
{
    const auto build = [](uint8_t floorLayer, uint8_t charLayer,
                          const HE::CollisionLayerConfig& cfg) {
        HorizonWorld world;
        Entity floor = world.createEntity("Floor");
        {
            TransformComponent t; t.position = { 0.0f, 0.0f, 0.0f };
            t.scale = { 40.0f, 0.5f, 40.0f };
            world.addComponent(floor, t);
            RigidBodyComponent rb; rb.type = RigidBodyType::Static;
            rb.collisionLayer = floorLayer;
            world.addComponent(floor, rb);
        }
        Entity character = world.createEntity("Walker");
        {
            TransformComponent t; t.position = { 0.0f, 4.0f, 0.0f };
            t.scale = { 1.0f, 1.0f, 1.0f };
            world.addComponent(character, t);
            CharacterControllerComponent cc;
            cc.collisionLayer = charLayer;
            world.addComponent(character, cc);
        }

        PhysicsWorld phys;
        phys.setCollisionLayers(cfg);
        phys.initialize(world);
        for (int i = 0; i < kSteps2s; ++i)
            phys.step(world, kDt);
        return world.registry().get<CharacterControllerComponent>(character).isGrounded;
    };

    // Default config: the character stands, exactly as before channels existed.
    CHECK(build(kChanA, kChanB, HE::CollisionLayerConfig{}));

    // The character's channel is switched off against the floor's. Its ground
    // check runs through the same matrix as everything else, so it falls.
    HE::CollisionLayerConfig blocked;
    blocked.setCollides(kChanA, kChanB, false);
    CHECK_FALSE(build(kChanA, kChanB, blocked));

    // …and a cell that names some OTHER pair leaves it standing.
    HE::CollisionLayerConfig unrelated;
    unrelated.setCollides(kChanA, 9, false);
    CHECK(build(kChanA, kChanB, unrelated));
}

TEST_CASE("PhysicsWorld: the landscape sits in the Terrain channel")
{
    // The implicit height field is the one body nobody has a component to name a
    // channel for, so its channel is fixed — and it is Terrain, not Default, so
    // that "everything but the ground" is a thing a project can express.
    HorizonWorld world;
    Entity land = world.createEntity("Landscape");
    {
        TransformComponent t; t.position = { 0.0f, 0.0f, 0.0f }; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(land, t);
        TerrainComponent tc;
        tc.resolution = 33;
        tc.sizeX      = 64.0f;
        tc.sizeZ      = 64.0f;
        tc.seed       = 0;      // flat
        world.addComponent(land, tc);
    }
    Entity box = world.createEntity("Box");
    {
        TransformComponent t; t.position = { 0.0f, 5.0f, 0.0f }; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(box, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.collisionLayer = kChanA;
        world.addComponent(box, rb);
    }

    PhysicsWorld phys;
    HE::CollisionLayerConfig cfg;
    cfg.setCollides(HE::CollisionLayerConfig::kTerrain, kChanA, false);
    phys.setCollisionLayers(cfg);
    phys.initialize(world);
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    // Blocking Terrain × kChanA is enough to fall through the landscape — which
    // it would not be if the height field had been left in Default.
    CHECK(world.registry().get<TransformComponent>(box).position.y < -5.0f);
}

// ─── Layer masks on the queries ───────────────────────────────────────────────
// The mask is what a query MAY SEE, and it has nothing to do with the collision
// matrix: the matrix decides what the simulation resolves, this decides what a
// ray is allowed to report. A body on a channel a ray excludes is invisible to
// it even though nothing about the simulation changed.
namespace
{
    // A static box on a named channel, so a query can be asked to see it or not.
    Entity makeLayeredBox(HorizonWorld& world, const char* name,
                          const glm::vec3& pos, uint8_t channel)
    {
        Entity e = world.createEntity(name);
        TransformComponent t; t.position = pos; t.scale = { 1.0f, 1.0f, 1.0f };
        world.addComponent(e, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Static; rb.collisionLayer = channel;
        world.addComponent(e, rb);
        return e;
    }

    constexpr uint32_t bit(uint8_t channel) { return 1u << channel; }
}

TEST_CASE("PhysicsWorld: a raycast reports which channel it hit")
{
    HorizonWorld world;
    const Entity wall = makeLayeredBox(world, "Wall", { 0.0f, 0.0f, 0.0f }, kChanA);

    PhysicsWorld phys;
    phys.initialize(world);

    const auto hit = phys.raycast({ -10.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 100.0f);
    REQUIRE(hit.hit);
    CHECK(hit.entityId == static_cast<uint32_t>(wall));
    // The point of the field: the caller learns WHAT it hit without looking the
    // entity up in the registry to read its RigidBodyComponent back.
    CHECK(hit.layer == kChanA);
}

// MUTATION: pass `{}` instead of layerFilter into CastRay and this fails — the
// excluded wall comes back as a hit.
TEST_CASE("PhysicsWorld: a raycast only sees the channels its mask names")
{
    HorizonWorld world;
    const Entity near_ = makeLayeredBox(world, "Near", { 0.0f, 0.0f, 0.0f }, kChanA);
    const Entity far_  = makeLayeredBox(world, "Far",  { 6.0f, 0.0f, 0.0f }, kChanB);

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 from{ -10.0f, 0.0f, 0.0f };
    const glm::vec3 dir { 1.0f, 0.0f, 0.0f };

    // No mask: the closest thing, as always.
    CHECK(phys.raycast(from, dir, 100.0f).entityId == static_cast<uint32_t>(near_));
    // Everything: the same answer, so the default really is "no filter".
    CHECK(phys.raycast(from, dir, 100.0f, PhysicsWorld::kNoEntity,
                       PhysicsWorld::kAllLayers).entityId == static_cast<uint32_t>(near_));

    // Only the far channel: the near box is skipped and the ray carries on.
    const auto onlyFar = phys.raycast(from, dir, 100.0f, PhysicsWorld::kNoEntity, bit(kChanB));
    REQUIRE(onlyFar.hit);
    CHECK(onlyFar.entityId == static_cast<uint32_t>(far_));
    CHECK(onlyFar.layer == kChanB);

    // A mask that names neither: a miss, not the nearest thing anyway.
    CHECK_FALSE(phys.raycast(from, dir, 100.0f, PhysicsWorld::kNoEntity, bit(kChanA + 4)).hit);
    // And zero really does mean "see nothing" — the reason the mask never got
    // added to the existing registry signatures.
    CHECK_FALSE(phys.raycast(from, dir, 100.0f, PhysicsWorld::kNoEntity, 0u).hit);
}

TEST_CASE("PhysicsWorld: a sphere cast only sees the channels its mask names")
{
    HorizonWorld world;
    const Entity near_ = makeLayeredBox(world, "Near", { 0.0f, 0.0f, 0.0f }, kChanA);
    const Entity far_  = makeLayeredBox(world, "Far",  { 6.0f, 0.0f, 0.0f }, kChanB);

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 from{ -10.0f, 0.0f, 0.0f };
    const glm::vec3 dir { 1.0f, 0.0f, 0.0f };

    const auto all = phys.sphereCast(from, dir, 0.25f, 100.0f);
    REQUIRE(all.hit);
    CHECK(all.entityId == static_cast<uint32_t>(near_));
    CHECK(all.layer == kChanA);

    const auto onlyFar = phys.sphereCast(from, dir, 0.25f, 100.0f,
                                         PhysicsWorld::kNoEntity, bit(kChanB));
    REQUIRE(onlyFar.hit);
    CHECK(onlyFar.entityId == static_cast<uint32_t>(far_));
    CHECK(onlyFar.layer == kChanB);

    CHECK_FALSE(phys.sphereCast(from, dir, 0.25f, 100.0f,
                                PhysicsWorld::kNoEntity, 0u).hit);
}

TEST_CASE("PhysicsWorld: an overlap query only sees the channels its mask names")
{
    HorizonWorld world;
    const Entity a = makeLayeredBox(world, "A", { 0.0f, 0.0f, 0.0f }, kChanA);
    const Entity b = makeLayeredBox(world, "B", { 1.5f, 0.0f, 0.0f }, kChanB);

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 at{ 0.75f, 0.0f, 0.0f };
    CHECK(phys.overlapSphere(at, 3.0f).size() == 2u);

    const auto onlyA = phys.overlapSphere(at, 3.0f, PhysicsWorld::kNoEntity, bit(kChanA));
    REQUIRE(onlyA.size() == 1u);
    CHECK(onlyA[0] == static_cast<uint32_t>(a));

    const auto onlyB = phys.overlapSphere(at, 3.0f, PhysicsWorld::kNoEntity, bit(kChanB));
    REQUIRE(onlyB.size() == 1u);
    CHECK(onlyB[0] == static_cast<uint32_t>(b));

    CHECK(phys.overlapSphere(at, 3.0f, PhysicsWorld::kNoEntity, 0u).empty());
}

// The mask filters what a query SEES; the matrix decides what the simulation
// RESOLVES. Two different questions that both mention layers, and confusing them
// is the easy mistake here — so one test pins them apart.
TEST_CASE("PhysicsWorld: the query mask is not the collision matrix")
{
    HorizonWorld world;
    const Entity wall = makeLayeredBox(world, "Wall", { 0.0f, 0.0f, 0.0f }, kChanA);

    PhysicsWorld phys;
    HE::CollisionLayerConfig cfg;
    // Everything about kChanA is switched off in the simulation…
    for (int b = 0; b < HE::CollisionLayerConfig::kCount; ++b)
        cfg.setCollides(kChanA, b, false);
    phys.setCollisionLayers(cfg);
    phys.initialize(world);

    // …and a ray that names kChanA still finds it. A query is not a contact.
    const auto hit = phys.raycast({ -10.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
                                  100.0f, PhysicsWorld::kNoEntity, bit(kChanA));
    REQUIRE(hit.hit);
    CHECK(hit.entityId == static_cast<uint32_t>(wall));
}

// ─── Shape casts and overlaps with an orientation ─────────────────────────────

namespace
{
    // A static box of an arbitrary size and pose. The three shape-query tests
    // below all need a wall that is somewhere other than axis-aligned at the
    // origin, which none of the earlier helpers can build.
    Entity makeBox(HorizonWorld& world, const char* name, const glm::vec3& pos,
                   const glm::vec3& scale, const glm::vec3& rotationEuler = glm::vec3(0.0f))
    {
        Entity e = world.createEntity(name);
        TransformComponent t;
        t.position = pos;
        t.scale    = scale;
        t.rotation = rotationEuler;
        world.addComponent(e, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Static;
        world.addComponent(e, rb);
        return e;
    }
}

TEST_CASE("PhysicsWorld: a box cast is the width the caller drew, and its rotation turns it")
{
    HorizonWorld world;
    // Two one-metre cubes with a one-metre gap between their inner faces.
    makeBox(world, "Left",  { -1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f });
    makeBox(world, "Right", {  1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 from{ 0.0f, 0.0f, -5.0f };
    const glm::vec3 dir { 0.0f, 0.0f,  1.0f };
    // Three metres wide across the gap, half a metre deep — it cannot fit.
    const glm::vec3 halfExtents{ 1.5f, 0.3f, 0.3f };

    const auto blocked = phys.boxCast(from, halfExtents, glm::vec3(0.0f), dir, 10.0f);
    REQUIRE(blocked.hit);
    // Where the BOX stopped, not where it touched: its leading face rests on the
    // wall, so its centre is its own half depth short of it.
    CHECK(blocked.distance == doctest::Approx(4.2f).epsilon(0.02));

    // The same box turned a quarter turn about Y is three metres DEEP and a
    // little over half a metre wide, so it goes straight through the gap.
    CHECK_FALSE(phys.boxCast(from, halfExtents, { 0.0f, 90.0f, 0.0f }, dir, 10.0f).hit);

    // A ray down the middle of the gap hits nothing either — which is exactly
    // the reason boxCast exists: a line does not know how wide the caller is.
    CHECK_FALSE(phys.raycast(from, dir, 10.0f).hit);

    // Degenerate extents are an empty question, not an assertion in a debug build.
    CHECK_FALSE(phys.boxCast(from, { 0.0f, 0.3f, 0.3f }, glm::vec3(0.0f), dir, 10.0f).hit);
    CHECK_FALSE(phys.boxCast(from, { -1.0f, 0.3f, 0.3f }, glm::vec3(0.0f), dir, 10.0f).hit);
}

// The one that would catch a second Euler convention. A cast box whose rotation
// is read the same way a BODY's rotation is lies parallel to the tilted wall and
// so travels furthest before touching it; read any other way it presents a
// corner and is stopped early.
TEST_CASE("PhysicsWorld: a cast's rotation means what a body's rotation means")
{
    HorizonWorld world;
    const glm::vec3 tilt{ 0.0f, 45.0f, 0.0f };
    makeBox(world, "TiltedWall", { 0.0f, 0.0f, 0.0f }, { 4.0f, 4.0f, 0.4f }, tilt);

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 from{ 0.0f, 0.0f, -10.0f };
    const glm::vec3 dir { 0.0f, 0.0f,  1.0f };
    const glm::vec3 plate{ 2.0f, 2.0f, 0.05f };   // a thin wide plate

    const auto aligned    = phys.boxCast(from, plate, tilt, dir, 20.0f);
    const auto misaligned = phys.boxCast(from, plate, glm::vec3(0.0f), dir, 20.0f);
    REQUIRE(aligned.hit);
    REQUIRE(misaligned.hit);

    // Face to face, so the plate gets within a couple of centimetres of the wall.
    CHECK(aligned.distance > misaligned.distance + 1.0f);
    CHECK(aligned.distance > 9.0f);
    CHECK(aligned.distance < 10.0f);
}

TEST_CASE("PhysicsWorld: a capsule cast stops its own half height short of the floor")
{
    HorizonWorld world;
    makeBox(world, "Floor", { 0.0f, 0.0f, 0.0f }, { 20.0f, 0.5f, 20.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 from{ 0.0f, 5.0f, 0.0f };
    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };

    // radius 0.5, full height 2 → half a metre of cylinder plus two caps, so the
    // capsule's lowest point is one metre below its centre. The floor's top face
    // is at 0.25.
    const auto hit = phys.capsuleCast(from, 0.5f, 2.0f, glm::vec3(0.0f), down, 20.0f);
    REQUIRE(hit.hit);
    CHECK(hit.distance == doctest::Approx(3.75f).epsilon(0.02));
    CHECK(hit.normal.y > 0.9f);

    // A height that is nothing but caps is a sphere, and is answered as one
    // rather than refused — Jolt would assert on a cylinder of zero length.
    const auto sphereish = phys.capsuleCast(from, 0.5f, 0.5f, glm::vec3(0.0f), down, 20.0f);
    REQUIRE(sphereish.hit);
    CHECK(sphereish.distance == doctest::Approx(4.25f).epsilon(0.02));

    CHECK_FALSE(phys.capsuleCast(from, 0.0f, 2.0f, glm::vec3(0.0f), down, 20.0f).hit);
    CHECK_FALSE(phys.capsuleCast(from, 0.5f, 2.0f, glm::vec3(0.0f), down, 0.0f).hit);
}

TEST_CASE("PhysicsWorld: an overlap box sweeps a corridor, and turning it changes the answer")
{
    HorizonWorld world;
    const Entity alongX = makeBox(world, "AlongX", { 3.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f });
    const Entity alongZ = makeBox(world, "AlongZ", { 0.0f, 0.0f, 3.0f }, { 1.0f, 1.0f, 1.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    // A rod eight metres long down the X axis: it reaches the box on X and
    // passes nowhere near the one on Z. A sphere big enough to reach either
    // would have caught both, which is the whole reason for the shape.
    const glm::vec3 rod{ 4.0f, 0.6f, 0.6f };
    const auto onX = phys.overlapBox(glm::vec3(0.0f), rod, glm::vec3(0.0f));
    REQUIRE(onX.size() == 1u);
    CHECK(onX[0] == static_cast<uint32_t>(alongX));

    const auto onZ = phys.overlapBox(glm::vec3(0.0f), rod, { 0.0f, 90.0f, 0.0f });
    REQUIRE(onZ.size() == 1u);
    CHECK(onZ[0] == static_cast<uint32_t>(alongZ));

    // Ignore and mask behave as they do everywhere else.
    CHECK(phys.overlapBox(glm::vec3(0.0f), rod, glm::vec3(0.0f),
                          static_cast<uint32_t>(alongX)).empty());
    CHECK(phys.overlapBox(glm::vec3(0.0f), rod, glm::vec3(0.0f),
                          PhysicsWorld::kNoEntity, 0u).empty());
    CHECK(phys.overlapBox(glm::vec3(0.0f), { 0.0f, 0.6f, 0.6f }, glm::vec3(0.0f)).empty());
}

TEST_CASE("PhysicsWorld: an overlap capsule stands up until it is told to lie down")
{
    HorizonWorld world;
    const Entity above = makeBox(world, "Above", { 0.0f, 1.5f, 0.0f }, { 1.0f, 1.0f, 1.0f });
    const Entity beside = makeBox(world, "Beside", { 1.5f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    // Radius 0.5, full height 4 → two metres of reach up and down, half a metre
    // sideways: the "is anybody standing inside me" query a respawn makes.
    const auto standing = phys.overlapCapsule(glm::vec3(0.0f), 0.5f, 4.0f, glm::vec3(0.0f));
    REQUIRE(standing.size() == 1u);
    CHECK(standing[0] == static_cast<uint32_t>(above));

    // A quarter turn about Z puts the same capsule on its side, and the answer
    // changes with it.
    const auto lying = phys.overlapCapsule(glm::vec3(0.0f), 0.5f, 4.0f, { 0.0f, 0.0f, 90.0f });
    REQUIRE(lying.size() == 1u);
    CHECK(lying[0] == static_cast<uint32_t>(beside));

    CHECK(phys.overlapCapsule(glm::vec3(0.0f), 0.0f, 4.0f, glm::vec3(0.0f)).empty());
    CHECK(phys.overlapCapsule(glm::vec3(0.0f), 0.5f, 4.0f, glm::vec3(0.0f),
                              PhysicsWorld::kNoEntity, 0u).empty());
}

TEST_CASE("PhysicsWorld: raycastAll reports everything on the line, nearest first")
{
    HorizonWorld world;
    const Entity near_ = makeStaticBox(world, "Near", {  0.0f, 0.0f, 0.0f });
    const Entity mid   = makeStaticBox(world, "Mid",  {  6.0f, 0.0f, 0.0f });
    const Entity far_  = makeStaticBox(world, "Far",  { 12.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 from{ -10.0f, 0.0f, 0.0f };
    const glm::vec3 dir { 1.0f, 0.0f, 0.0f };

    const auto hits = phys.raycastAll(from, dir, 100.0f);
    REQUIRE(hits.size() == 3u);
    CHECK(hits[0].entityId == static_cast<uint32_t>(near_));
    CHECK(hits[1].entityId == static_cast<uint32_t>(mid));
    CHECK(hits[2].entityId == static_cast<uint32_t>(far_));
    CHECK(hits[0].distance < hits[1].distance);
    CHECK(hits[1].distance < hits[2].distance);
    CHECK(hits[0].distance == doctest::Approx(9.5f).epsilon(0.02));
    for (const auto& h : hits)
    {
        CHECK(h.hit);
        CHECK(h.normal.x < -0.9f);   // every face turned back towards the shooter
    }

    // The promise the header makes: the first of these is the hit `raycast`
    // would have returned on its own.
    const auto single = phys.raycast(from, dir, 100.0f);
    REQUIRE(single.hit);
    CHECK(single.entityId == hits[0].entityId);
    CHECK(single.distance == doctest::Approx(hits[0].distance));

    // Range, ignore and "nothing there" all behave like the single-hit form.
    CHECK(phys.raycastAll(from, dir, 12.0f).size() == 1u);
    CHECK(phys.raycastAll(from, dir, 100.0f, static_cast<uint32_t>(mid)).size() == 2u);
    CHECK(phys.raycastAll({ 0.0f, 50.0f, 0.0f }, dir, 100.0f).empty());
    CHECK(phys.raycastAll(from, glm::vec3(0.0f), 100.0f).empty());
}

TEST_CASE("PhysicsWorld: raycastAll sees only the channels its mask names")
{
    HorizonWorld world;
    const Entity a = makeLayeredBox(world, "A", { 0.0f, 0.0f, 0.0f }, kChanA);
    const Entity b = makeLayeredBox(world, "B", { 6.0f, 0.0f, 0.0f }, kChanB);

    PhysicsWorld phys;
    phys.initialize(world);

    const glm::vec3 from{ -10.0f, 0.0f, 0.0f };
    const glm::vec3 dir { 1.0f, 0.0f, 0.0f };

    CHECK(phys.raycastAll(from, dir, 100.0f).size() == 2u);

    const auto onlyB = phys.raycastAll(from, dir, 100.0f, PhysicsWorld::kNoEntity, bit(kChanB));
    REQUIRE(onlyB.size() == 1u);
    CHECK(onlyB[0].entityId == static_cast<uint32_t>(b));
    CHECK(onlyB[0].layer == kChanB);

    const auto onlyA = phys.raycastAll(from, dir, 100.0f, PhysicsWorld::kNoEntity, bit(kChanA));
    REQUIRE(onlyA.size() == 1u);
    CHECK(onlyA[0].entityId == static_cast<uint32_t>(a));

    CHECK(phys.raycastAll(from, dir, 100.0f, PhysicsWorld::kNoEntity, 0u).empty());
}

TEST_CASE("PhysicsWorld: shape queries answer nothing on a world that was never initialised")
{
    PhysicsWorld phys;   // no initialize()
    CHECK_FALSE(phys.boxCast({ 0, 0, 0 }, { 1, 1, 1 }, glm::vec3(0.0f), { 0, 0, 1 }, 10.0f).hit);
    CHECK_FALSE(phys.capsuleCast({ 0, 0, 0 }, 0.5f, 2.0f, glm::vec3(0.0f), { 0, 0, 1 }, 10.0f).hit);
    CHECK(phys.overlapBox({ 0, 0, 0 }, { 1, 1, 1 }, glm::vec3(0.0f)).empty());
    CHECK(phys.overlapCapsule({ 0, 0, 0 }, 0.5f, 2.0f, glm::vec3(0.0f)).empty());
    CHECK(phys.raycastAll({ 0, 0, 0 }, { 0, 0, 1 }, 10.0f).empty());
}

// ─── Force at a point, and spin ───────────────────────────────────────────────

// One test for both features, because each is the other's measuring instrument:
// an off-centre impulse is the only push that produces a spin, and the angular
// velocity is the only way to see that the point was used at all.
TEST_CASE("PhysicsWorld: an impulse off the centre spins the body, one through it does not")
{
    HorizonWorld world;
    const Entity centred = makeDynamicBox(world, "Centred", { 0.0f, 10.0f, 0.0f });
    const Entity edged   = makeDynamicBox(world, "Edged",   { 8.0f, 10.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    phys.setGravity({ 0.0f, 0.0f, 0.0f });   // isolate the push from the fall

    // Straight through the centre of mass: all of it becomes travel.
    CHECK(phys.addImpulse(static_cast<uint32_t>(centred), { 0.0f, 0.0f, 5.0f }));
    CHECK(glm::length(phys.getAngularVelocity(static_cast<uint32_t>(centred))) < 1e-4f);
    CHECK(phys.getVelocity(static_cast<uint32_t>(centred)).z == doctest::Approx(5.0f));

    // The same push at the corner of the other box: it travels AND it turns.
    CHECK(phys.addImpulseAtPosition(static_cast<uint32_t>(edged), { 0.0f, 0.0f, 5.0f },
                                    { 8.5f, 10.5f, 0.0f }));
    CHECK(glm::length(phys.getAngularVelocity(static_cast<uint32_t>(edged))) > 0.1f);
    CHECK(phys.getVelocity(static_cast<uint32_t>(edged)).z == doctest::Approx(5.0f));

    // A force at a point needs a step to be felt — Jolt accumulates it and
    // consumes it in the next update, exactly like addForce.
    const Entity pushed = edged;
    phys.setVelocity(static_cast<uint32_t>(pushed), glm::vec3(0.0f));
    phys.setAngularVelocity(static_cast<uint32_t>(pushed), glm::vec3(0.0f));
    CHECK(phys.addForceAtPosition(static_cast<uint32_t>(pushed), { 0.0f, 0.0f, 500.0f },
                                  { 8.5f, 10.5f, 0.0f }));
    phys.step(world, kDt);
    CHECK(glm::length(phys.getAngularVelocity(static_cast<uint32_t>(pushed))) > 0.1f);
}

TEST_CASE("PhysicsWorld: angular velocity round-trips, and refuses what cannot turn")
{
    HorizonWorld world;
    const Entity box   = makeDynamicBox(world, "Box", { 0.0f, 10.0f, 0.0f });
    const Entity wall  = makeStaticBox(world, "Wall", { 20.0f, 0.0f, 0.0f });
    const Entity ghost = world.createEntity("NoBody");
    { TransformComponent t; world.addComponent(ghost, t); }

    PhysicsWorld phys;
    phys.initialize(world);
    phys.setGravity(glm::vec3(0.0f));

    // Radians per second about the world axes: a full turn a second is 2π.
    CHECK(phys.setAngularVelocity(static_cast<uint32_t>(box), { 0.0f, 6.2832f, 0.0f }));
    CHECK(phys.getAngularVelocity(static_cast<uint32_t>(box)).y == doctest::Approx(6.2832f));

    // And it is a real spin, not a stored number: a quarter of a second of it
    // turns the box a noticeable amount about Y.
    for (int i = 0; i < 15; ++i)
        phys.step(world, kDt);
    CHECK(std::abs(world.registry().get<TransformComponent>(box).rotation.y) > 20.0f);

    CHECK_FALSE(phys.setAngularVelocity(static_cast<uint32_t>(wall), { 0.0f, 1.0f, 0.0f }));
    CHECK_FALSE(phys.setAngularVelocity(static_cast<uint32_t>(ghost), { 0.0f, 1.0f, 0.0f }));
    CHECK(phys.getAngularVelocity(static_cast<uint32_t>(ghost)) == glm::vec3(0.0f));
    CHECK(phys.getAngularVelocity(99999u) == glm::vec3(0.0f));
}

TEST_CASE("PhysicsWorld: a push at a point refuses exactly what a push at the centre refuses")
{
    HorizonWorld world;
    const Entity wall  = makeStaticBox(world, "Wall", { 0.0f, 0.0f, 0.0f });
    const Entity ghost = world.createEntity("NoBody");
    { TransformComponent t; world.addComponent(ghost, t); }

    PhysicsWorld phys;
    phys.initialize(world);

    CHECK_FALSE(phys.addForceAtPosition(static_cast<uint32_t>(wall), { 1, 0, 0 }, { 0, 0, 0 }));
    CHECK_FALSE(phys.addImpulseAtPosition(static_cast<uint32_t>(wall), { 1, 0, 0 }, { 0, 0, 0 }));
    CHECK_FALSE(phys.addForceAtPosition(static_cast<uint32_t>(ghost), { 1, 0, 0 }, { 0, 0, 0 }));
    CHECK_FALSE(phys.addImpulseAtPosition(99999u, { 1, 0, 0 }, { 0, 0, 0 }));
}

// ─── Joints ───────────────────────────────────────────────────────────────────

namespace {
    // Author a joint the way the editor does: a component on A that names B by
    // UUID. The runtime path (addJoint) writes exactly this, so a test that used
    // only the runtime path would never exercise what a saved scene loads.
    void jointTo(HorizonWorld& world, Entity a, Entity b, JointType type,
                 const glm::vec3& anchorA = glm::vec3(0.0f),
                 const glm::vec3& axis    = glm::vec3(0.0f, 1.0f, 0.0f),
                 float minLimit = 0.0f, float maxLimit = 0.0f,
                 const glm::vec3& anchorB = glm::vec3(0.0f))
    {
        JointComponent j;
        j.type     = type;
        j.target   = world.entityId(b);
        j.anchorA  = anchorA;
        j.anchorB  = anchorB;
        j.axis     = axis;
        j.minLimit = minLimit;
        j.maxLimit = maxLimit;
        world.registry().emplace_or_replace<JointComponent>(a, j);
    }

    glm::vec3 posOf(HorizonWorld& world, Entity e)
    {
        return world.registry().get<TransformComponent>(e).position;
    }
}

TEST_CASE("PhysicsWorld: a Fixed joint holds a body up that would otherwise fall")
{
    HorizonWorld world;
    const Entity anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
    const Entity hung   = makeDynamicBox(world, "Hung",   { 2.0f, 10.0f, 0.0f });
    jointTo(world, hung, anchor, JointType::Fixed);

    PhysicsWorld phys;
    phys.initialize(world);
    CHECK(phys.hasJoint(static_cast<uint32_t>(hung)));

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    // Welded to something that cannot move, so two seconds of gravity does
    // nothing at all. Without the joint this is a body 19 m lower.
    CHECK(posOf(world, hung).y == doctest::Approx(10.0f).epsilon(0.02));
    CHECK(posOf(world, hung).x == doctest::Approx(2.0f).epsilon(0.02));
}

TEST_CASE("PhysicsWorld: a Point joint is a pendulum — it swings but never leaves its pivot")
{
    HorizonWorld world;
    const Entity anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
    const Entity arm    = makeDynamicBox(world, "Arm",    { 2.0f, 10.0f, 0.0f });
    // The pivot is ONE point: A's own (-2, 0, 0) is the anchor's position in the
    // world. Two separate anchors here would tell Jolt to make them coincide and
    // the first step would snap the arm into the anchor.
    jointTo(world, arm, anchor, JointType::Point, { -2.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(arm)));

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    const glm::vec3 pivot{ 0.0f, 10.0f, 0.0f };
    CHECK(glm::length(posOf(world, arm) - pivot) == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(posOf(world, arm).y < 9.5f);   // it did swing down; it is not welded
}

TEST_CASE("PhysicsWorld: a Hinge's limits are degrees, and they stop the swing")
{
    const auto swing = [](float minLimit, float maxLimit) {
        HorizonWorld world;
        const Entity anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
        const Entity door   = makeDynamicBox(world, "Door",   { 2.0f, 10.0f, 0.0f });
        jointTo(world, door, anchor, JointType::Hinge, { -2.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f }, minLimit, maxLimit);
        PhysicsWorld phys;
        phys.initialize(world);
        REQUIRE(phys.hasJoint(static_cast<uint32_t>(door)));
        for (int i = 0; i < kSteps2s; ++i)
            phys.step(world, kDt);
        return posOf(world, door);
    };

    // min >= max is the authored way of saying "no limit": the arm swings all
    // the way down like the point joint above.
    CHECK(swing(0.0f, 0.0f).y < 9.5f);

    // Five degrees either side of the pose it was built in. Five degrees of a
    // two-metre arm is 2·sin(5°) ≈ 0.17 m of drop — and it is degrees that make
    // that number: read as radians, five would be most of a full turn and the
    // arm would hang straight down.
    const glm::vec3 held = swing(-5.0f, 5.0f);
    CHECK(held.y > 9.7f);
    CHECK(held.y < 10.0f);   // it moved a little, so the limit is a limit and not a weld
}

TEST_CASE("PhysicsWorld: a Slider on a horizontal axis carries the body's weight")
{
    HorizonWorld world;
    const Entity rail    = makeStaticBox(world,  "Rail",    { 5.0f, 10.0f, 0.0f });
    const Entity carriage = makeDynamicBox(world, "Carriage", { 0.0f, 10.0f, 0.0f });
    // Travel along X only, limited to half a metre either way.
    jointTo(world, carriage, rail, JointType::Slider, glm::vec3(0.0f),
            { 1.0f, 0.0f, 0.0f }, -0.5f, 0.5f);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(carriage)));

    phys.addImpulse(static_cast<uint32_t>(carriage), { 20.0f, 0.0f, 0.0f });
    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    // Gravity is taken by the rail: a slider forbids every direction but its own.
    CHECK(posOf(world, carriage).y == doctest::Approx(10.0f).epsilon(0.02));
    // And the push spent itself against the stop rather than sending it away.
    CHECK(posOf(world, carriage).x > 0.2f);
    CHECK(posOf(world, carriage).x < 0.7f);
}

TEST_CASE("PhysicsWorld: a Distance joint is a rope — it catches the fall at its own length")
{
    HorizonWorld world;
    const Entity hook = makeStaticBox(world,  "Hook", { 0.0f, 10.0f, 0.0f });
    const Entity load = makeDynamicBox(world, "Load", { 0.0f,  5.0f, 0.0f });
    // Unlimited means "however far apart they are right now", which is the taut
    // rope an author placed by hand: five metres.
    jointTo(world, load, hook, JointType::Distance);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(load)));

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    CHECK(glm::length(posOf(world, load) - glm::vec3(0.0f, 10.0f, 0.0f))
              == doctest::Approx(5.0f).epsilon(0.05));
}

TEST_CASE("PhysicsWorld: destroying the other end takes the joint with it")
{
    HorizonWorld world;
    const Entity anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
    const Entity hung   = makeDynamicBox(world, "Hung",   { 2.0f, 10.0f, 0.0f });
    jointTo(world, hung, anchor, JointType::Fixed);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(hung)));

    // The joint belongs to `hung`, but it is `anchor` that is going away. A
    // constraint keeps raw Jolt Body pointers, so a joint left behind here reads
    // freed memory on the next step — with nothing to announce it.
    phys.removeEntity(static_cast<uint32_t>(anchor));
    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(hung)));

    for (int i = 0; i < 30; ++i)
        phys.step(world, kDt);
    CHECK(posOf(world, hung).y < 9.9f);   // nothing holds it any more
}

TEST_CASE("PhysicsWorld: the middle link of a chain can die without taking the process with it")
{
    HorizonWorld world;
    const Entity top    = makeStaticBox(world,  "Top",    { 0.0f, 10.0f, 0.0f });
    const Entity middle = makeDynamicBox(world, "Middle", { 0.0f,  8.0f, 0.0f });
    const Entity bottom = makeDynamicBox(world, "Bottom", { 0.0f,  6.0f, 0.0f });
    jointTo(world, middle, top,    JointType::Distance);
    jointTo(world, bottom, middle, JointType::Distance);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(middle)));
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(bottom)));

    // `middle` is named by the joint BELOW it, which nothing else knows about:
    // a bookkeeping that only mapped owner → joint would leave that one behind.
    world.destroyEntity(middle);
    for (int i = 0; i < 30; ++i)
        phys.step(world, kDt);   // the reap runs here

    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(middle)));
    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(bottom)));
    CHECK(posOf(world, bottom).y < 6.0f);
}

TEST_CASE("PhysicsWorld: rebuilding a body puts its joint back")
{
    HorizonWorld world;
    const Entity anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
    const Entity hung   = makeDynamicBox(world, "Hung",   { 2.0f, 10.0f, 0.0f });
    jointTo(world, hung, anchor, JointType::Fixed);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(hung)));

    // addEntity is documented idempotent — it tears the old body down and builds
    // a new one, which is what changing a collider at runtime goes through. The
    // joint has to survive that, and so does a joint pointing AT the rebuilt one.
    ColliderComponent col; col.shape = ColliderShape::Sphere; col.radius = 0.5f;
    world.addComponent(anchor, col);
    CHECK(phys.addEntity(world, static_cast<uint32_t>(anchor)));
    CHECK(phys.hasJoint(static_cast<uint32_t>(hung)));

    CHECK(phys.addEntity(world, static_cast<uint32_t>(hung)));
    CHECK(phys.hasJoint(static_cast<uint32_t>(hung)));

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);
    CHECK(posOf(world, hung).y == doctest::Approx(10.0f).epsilon(0.02));
}

TEST_CASE("PhysicsWorld: a joint whose partner spawns later is built when it arrives")
{
    HorizonWorld world;
    const Entity anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
    PhysicsWorld phys;
    phys.initialize(world);

    // The grapple line is hooked up before the thing at the other end exists —
    // built at runtime, entity by entity, in whatever order the spawn produces.
    const Entity hung = makeDynamicBox(world, "Hung", { 2.0f, 10.0f, 0.0f });
    jointTo(world, hung, anchor, JointType::Fixed);
    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(hung)));   // no body yet

    CHECK(phys.addEntity(world, static_cast<uint32_t>(hung)));
    CHECK(phys.hasJoint(static_cast<uint32_t>(hung)));
}

TEST_CASE("PhysicsWorld: the joint pass does not depend on the order entities were created")
{
    // The failure this guards against is invisible in one direction: entt hands
    // entities out in creation order, so a one-pass build works whenever the
    // partner happens to come first and fails silently when it does not.
    const auto hang = [](bool anchorFirst) {
        HorizonWorld world;
        Entity anchor = entt::null, hung = entt::null;
        if (anchorFirst)
        {
            anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
            hung   = makeDynamicBox(world, "Hung",   { 2.0f, 10.0f, 0.0f });
        }
        else
        {
            hung   = makeDynamicBox(world, "Hung",   { 2.0f, 10.0f, 0.0f });
            anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
        }
        jointTo(world, hung, anchor, JointType::Fixed);
        PhysicsWorld phys;
        phys.initialize(world);
        return phys.hasJoint(static_cast<uint32_t>(hung));
    };
    CHECK(hang(true));
    CHECK(hang(false));
}

TEST_CASE("PhysicsWorld: a joint to something that is not a rigid body is refused, not ignored")
{
    HorizonWorld world;
    const Entity box = makeDynamicBox(world, "Box", { 0.0f, 10.0f, 0.0f });

    // A character controller is not a body — CharacterVirtual has none — so it
    // cannot be one end of a joint. The usual way to discover that is a chain
    // that simply does not exist.
    const Entity player = world.createEntity("Player");
    { TransformComponent t; t.position = { 2.0f, 10.0f, 0.0f }; world.addComponent(player, t); }
    world.addComponent(player, CharacterControllerComponent{});

    const Entity nowhere = world.createEntity("Nowhere");
    { TransformComponent t; world.addComponent(nowhere, t); }

    jointTo(world, box, player, JointType::Fixed);
    PhysicsWorld phys;
    phys.initialize(world);
    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(box)));

    // Two static bodies could never move, and a joint naming nothing at all is a
    // component somebody added and never filled in. Both are refused.
    const Entity wallA = makeStaticBox(world, "WallA", { 20.0f, 0.0f, 0.0f });
    const Entity wallB = makeStaticBox(world, "WallB", { 24.0f, 0.0f, 0.0f });
    PhysicsWorld::JointDesc desc;
    CHECK_FALSE(phys.addJoint(world, static_cast<uint32_t>(wallA),
                              static_cast<uint32_t>(wallB), desc));
    CHECK_FALSE(phys.addJoint(world, static_cast<uint32_t>(box),
                              static_cast<uint32_t>(box), desc));
    CHECK_FALSE(phys.addJoint(world, static_cast<uint32_t>(box),
                              static_cast<uint32_t>(nowhere), desc));

    // A hinge with no axis has no direction to turn about; that is a refusal
    // rather than a NaN in the solver.
    desc.type = JointType::Hinge;
    desc.axis = glm::vec3(0.0f);
    const Entity other = makeDynamicBox(world, "Other", { 40.0f, 10.0f, 0.0f });
    phys.addEntity(world, static_cast<uint32_t>(other));
    CHECK_FALSE(phys.addJoint(world, static_cast<uint32_t>(box),
                              static_cast<uint32_t>(other), desc));
}

TEST_CASE("PhysicsWorld: addJoint writes the component, removeJoint takes it away again")
{
    HorizonWorld world;
    const Entity anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
    const Entity hung   = makeDynamicBox(world, "Hung",   { 2.0f, 10.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    PhysicsWorld::JointDesc desc;
    desc.type     = JointType::Hinge;
    desc.anchorA  = { -2.0f, 0.0f, 0.0f };
    desc.axis     = { 0.0f, 0.0f, 1.0f };
    desc.minLimit = -30.0f;
    desc.maxLimit =  30.0f;
    CHECK(phys.addJoint(world, static_cast<uint32_t>(hung),
                        static_cast<uint32_t>(anchor), desc));
    CHECK(phys.hasJoint(static_cast<uint32_t>(hung)));

    // The COMPONENT is the source of truth: a joint that only lived inside Jolt
    // would be gone the next time the scene was saved and loaded.
    const auto* jc = world.registry().try_get<JointComponent>(hung);
    REQUIRE(jc != nullptr);
    CHECK(jc->type == JointType::Hinge);
    CHECK(jc->target == world.entityId(anchor));
    CHECK(jc->minLimit == doctest::Approx(-30.0f));

    // Idempotent, like addEntity: the second call replaces rather than stacks.
    desc.type = JointType::Fixed;
    CHECK(phys.addJoint(world, static_cast<uint32_t>(hung),
                        static_cast<uint32_t>(anchor), desc));
    CHECK(world.registry().get<JointComponent>(hung).type == JointType::Fixed);

    CHECK(phys.removeJoint(world, static_cast<uint32_t>(hung)));
    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(hung)));
    CHECK_FALSE(world.registry().all_of<JointComponent>(hung));
    CHECK_FALSE(phys.removeJoint(world, static_cast<uint32_t>(hung)));   // nothing left
}

TEST_CASE("PhysicsWorld: clearing a world full of joints destroys them before the bodies")
{
    HorizonWorld world;
    const Entity top    = makeStaticBox(world,  "Top",    { 0.0f, 10.0f, 0.0f });
    const Entity middle = makeDynamicBox(world, "Middle", { 0.0f,  8.0f, 0.0f });
    const Entity bottom = makeDynamicBox(world, "Bottom", { 0.0f,  6.0f, 0.0f });
    jointTo(world, middle, top,    JointType::Distance);
    jointTo(world, bottom, middle, JointType::Distance);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(bottom)));

    phys.clear();
    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(middle)));
    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(bottom)));

    // And the whole thing can be stood back up on the same world.
    phys.initialize(world);
    CHECK(phys.hasJoint(static_cast<uint32_t>(middle)));
    CHECK(phys.hasJoint(static_cast<uint32_t>(bottom)));
    for (int i = 0; i < 30; ++i)
        phys.step(world, kDt);
}

TEST_CASE("PhysicsWorld: a joint anchor is local, so a parented body hangs where it stands")
{
    // The anchor is authored in the entity's OWN space, and the entity here is a
    // child standing 100 m from its parent's origin. Read as a world point, or
    // composed from TransformComponent::worldMatrix (which nothing has
    // propagated yet), the pivot would land at the origin and the whole thing
    // would be dragged there instead of swinging in place.
    HorizonWorld world;
    const Entity zone = world.createEntity("Zone");
    { TransformComponent t; t.position = { 100.0f, 0.0f, 0.0f }; world.addComponent(zone, t); }

    const Entity anchor = makeStaticBox(world,  "Anchor", { 0.0f, 10.0f, 0.0f });
    const Entity arm    = makeDynamicBox(world, "Arm",    { 2.0f, 10.0f, 0.0f });
    world.reparentEntity(anchor, zone);
    world.reparentEntity(arm,    zone);
    jointTo(world, arm, anchor, JointType::Point, { -2.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(arm)));

    for (int i = 0; i < kSteps2s; ++i)
        phys.step(world, kDt);

    // Still hanging off its own pivot 100 m out, not pulled to the origin.
    const glm::vec3 worldPos = HE::worldPositionOf(world, arm);
    CHECK(glm::length(worldPos - glm::vec3(100.0f, 10.0f, 0.0f))
              == doctest::Approx(2.0f).epsilon(0.05));
}

// ─── Joints, part two: collideConnected, motor, break ─────────────────────────

namespace {
    // The four fields Step 6 added are set on the component after jointTo has
    // written it, rather than as four more defaulted parameters there: the
    // helper's signature is already at its limit, and a test that says
    // `jc.breakForce = 5` reads as what it is.
    JointComponent& jointOf(HorizonWorld& world, Entity e)
    {
        return world.registry().get<JointComponent>(e);
    }

    // Did the two entities touch at all over `steps` steps? Drains the queue
    // every step, because it is a queue and a scene that keeps touching would
    // otherwise only ever be asked about its first frame.
    bool touchedEachOther(HorizonWorld& world, PhysicsWorld& phys,
                          Entity a, Entity b, int steps)
    {
        const auto ia = static_cast<uint32_t>(a);
        const auto ib = static_cast<uint32_t>(b);
        bool touched = false;
        for (int i = 0; i < steps; ++i)
        {
            phys.step(world, kDt);
            for (const auto& ev : phys.pollCollisionEnter())
                if ((ev.entityA == ia && ev.entityB == ib) ||
                    (ev.entityA == ib && ev.entityB == ia))
                    touched = true;
        }
        return touched;
    }
}

TEST_CASE("PhysicsWorld: collideConnected decides whether two jointed bodies touch")
{
    // Two boxes welded together while overlapping — the shape of every chain
    // link and every ragdoll limb. Off, they ignore each other; on, they report
    // the contact like any other pair.
    const auto run = [](bool collide) {
        HorizonWorld world;
        const Entity a = makeDynamicBox(world, "A", { 0.00f, 10.0f, 0.0f });
        const Entity b = makeDynamicBox(world, "B", { 0.25f, 10.0f, 0.0f });
        jointTo(world, b, a, JointType::Fixed);
        PhysicsWorld phys;
        phys.initialize(world);
        // Set BEFORE the build would be the same thing; set after and rebuilt is
        // the live path, and it has to agree with the authored one.
        jointOf(world, b).collideConnected = collide;
        phys.addEntity(world, static_cast<uint32_t>(b));
        REQUIRE(phys.hasJoint(static_cast<uint32_t>(b)));
        return touchedEachOther(world, phys, a, b, 30);
    };

    CHECK_FALSE(run(false));   // the default, and what a chain needs
    CHECK(run(true));
}

TEST_CASE("PhysicsWorld: setJointCollideConnected takes effect without a rebuild")
{
    HorizonWorld world;
    const Entity a = makeDynamicBox(world, "A", { 0.00f, 10.0f, 0.0f });
    const Entity b = makeDynamicBox(world, "B", { 0.25f, 10.0f, 0.0f });
    jointTo(world, b, a, JointType::Fixed);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(b)));
    CHECK_FALSE(touchedEachOther(world, phys, a, b, 20));

    CHECK(phys.setJointCollideConnected(world, static_cast<uint32_t>(b), true));
    CHECK(jointOf(world, b).collideConnected);         // the component is the truth
    CHECK(touchedEachOther(world, phys, a, b, 20));    // and the simulation agrees

    // A joint the entity does not own is not a joint it can change: only the
    // side that AUTHORED it carries the component.
    CHECK_FALSE(phys.setJointCollideConnected(world, static_cast<uint32_t>(a), true));
}

TEST_CASE("PhysicsWorld: a Hinge motor drives the door, and the force is the switch")
{
    // A VERTICAL hinge axis, so gravity pulls along the axis and cannot turn the
    // door at all: every degree of rotation below is the motor's doing and
    // nothing else's.
    const auto swing = [](float targetSpeed, float maxForce) {
        HorizonWorld world;
        const Entity frame = makeStaticBox(world,  "Frame", { 0.0f, 10.0f, 0.0f });
        const Entity door  = makeDynamicBox(world, "Door",  { 1.0f, 10.0f, 0.0f });
        jointTo(world, door, frame, JointType::Hinge, { -1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f });
        auto& jc = jointOf(world, door);
        jc.motorTarget   = targetSpeed;
        jc.motorMaxForce = maxForce;
        PhysicsWorld phys;
        phys.initialize(world);
        REQUIRE(phys.hasJoint(static_cast<uint32_t>(door)));
        for (int i = 0; i < 60; ++i)
            phys.step(world, kDt);
        return posOf(world, door);
    };

    // Half a turn a second for a second: the door is somewhere else entirely,
    // and still exactly a metre from its pivot.
    const glm::vec3 driven = swing(3.14f, 200.0f);
    CHECK(glm::length(driven - glm::vec3(1.0f, 10.0f, 0.0f)) > 1.0f);
    CHECK(glm::length(driven - glm::vec3(0.0f, 10.0f, 0.0f))
              == doctest::Approx(1.0f).epsilon(0.05));

    // A target with no force behind it is not a motor. This is the default, so
    // it is also the promise that no old scene starts moving by itself.
    const glm::vec3 idle = swing(3.14f, 0.0f);
    CHECK(glm::length(idle - glm::vec3(1.0f, 10.0f, 0.0f)) < 0.1f);
}

TEST_CASE("PhysicsWorld: a Slider motor drives the platform, and a zero target brakes it")
{
    const auto travel = [](float targetSpeed, float maxForce, bool push) {
        HorizonWorld world;
        const Entity rail     = makeStaticBox(world,  "Rail",     { 5.0f, 10.0f, 0.0f });
        const Entity carriage = makeDynamicBox(world, "Carriage", { 0.0f, 10.0f, 0.0f });
        jointTo(world, carriage, rail, JointType::Slider, glm::vec3(0.0f),
                { 1.0f, 0.0f, 0.0f });
        PhysicsWorld phys;
        phys.initialize(world);
        REQUIRE(phys.hasJoint(static_cast<uint32_t>(carriage)));
        CHECK(phys.setJointMotor(world, static_cast<uint32_t>(carriage),
                                 targetSpeed, maxForce));
        if (push)
            phys.addImpulse(static_cast<uint32_t>(carriage), { 20.0f, 0.0f, 0.0f });
        for (int i = 0; i < 60; ++i)
            phys.step(world, kDt);
        return posOf(world, carriage).x;
    };

    // A metre a second for a second, along the only axis it may travel.
    CHECK(travel(1.0f, 500.0f, false) == doctest::Approx(1.0f).epsilon(0.1));
    // Zero target WITH force is a brake: the shove is absorbed in a few
    // centimetres instead of carrying the carriage down the rail. That case is
    // the reason the force is the switch and not the target.
    CHECK(std::abs(travel(0.0f, 500.0f, true)) < 0.4f);
    // Zero force is off, so the same shove sends it down the rail.
    CHECK(travel(0.0f, 0.0f, true) > 1.0f);
}

TEST_CASE("PhysicsWorld: only a Hinge or a Slider has a motor")
{
    HorizonWorld world;
    const Entity hook = makeStaticBox(world,  "Hook", { 0.0f, 10.0f, 0.0f });
    const Entity load = makeDynamicBox(world, "Load", { 0.0f,  5.0f, 0.0f });
    jointTo(world, load, hook, JointType::Distance);

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(load)));

    // Refused, not ignored: a rope has no axis to drive along, and a silent
    // no-op reads to the author as "physics is broken".
    CHECK_FALSE(phys.setJointMotor(world, static_cast<uint32_t>(load), 1.0f, 100.0f));
    CHECK(jointOf(world, load).motorMaxForce == doctest::Approx(0.0f));

    // Neither is an entity with no joint at all.
    CHECK_FALSE(phys.setJointMotor(world, static_cast<uint32_t>(hook), 1.0f, 100.0f));
}

TEST_CASE("PhysicsWorld: a motor set on an unbuilt joint is applied when it is built")
{
    HorizonWorld world;
    const Entity rail     = makeStaticBox(world,  "Rail",     { 5.0f, 10.0f, 0.0f });
    const Entity carriage = makeDynamicBox(world, "Carriage", { 0.0f, 10.0f, 0.0f });
    jointTo(world, carriage, rail, JointType::Slider, glm::vec3(0.0f),
            { 1.0f, 0.0f, 0.0f });

    PhysicsWorld phys;   // NOT initialised yet — nothing is built
    CHECK(phys.setJointMotor(world, static_cast<uint32_t>(carriage), 1.0f, 500.0f));

    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(carriage)));
    for (int i = 0; i < 60; ++i)
        phys.step(world, kDt);
    CHECK(posOf(world, carriage).x == doctest::Approx(1.0f).epsilon(0.1));
}

TEST_CASE("PhysicsWorld: breakForce lets go, and says so exactly once")
{
    HorizonWorld world;
    const Entity hook = makeStaticBox(world,  "Hook", { 0.0f, 10.0f, 0.0f });
    const Entity load = makeDynamicBox(world, "Load", { 0.0f,  8.0f, 0.0f });
    jointTo(world, load, hook, JointType::Distance);
    // The rope carries the load's weight, m·g ≈ 9.8 N. Two newtons is not
    // enough rope for that.
    jointOf(world, load).breakForce = 2.0f;

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(load)));
    CHECK(phys.pollJointBroken().empty());

    std::vector<PhysicsWorld::CollisionEvent> broken;
    for (int i = 0; i < 60; ++i)
    {
        phys.step(world, kDt);
        for (const auto& ev : phys.pollJointBroken())
            broken.push_back(ev);
    }

    REQUIRE(broken.size() == 1);   // once, not once per step after it went
    CHECK(broken[0].entityA == static_cast<uint32_t>(load));
    CHECK(broken[0].entityB == static_cast<uint32_t>(hook));
    CHECK_FALSE(phys.hasJoint(static_cast<uint32_t>(load)));
    // The COMPONENT goes too — that is the whole difference between a joint that
    // broke and one that is merely not built: left behind, it would come back on
    // the next scene load and the next rebuild of either body.
    CHECK_FALSE(world.registry().all_of<JointComponent>(load));
    CHECK(posOf(world, load).y < 7.0f);   // and it is falling
}

TEST_CASE("PhysicsWorld: a rope strong enough for its load never breaks")
{
    HorizonWorld world;
    const Entity hook = makeStaticBox(world,  "Hook", { 0.0f, 10.0f, 0.0f });
    const Entity load = makeDynamicBox(world, "Load", { 0.0f,  8.0f, 0.0f });
    jointTo(world, load, hook, JointType::Distance);

    PhysicsWorld phys;
    phys.initialize(world);

    // Zero is the default and means "never", so the first run is also the
    // promise that no existing scene starts shedding its joints.
    for (int i = 0; i < 60; ++i)
    {
        phys.step(world, kDt);
        CHECK(phys.pollJointBroken().empty());
    }
    CHECK(phys.hasJoint(static_cast<uint32_t>(load)));

    // A thousand newtons is far more than this load can pull.
    CHECK(phys.setJointBreakForce(world, static_cast<uint32_t>(load), 1000.0f));
    for (int i = 0; i < 60; ++i)
    {
        phys.step(world, kDt);
        CHECK(phys.pollJointBroken().empty());
    }
    CHECK(phys.hasJoint(static_cast<uint32_t>(load)));
}

TEST_CASE("PhysicsWorld: a joint that was REMOVED did not break")
{
    // "It is gone" is not "it broke", and the code that plays a snapping sound
    // has no use for the first. Same line pollCollisionExit draws for a
    // destroyed body.
    HorizonWorld world;
    const Entity hook = makeStaticBox(world,  "Hook", { 0.0f, 10.0f, 0.0f });
    const Entity load = makeDynamicBox(world, "Load", { 0.0f,  8.0f, 0.0f });
    jointTo(world, load, hook, JointType::Distance);
    jointOf(world, load).breakForce = 1000.0f;

    PhysicsWorld phys;
    phys.initialize(world);
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(load)));

    CHECK(phys.removeJoint(world, static_cast<uint32_t>(load)));
    phys.step(world, kDt);
    CHECK(phys.pollJointBroken().empty());

    // Nor did destroying the far end of one.
    const Entity hook2 = makeStaticBox(world,  "Hook2", { 20.0f, 10.0f, 0.0f });
    const Entity load2 = makeDynamicBox(world, "Load2", { 20.0f,  8.0f, 0.0f });
    jointTo(world, load2, hook2, JointType::Distance);
    jointOf(world, load2).breakForce = 1000.0f;
    phys.addEntity(world, static_cast<uint32_t>(hook2));
    phys.addEntity(world, static_cast<uint32_t>(load2));
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(load2)));
    phys.removeEntity(static_cast<uint32_t>(hook2));
    phys.step(world, kDt);
    CHECK(phys.pollJointBroken().empty());
}

TEST_CASE("PhysicsWorld: clearing the world forgets the broken joints with it")
{
    HorizonWorld world;
    const Entity hook = makeStaticBox(world,  "Hook", { 0.0f, 10.0f, 0.0f });
    const Entity load = makeDynamicBox(world, "Load", { 0.0f,  8.0f, 0.0f });
    jointTo(world, load, hook, JointType::Distance);
    jointOf(world, load).breakForce = 2.0f;

    PhysicsWorld phys;
    phys.initialize(world);
    for (int i = 0; i < 30; ++i)
        phys.step(world, kDt);

    // An event nobody drained is about a scene that no longer exists; handing it
    // over after a clear would name entities of the previous level.
    phys.clear();
    CHECK(phys.pollJointBroken().empty());
}

TEST_CASE("PhysicsWorld: a rebuilt body keeps the motor and the break force its component names")
{
    // addEntity is documented idempotent: it tears the body down and puts a new
    // one up, which takes the joint with it. Everything the component says has
    // to come back, or a collider change mid-game silently stops the lift.
    HorizonWorld world;
    const Entity rail     = makeStaticBox(world,  "Rail",     { 5.0f, 10.0f, 0.0f });
    const Entity carriage = makeDynamicBox(world, "Carriage", { 0.0f, 10.0f, 0.0f });
    jointTo(world, carriage, rail, JointType::Slider, glm::vec3(0.0f),
            { 1.0f, 0.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);
    CHECK(phys.setJointMotor(world, static_cast<uint32_t>(carriage), 1.0f, 500.0f));

    phys.addEntity(world, static_cast<uint32_t>(carriage));   // the rebuild
    REQUIRE(phys.hasJoint(static_cast<uint32_t>(carriage)));

    for (int i = 0; i < 60; ++i)
        phys.step(world, kDt);
    CHECK(posOf(world, carriage).x == doctest::Approx(1.0f).epsilon(0.1));
}

TEST_CASE("PhysicsWorld: positive means THIS entity moves along its own Axis")
{
    // The one sign in the whole joint surface, and it is worth a test because
    // Jolt's own convention is the other way round: it measures the OTHER body
    // relative to this one, so a door told to open to 90° would have swung the
    // building. The axis is flipped once, where it crosses into Jolt, and the
    // limits and the motor both inherit that — which is the property this test
    // is really about, since a motor driving away from its own limit is the way
    // that mistake shows up in a level.

    // A hinge about +Y, arm out along +X: a POSITIVE rotation by the right-hand
    // rule takes +X towards -Z.
    {
        HorizonWorld world;
        const Entity frame = makeStaticBox(world,  "Frame", { 0.0f, 10.0f, 0.0f });
        const Entity door  = makeDynamicBox(world, "Door",  { 1.0f, 10.0f, 0.0f });
        jointTo(world, door, frame, JointType::Hinge, { -1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f });
        auto& jc = jointOf(world, door);
        jc.motorTarget   = 1.0f;
        jc.motorMaxForce = 200.0f;
        PhysicsWorld phys;
        phys.initialize(world);
        for (int i = 0; i < 20; ++i)
            phys.step(world, kDt);
        CHECK(posOf(world, door).z < -0.1f);
    }

    // And the limits agree: 0 to 45 degrees allows that same direction and stops
    // the other one dead.
    {
        const auto shoved = [](float impulseZ) {
            HorizonWorld world;
            const Entity frame = makeStaticBox(world,  "Frame", { 0.0f, 10.0f, 0.0f });
            const Entity door  = makeDynamicBox(world, "Door",  { 1.0f, 10.0f, 0.0f });
            jointTo(world, door, frame, JointType::Hinge, { -1.0f, 0.0f, 0.0f },
                    { 0.0f, 1.0f, 0.0f }, 0.0f, 45.0f);
            PhysicsWorld phys;
            phys.initialize(world);
            phys.addImpulse(static_cast<uint32_t>(door), { 0.0f, 0.0f, impulseZ });
            for (int i = 0; i < 60; ++i)
                phys.step(world, kDt);
            return posOf(world, door).z;
        };
        CHECK(shoved(-5.0f) < -0.2f);   // into the range, and it travels
        CHECK(shoved( 5.0f) < 0.05f);   // against the stop at 0, and it does not
    }

    // A slider is the same statement without the trigonometry: positive travel
    // is along the authored axis.
    {
        HorizonWorld world;
        const Entity rail     = makeStaticBox(world,  "Rail",     { 5.0f, 10.0f, 0.0f });
        const Entity carriage = makeDynamicBox(world, "Carriage", { 0.0f, 10.0f, 0.0f });
        jointTo(world, carriage, rail, JointType::Slider, glm::vec3(0.0f),
                { 1.0f, 0.0f, 0.0f }, 0.0f, 0.5f);
        PhysicsWorld phys;
        phys.initialize(world);
        phys.addImpulse(static_cast<uint32_t>(carriage), { 20.0f, 0.0f, 0.0f });
        for (int i = 0; i < 60; ++i)
            phys.step(world, kDt);
        CHECK(posOf(world, carriage).x == doctest::Approx(0.5f).epsilon(0.05));
    }
}
