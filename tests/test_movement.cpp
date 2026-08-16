#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/MovementSystem.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MovementComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>

namespace
{
	Entity makeMover(HorizonWorld& world)
	{
		const Entity e = world.createEntity("Mover");
		TransformComponent t; t.position = { 0.0f, 2.0f, 0.0f };
		world.addComponent(e, t);
		world.addComponent(e, CharacterControllerComponent{});
		ColliderComponent col;
		col.shape = ColliderShape::Capsule; col.radius = 0.35f; col.height = 1.8f;
		world.addComponent(e, col);
		world.addComponent(e, MovementComponent{});
		return e;
	}
}

TEST_CASE("MovementSystem: intent lasts exactly one frame")
{
	// move() reads like a verb, so one call must mean one frame of moving. If
	// the intent survived the tick, a single call would mean "walk forever" —
	// and the bug would look like a character that will not stop.
	HorizonWorld world;
	const Entity e = makeMover(world);

	HE::api::Ctx c{ &world, nullptr, nullptr };
	HE::api::locomotion::move(c, (HE::api::Entity)e, { 1.0f, 0.0f, 0.0f });
	CHECK(world.registry().get<MovementComponent>(e).moveInput.x == doctest::Approx(1.0f));

	MovementSystem::update(world, nullptr, 0.016f);
	CHECK(world.registry().get<MovementComponent>(e).moveInput == glm::vec3(0.0f));
}

TEST_CASE("MovementSystem: two move calls in one frame add up")
{
	// A caller that adds "forward" and "strafe" separately means both, not the
	// last one — so the intent accumulates rather than being assigned.
	HorizonWorld world;
	const Entity e = makeMover(world);

	HE::api::Ctx c{ &world, nullptr, nullptr };
	HE::api::locomotion::move(c, (HE::api::Entity)e, { 1.0f, 0.0f, 0.0f });
	HE::api::locomotion::move(c, (HE::api::Entity)e, { 0.0f, 0.0f, 1.0f });

	const auto& mv = world.registry().get<MovementComponent>(e);
	CHECK(mv.moveInput.x == doctest::Approx(1.0f));
	CHECK(mv.moveInput.z == doctest::Approx(1.0f));
}

TEST_CASE("MovementSystem: the vertical velocity is left to the physics")
{
	// The trap this exists for: writing a 0 into Y every frame does not stop a
	// character falling, it makes it sink at one gravity-step per tick. So the
	// system reads the controller's own Y back and preserves it — a character
	// walking off a ledge keeps accelerating downward.
	HorizonWorld world;
	// Ground, so the character has something to be supported by.
	{
		const Entity floor = world.createEntity("Floor");
		TransformComponent t; t.position = { 0.0f, -0.5f, 0.0f }; t.scale = { 20.0f, 1.0f, 20.0f };
		world.addComponent(floor, t);
		RigidBodyComponent rb; rb.type = RigidBodyType::Static;
		world.addComponent(floor, rb);
		ColliderComponent col;
		col.shape = ColliderShape::Box; col.halfExtents = { 10.0f, 0.5f, 10.0f };
		world.addComponent(floor, col);
	}
	const Entity e = makeMover(world);
	world.registry().get<TransformComponent>(e).position = { 0.0f, 12.0f, 0.0f };  // in the air

	PhysicsWorld phys;
	phys.initialize(world);

	HE::api::Ctx c{ &world, &phys, nullptr };
	float lastFall = 0.0f;
	for (int i = 0; i < 20; ++i)
	{
		// Ask to walk sideways every frame — the horizontal request must not
		// touch the fall.
		HE::api::locomotion::move(c, (HE::api::Entity)e, { 1.0f, 0.0f, 0.0f });
		MovementSystem::update(world, &phys, PhysicsWorld::kFixedDt);
		phys.step(world, PhysicsWorld::kFixedDt);
		lastFall = world.registry().get<CharacterControllerComponent>(e).velocity.y;
	}
	// After a third of a second of falling, roughly -3 m/s. The number that
	// matters is that it ACCELERATED — a clobbered Y would sit near -0.16.
	CHECK(lastFall < -1.0f);
}

TEST_CASE("EngineApi movement.*: derived from the controller, not stored twice")
{
	// speed/grounded are computed on read. There is deliberately no second copy
	// on MovementComponent, so there is nothing that can go stale against the
	// character controller the physics writes back to.
	HorizonWorld world;
	const Entity e = makeMover(world);

	auto& cc = world.registry().get<CharacterControllerComponent>(e);
	cc.velocity   = { 3.0f, -9.0f, 4.0f };   // horizontal 5, falling
	cc.isGrounded = true;

	HE::api::Ctx c{ &world, nullptr, nullptr };
	const auto id = (HE::api::Entity)e;
	CHECK(HE::api::movement::speed(c, id) == doctest::Approx(5.0f));          // ignores Y
	CHECK(HE::api::movement::verticalSpeed(c, id) == doctest::Approx(-9.0f));
	CHECK(HE::api::movement::isGrounded(c, id));
	CHECK(HE::api::movement::velocity(c, id).y == doctest::Approx(-9.0f));

	// An entity with no character controller answers neutrally instead of
	// crashing — the same tolerance every other row has.
	const Entity bare = world.createEntity("Bare");
	world.addComponent(bare, TransformComponent{});
	CHECK(HE::api::movement::speed(c, (HE::api::Entity)bare) == doctest::Approx(0.0f));
	CHECK_FALSE(HE::api::movement::isGrounded(c, (HE::api::Entity)bare));
}

TEST_CASE("EngineApi movement.*: forward/right are in the character's own frame")
{
	// What a 2D blend space wants. Doing it here keeps the trigonometry out of
	// every animator graph that needs it.
	HorizonWorld world;
	const Entity e = makeMover(world);
	auto& reg = world.registry();

	reg.get<CharacterControllerComponent>(e).velocity = { 0.0f, 0.0f, -4.0f };  // due -Z

	HE::api::Ctx c{ &world, nullptr, nullptr };
	const auto id = (HE::api::Entity)e;

	// Facing default (-Z): travelling -Z is straight forward.
	CHECK(HE::api::movement::forwardAmount(c, id) == doctest::Approx(4.0f));
	CHECK(HE::api::movement::rightAmount(c, id)   == doctest::Approx(0.0f).epsilon(0.01));

	// Turned 90°: the same world velocity is now pure strafe.
	reg.get<TransformComponent>(e).rotation.y = 90.0f;
	CHECK(HE::api::movement::forwardAmount(c, id) == doctest::Approx(0.0f).epsilon(0.01));
	CHECK(std::abs(HE::api::movement::rightAmount(c, id)) == doctest::Approx(4.0f).epsilon(0.01));
}
