#include "doctest.h"
#include <DebugDraw/DebugDraw.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <filesystem>

static constexpr float kDt      = 1.0f / 60.0f;
static constexpr int   kSteps2s = 120; // 2 s at 60 Hz

// ─── Shape selection ──────────────────────────────────────────────────────────

TEST_CASE("ColliderComponent: box shape — dynamic body falls under gravity")
{
    HorizonWorld world;
    Entity e = world.createEntity("BoxBody");

    TransformComponent t; t.position = { 0, 10, 0 }; t.scale = { 1, 1, 1 };
    world.addComponent(e, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
    world.addComponent(e, rb);
    ColliderComponent col;
    col.shape       = ColliderShape::Box;
    col.halfExtents = { 0.5f, 0.5f, 0.5f };
    world.addComponent(e, col);

    PhysicsWorld phys;
    phys.initialize(world);
    for (int i = 0; i < kSteps2s; ++i) phys.step(world, kDt);

    CHECK(world.registry().get<TransformComponent>(e).position.y < 5.0f);
}

TEST_CASE("ColliderComponent: sphere shape — dynamic body falls under gravity")
{
    HorizonWorld world;
    Entity e = world.createEntity("SphereBody");

    TransformComponent t; t.position = { 0, 10, 0 }; t.scale = { 1, 1, 1 };
    world.addComponent(e, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
    world.addComponent(e, rb);
    ColliderComponent col;
    col.shape  = ColliderShape::Sphere;
    col.radius = 0.5f;
    world.addComponent(e, col);

    PhysicsWorld phys;
    phys.initialize(world);
    for (int i = 0; i < kSteps2s; ++i) phys.step(world, kDt);

    CHECK(world.registry().get<TransformComponent>(e).position.y < 5.0f);
}

TEST_CASE("ColliderComponent: capsule shape — dynamic body falls under gravity")
{
    HorizonWorld world;
    Entity e = world.createEntity("CapsuleBody");

    TransformComponent t; t.position = { 0, 10, 0 }; t.scale = { 1, 1, 1 };
    world.addComponent(e, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
    world.addComponent(e, rb);
    ColliderComponent col;
    col.shape  = ColliderShape::Capsule;
    col.radius = 0.4f;
    col.height = 1.8f;
    world.addComponent(e, col);

    PhysicsWorld phys;
    phys.initialize(world);
    for (int i = 0; i < kSteps2s; ++i) phys.step(world, kDt);

    CHECK(world.registry().get<TransformComponent>(e).position.y < 5.0f);
}

TEST_CASE("ColliderComponent: static box does not move")
{
    HorizonWorld world;
    Entity e = world.createEntity("StaticBox");

    TransformComponent t; t.position = { 1, 2, 3 }; t.scale = { 1, 1, 1 };
    world.addComponent(e, t);
    RigidBodyComponent rb; rb.type = RigidBodyType::Static;
    world.addComponent(e, rb);
    ColliderComponent col;
    col.shape       = ColliderShape::Box;
    col.halfExtents = { 1.0f, 1.0f, 1.0f };
    world.addComponent(e, col);

    PhysicsWorld phys;
    phys.initialize(world);
    for (int i = 0; i < kSteps2s; ++i) phys.step(world, kDt);

    const auto& tr = world.registry().get<TransformComponent>(e);
    CHECK(tr.position.x == doctest::Approx(1.0f));
    CHECK(tr.position.y == doctest::Approx(2.0f));
    CHECK(tr.position.z == doctest::Approx(3.0f));
}

// ─── SceneSerializer round-trip ───────────────────────────────────────────────

TEST_CASE("ColliderComponent: JSON round-trip preserves all fields")
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "he_test_collider.hescene";

    HorizonWorld world;
    Entity e = world.createEntity("ColliderRoundTrip");
    world.addComponent(e, TransformComponent{});
    world.addComponent(e, RigidBodyComponent{});

    ColliderComponent col;
    col.shape       = ColliderShape::Capsule;
    col.radius      = 0.7f;
    col.height      = 2.5f;
    col.halfExtents = { 0.3f, 0.4f, 0.5f };
    col.isTrigger   = true;
    world.addComponent(e, col);

    SceneSerializer ser;
    REQUIRE(ser.save(world, file, SerializeFormat::JSON));

    HorizonWorld loaded;
    REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));

    auto& reg = loaded.registry();
    bool found = false;
    for (auto [ent, name] : reg.view<NameComponent>().each())
    {
        if (name.name != "ColliderRoundTrip") continue;
        found = true;
        auto* lc = reg.try_get<ColliderComponent>(ent);
        REQUIRE(lc != nullptr);
        CHECK(lc->shape  == ColliderShape::Capsule);
        CHECK(lc->radius == doctest::Approx(0.7f));
        CHECK(lc->height == doctest::Approx(2.5f));
        CHECK(lc->halfExtents.x == doctest::Approx(0.3f));
        CHECK(lc->halfExtents.y == doctest::Approx(0.4f));
        CHECK(lc->halfExtents.z == doctest::Approx(0.5f));
        CHECK(lc->isTrigger == true);
    }
    CHECK(found);

    std::filesystem::remove(file);
}

TEST_CASE("ColliderComponent: binary round-trip")
{
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "he_test_collider.hescene_bin";

    HorizonWorld world;
    Entity e = world.createEntity("BinCollider");
    world.addComponent(e, TransformComponent{});

    ColliderComponent col;
    col.shape  = ColliderShape::Sphere;
    col.radius = 1.5f;
    world.addComponent(e, col);

    SceneSerializer ser;
    REQUIRE(ser.save(world, file, SerializeFormat::Binary));

    HorizonWorld loaded;
    REQUIRE(ser.load(loaded, file, SerializeFormat::Binary));

    auto& reg = loaded.registry();
    bool found = false;
    for (auto [ent, name] : reg.view<NameComponent>().each())
    {
        if (name.name != "BinCollider") continue;
        found = true;
        auto* lc = reg.try_get<ColliderComponent>(ent);
        REQUIRE(lc != nullptr);
        CHECK(lc->shape  == ColliderShape::Sphere);
        CHECK(lc->radius == doctest::Approx(1.5f));
    }
    CHECK(found);

    std::filesystem::remove(file);
}

// ─── DebugDraw capsule helper (compile-only smoke test) ───────────────────────

TEST_CASE("DebugDrawBuffer: capsule produces lines without crash")
{
    DebugDrawBuffer dbg;
    dbg.capsule({ 0, 0, 0 }, 0.5f, 2.0f);
    CHECK(!dbg.lines().empty());
}
