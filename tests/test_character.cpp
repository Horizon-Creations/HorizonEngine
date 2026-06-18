#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static Entity buildFloor(HorizonWorld& world)
{
    Entity floor = world.createEntity("Floor");
    TransformComponent t; t.position = { 0.0f, -0.5f, 0.0f }; t.scale = { 20.0f, 1.0f, 20.0f };
    world.addComponent(floor, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Static;
    world.addComponent(floor, rb);
    ColliderComponent col; col.shape = ColliderShape::Box; col.halfExtents = { 10.0f, 0.5f, 10.0f };
    world.addComponent(floor, col);
    return floor;
}

static Entity buildCharacter(HorizonWorld& world, glm::vec3 pos = { 0.0f, 2.0f, 0.0f })
{
    Entity e = world.createEntity("Player");
    TransformComponent t; t.position = pos;
    world.addComponent(e, t);
    CharacterControllerComponent cc;
    world.addComponent(e, cc);
    return e;
}

// ─── Tests ────────────────────────────────────────────────────────────────────

TEST_CASE("CharacterController: initialize does not crash")
{
    HorizonWorld world;
    buildFloor(world);
    buildCharacter(world);

    PhysicsWorld phys;
    CHECK_NOTHROW(phys.initialize(world));
}

TEST_CASE("CharacterController: entity registered after initialize")
{
    HorizonWorld world;
    buildFloor(world);
    Entity player = buildCharacter(world);

    PhysicsWorld phys;
    phys.initialize(world);

    // isCharacterGrounded returns false/true — either way should not crash
    uint32_t id = static_cast<uint32_t>(player);
    CHECK_NOTHROW(phys.isCharacterGrounded(id));
}

TEST_CASE("CharacterController: falls and lands on floor")
{
    HorizonWorld world;
    buildFloor(world);
    Entity player = buildCharacter(world, { 0.0f, 3.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    // Simulate ~0.5 s at fixed 1/60 steps
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i)
        phys.step(world, dt);

    auto& reg = world.registry();
    auto* t = reg.try_get<TransformComponent>(player);
    REQUIRE(t != nullptr);
    // Character should have fallen (y < 3.0)
    CHECK(t->position.y < 3.0f);
}

TEST_CASE("CharacterController: lands on floor and isGrounded becomes true")
{
    HorizonWorld world;
    buildFloor(world);
    Entity player = buildCharacter(world, { 0.0f, 3.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i)
        phys.step(world, dt);

    uint32_t id = static_cast<uint32_t>(player);
    CHECK(phys.isCharacterGrounded(id));
}

TEST_CASE("CharacterController: setCharacterVelocity moves character horizontally")
{
    HorizonWorld world;
    buildFloor(world);
    Entity player = buildCharacter(world, { 0.0f, 1.0f, 0.0f });

    PhysicsWorld phys;
    phys.initialize(world);

    // First let it land
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < 60; ++i)
        phys.step(world, dt);

    auto& reg = world.registry();
    auto* t = reg.try_get<TransformComponent>(player);
    REQUIRE(t != nullptr);
    float startX = t->position.x;

    // Move right at 5 m/s for 0.5 s
    uint32_t id = static_cast<uint32_t>(player);
    phys.setCharacterVelocity(id, { 5.0f, 0.0f, 0.0f });
    for (int i = 0; i < 30; ++i)
        phys.step(world, dt);

    CHECK(t->position.x > startX + 0.5f);
}

TEST_CASE("CharacterController: unknown entity returns false for isGrounded")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);

    CHECK_FALSE(phys.isCharacterGrounded(99999));
}

TEST_CASE("CharacterController: setCharacterVelocity on unknown entity is safe")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);

    CHECK_NOTHROW(phys.setCharacterVelocity(99999, { 1.0f, 0.0f, 0.0f }));
}

TEST_CASE("CharacterController: CharacterControllerComponent defaults are sane")
{
    CharacterControllerComponent cc;
    CHECK(cc.slopeLimit  == doctest::Approx(45.0f));
    CHECK(cc.stepHeight  == doctest::Approx(0.4f));
    CHECK(cc.mass        == doctest::Approx(70.0f));
    CHECK(cc.gravity     == doctest::Approx(9.81f));
    CHECK_FALSE(cc.isGrounded);
    CHECK(cc.velocity.x  == doctest::Approx(0.0f));
}

TEST_CASE("CharacterController: clear() removes all characters safely")
{
    HorizonWorld world;
    buildFloor(world);
    buildCharacter(world);

    PhysicsWorld phys;
    phys.initialize(world);
    CHECK_NOTHROW(phys.clear());
    CHECK_NOTHROW(phys.clear()); // double-clear is safe
}
