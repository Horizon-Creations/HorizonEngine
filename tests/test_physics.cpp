#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/TransformHierarchy.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
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
