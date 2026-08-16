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

// A player as EntityHost::defaultComponents actually builds one: character
// controller AND a kinematic rigid body, on the same entity.
static Entity buildDefaultPlayer(HorizonWorld& world, glm::vec3 pos = { 0.0f, 2.0f, 0.0f })
{
    Entity e = world.createEntity("PlayerCharacter");
    TransformComponent t; t.position = pos;
    world.addComponent(e, t);
    world.addComponent(e, CharacterControllerComponent{});
    ColliderComponent col; col.shape = ColliderShape::Capsule; col.radius = 0.35f; col.height = 1.8f;
    world.addComponent(e, col);
    RigidBodyComponent rb; rb.type = RigidBodyType::Kinematic;
    world.addComponent(e, rb);
    return e;
}

TEST_CASE("CharacterController: game code can rotate a character with a kinematic body")
{
    // The character's kinematic body is a collision proxy that nothing ever moves,
    // so it keeps reporting its spawn pose. If the rigid-body write-back claimed
    // this entity, that stale pose would overwrite every rotation game code writes
    // — which is what kept scripts (and, later, the camera rig) from turning a
    // player. The character loop owns this transform; the body loop must skip it.
    HorizonWorld world;
    buildFloor(world);
    Entity player = buildDefaultPlayer(world);

    PhysicsWorld phys;
    phys.initialize(world);

    auto& t = world.registry().get<TransformComponent>(player);
    t.rotation = { 0.0f, 90.0f, 0.0f };

    phys.step(world, PhysicsWorld::kFixedDt);

    CHECK(t.rotation.y == doctest::Approx(90.0f));

    // And it must keep surviving — a single step could pass by luck of ordering.
    for (int i = 0; i < 10; ++i)
    {
        t.rotation.y = 90.0f + static_cast<float>(i);
        phys.step(world, PhysicsWorld::kFixedDt);
        CHECK(t.rotation.y == doctest::Approx(90.0f + static_cast<float>(i)));
    }
}

TEST_CASE("CharacterController: an entity with a rigid body still GETS a character")
{
	// The root cause behind "a default player cannot move". The character loop
	// skipped every entity that also had a RigidBodyComponent — the comment said
	// "skip as body", but this is the character loop, so what it skipped was the
	// character. And that combination is what EntityHost::defaultComponents
	// hands every PlayerCharacter, so a stock player was a bare kinematic body:
	// no walking, never grounded, movement.* reading zeros forever.
	HorizonWorld world;
	buildFloor(world);
	Entity player = buildDefaultPlayer(world, { 0.0f, 3.0f, 0.0f });

	PhysicsWorld phys;
	phys.initialize(world);

	const uint32_t id = static_cast<uint32_t>(player);
	for (int i = 0; i < 90; ++i) phys.step(world, PhysicsWorld::kFixedDt);

	// It fell and it landed — both only possible if a character exists at all.
	const auto& p = world.registry().get<TransformComponent>(player).position;
	INFO("landed at y=", p.y);
	CHECK(p.y < 2.0f);
	CHECK(phys.isCharacterGrounded(id));
}

TEST_CASE("CharacterController: the collision proxy follows the character")
{
	// A character carries BOTH a CharacterVirtual (what moves) and a kinematic
	// rigid body (what everything else collides with). Nothing used to move the
	// body: it was placed at initialize() and stayed. The character walked off
	// and left a ghost of itself at its spawn point — something other bodies
	// bumped into, and something a camera boom read as a wall.
	//
	// Measured through a RAYCAST rather than the transform, because the
	// transform is the character's and always right; the question is where the
	// BODY is, and only a query can answer that.
	HorizonWorld world;
	buildFloor(world);
	Entity player = buildDefaultPlayer(world, { 0.0f, 1.0f, 0.0f });

	PhysicsWorld phys;
	phys.initialize(world);

	// Before anything moves, the proxy is findable where the character spawned.
	// Cast along -Z→+Z so the floor (a wide, flat box below) is never in the way.
	REQUIRE(phys.raycast({ 0.0f, 1.0f, -5.0f }, { 0.0f, 0.0f, 1.0f }, 10.0f).hit);

	// Walk the character well clear of where it started.
	for (int i = 0; i < 60; ++i)
	{
		phys.setCharacterVelocity(static_cast<uint32_t>(player), { 6.0f, 0.0f, 0.0f });
		phys.step(world, PhysicsWorld::kFixedDt);
	}
	const auto& pt = world.registry().get<TransformComponent>(player).position;
	INFO("character ended at (", pt.x, ", ", pt.y, ", ", pt.z, ")");
	const float movedTo = pt.x;
	REQUIRE(movedTo > 2.0f);   // it really did leave

	// Nothing left behind at the spawn point…
	const auto ghost = phys.raycast({ 0.0f, 1.0f, -5.0f }, { 0.0f, 0.0f, 1.0f }, 3.0f);
	CHECK_FALSE(ghost.hit);

	// …and the proxy is where the character now is.
	const auto here = phys.raycast({ movedTo, 1.0f, -5.0f }, { 0.0f, 0.0f, 1.0f }, 10.0f);
	REQUIRE(here.hit);
	CHECK(here.entityId == static_cast<uint32_t>(player));
}

TEST_CASE("CharacterController: a plain kinematic body still gets its transform written back")
{
    // The guard above keys on CharacterControllerComponent, not on "kinematic" —
    // a kinematic body WITHOUT a controller must keep its existing write-back.
    HorizonWorld world;
    buildFloor(world);

    Entity platform = world.createEntity("Platform");
    TransformComponent t; t.position = { 0.0f, 5.0f, 0.0f };
    world.addComponent(platform, t);
    ColliderComponent col; col.shape = ColliderShape::Box; col.halfExtents = { 1.0f, 0.1f, 1.0f };
    world.addComponent(platform, col);
    RigidBodyComponent rb; rb.type = RigidBodyType::Kinematic;
    world.addComponent(platform, rb);

    PhysicsWorld phys;
    phys.initialize(world);

    auto& tr = world.registry().get<TransformComponent>(platform);
    tr.rotation = { 0.0f, 90.0f, 0.0f };
    phys.step(world, PhysicsWorld::kFixedDt);

    // Nothing pushed that rotation into the body, so the body's spawn pose wins.
    CHECK(tr.rotation.y == doctest::Approx(0.0f));
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
