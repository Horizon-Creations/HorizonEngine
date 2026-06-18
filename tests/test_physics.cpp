#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>

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
