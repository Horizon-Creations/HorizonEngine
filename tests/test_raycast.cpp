#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Build a simple world with a static 1×1×1 box at the origin.
static Entity buildBoxWorld(HorizonWorld& world)
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

// ─── Basic raycast ────────────────────────────────────────────────────────────

TEST_CASE("PhysicsWorld::raycast: ray hits static box from above")
{
    HorizonWorld world;
    Entity floor = buildBoxWorld(world);

    PhysicsWorld phys;
    phys.initialize(world);

    // Cast from (0, 5, 0) straight down
    auto hit = phys.raycast({ 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 10.0f);
    REQUIRE(hit.hit);
    // Hit point should be at the top surface of the unit box: y ≈ 0.5
    CHECK(hit.point.y == doctest::Approx(0.5f).epsilon(0.05f));
    CHECK(hit.distance == doctest::Approx(4.5f).epsilon(0.05f)); // 5 - 0.5
    CHECK(hit.entityId == static_cast<uint32_t>(floor));
    // Normal should point upward
    CHECK(hit.normal.y > 0.9f);
}

TEST_CASE("PhysicsWorld::raycast: ray misses returns hit=false")
{
    HorizonWorld world;
    buildBoxWorld(world);

    PhysicsWorld phys;
    phys.initialize(world);

    // Cast from (10, 0, 0) in the +X direction — misses the box at origin
    auto hit = phys.raycast({ 10.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, 100.0f);
    CHECK(!hit.hit);
}

TEST_CASE("PhysicsWorld::raycast: ray too short to reach body")
{
    HorizonWorld world;
    buildBoxWorld(world);

    PhysicsWorld phys;
    phys.initialize(world);

    // Box top is at y=0.5; ray starts at y=5, maxDistance=4 — won't reach it
    auto hit = phys.raycast({ 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 4.0f);
    CHECK(!hit.hit);
}

TEST_CASE("PhysicsWorld::raycast: uninitialised world returns no hit")
{
    HorizonWorld world;
    buildBoxWorld(world);

    PhysicsWorld phys; // not initialized
    auto hit = phys.raycast({ 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f });
    CHECK(!hit.hit);
}

TEST_CASE("PhysicsWorld::raycast: zero-length direction returns no hit")
{
    HorizonWorld world;
    buildBoxWorld(world);

    PhysicsWorld phys;
    phys.initialize(world);

    auto hit = phys.raycast({ 0.0f, 5.0f, 0.0f }, { 0.0f, 0.0f, 0.0f });
    CHECK(!hit.hit);
}

TEST_CASE("PhysicsWorld::raycast: sphere collider hit")
{
    HorizonWorld world;
    Entity ball = world.createEntity("Ball");
    TransformComponent t; t.position = { 0, 0, 0 };
    world.addComponent(ball, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Static;
    world.addComponent(ball, rb);
    ColliderComponent col; col.shape = ColliderShape::Sphere; col.radius = 1.0f;
    world.addComponent(ball, col);

    PhysicsWorld phys;
    phys.initialize(world);

    // Ray from above: sphere top is at y=1
    auto hit = phys.raycast({ 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f });
    REQUIRE(hit.hit);
    CHECK(hit.point.y == doctest::Approx(1.0f).epsilon(0.05f));
    CHECK(hit.entityId == static_cast<uint32_t>(ball));
}

// ─── Lua horizon.raycast binding ─────────────────────────────────────────────

TEST_CASE("ScriptContext::horizon.raycast: returns nil when no physics world")
{
    HorizonWorld world;
    ScriptContext ctx(world);

    CHECK(ctx.engine().exec(R"lua(
        local hit = horizon.raycast(0, 5, 0,  0, -1, 0,  10)
        _G._hitIsNil = (hit == nil) and 1 or 0
    )lua"));
    CHECK(ctx.engine().getGlobalNumber("_hitIsNil") == doctest::Approx(1.0));
}

TEST_CASE("ScriptContext::horizon.raycast: returns hit table when body intersected")
{
    HorizonWorld world;
    buildBoxWorld(world);

    PhysicsWorld phys;
    phys.initialize(world);

    ScriptContext ctx(world);
    ctx.setPhysicsWorld(&phys);

    CHECK(ctx.engine().exec(R"lua(
        local hit = horizon.raycast(0, 5, 0,  0, -1, 0,  10)
        _G._gotHit  = (hit ~= nil) and 1 or 0
        _G._hitY    = hit and hit.y or 0
        _G._hitDist = hit and hit.distance or 0
        _G._normY   = hit and hit.ny or 0
    )lua"));
    CHECK(ctx.engine().getGlobalNumber("_gotHit")  == doctest::Approx(1.0));
    CHECK(ctx.engine().getGlobalNumber("_hitY")    == doctest::Approx(0.5).epsilon(0.1));
    CHECK(ctx.engine().getGlobalNumber("_hitDist") == doctest::Approx(4.5).epsilon(0.1));
    CHECK(ctx.engine().getGlobalNumber("_normY")   >  0.9);
}

TEST_CASE("ScriptContext::horizon.raycast: returns nil on miss")
{
    HorizonWorld world;
    buildBoxWorld(world);

    PhysicsWorld phys;
    phys.initialize(world);

    ScriptContext ctx(world);
    ctx.setPhysicsWorld(&phys);

    CHECK(ctx.engine().exec(R"lua(
        local hit = horizon.raycast(100, 0, 0,  1, 0, 0,  100)
        _G._missIsNil = (hit == nil) and 1 or 0
    )lua"));
    CHECK(ctx.engine().getGlobalNumber("_missIsNil") == doctest::Approx(1.0));
}
