#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
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
