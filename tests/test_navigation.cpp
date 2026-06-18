#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/NavigationSystem.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <glm/glm.hpp>

// Build a flat 10×10 floor in the XZ plane (Y=0) for nav tests.
// Vertices: 4 corners; triangles: 2 covering the quad.
static NavMeshGeometry makeFlatFloor(float half = 5.0f, float y = 0.0f)
{
    NavMeshGeometry geo;
    geo.verts = {
        -half, y,  half,   // 0: -X +Z
         half, y,  half,   // 1: +X +Z
         half, y, -half,   // 2: +X -Z
        -half, y, -half,   // 3: -X -Z
    };
    geo.tris = {
        0, 1, 2,
        0, 2, 3,
    };
    return geo;
}

TEST_CASE("NavMeshComponent defaults")
{
    NavMeshComponent nmc;
    CHECK(!nmc.navMesh);
    CHECK(!nmc.navQuery);
    CHECK(nmc.isDirty);
    CHECK(nmc.config.cellSize     == doctest::Approx(0.3f));
    CHECK(nmc.config.walkableHeight == doctest::Approx(2.0f));
}

TEST_CASE("NavAgentComponent defaults")
{
    NavAgentComponent na;
    CHECK(na.speed        == doctest::Approx(3.5f));
    CHECK(na.stoppingDist == doctest::Approx(0.1f));
    CHECK(!na.moving);
    CHECK(!na.hasPath);
    CHECK(na.path.empty());
}

TEST_CASE("NavigationSystem::bake returns false on empty geometry")
{
    NavMeshComponent nmc;
    // No geometry provided
    CHECK(!NavigationSystem::bake(nmc));
    CHECK(!nmc.navMesh);
}

TEST_CASE("NavigationSystem::bake succeeds on flat floor geometry")
{
    NavMeshComponent nmc;
    nmc.geometry = makeFlatFloor();
    const bool ok = NavigationSystem::bake(nmc);
    CHECK(ok);
    CHECK((bool)nmc.navMesh);
    CHECK((bool)nmc.navQuery);
    CHECK(!nmc.isDirty);
}

TEST_CASE("NavigationSystem::update moves agent toward target")
{
    HorizonWorld world;

    // NavMesh entity
    Entity nmEntity = world.createEntity("NavMeshEntity");
    NavMeshComponent nmc;
    nmc.geometry = makeFlatFloor();
    REQUIRE(NavigationSystem::bake(nmc));
    world.addComponent(nmEntity, nmc);

    // Agent entity at (0,0,0), target at (3,0,0)
    Entity agentE = world.createEntity("Agent");
    TransformComponent tc; tc.position = {}; tc.rotation = {}; tc.scale = glm::vec3(1.0f);
    world.addComponent(agentE, tc);
    NavAgentComponent na;
    na.targetPos = glm::vec3(3.0f, 0.0f, 0.0f);
    na.speed     = 5.0f;
    na.moving    = true;
    world.addComponent(agentE, na);

    // Run several ticks
    for (int i = 0; i < 20; ++i)
        NavigationSystem::update(world, 0.1f);

    const auto& updTc = world.registry().get<TransformComponent>(agentE);
    // Agent must have moved closer to target
    CHECK(updTc.position.x > 0.5f);
}

TEST_CASE("NavigationSystem::update does not move agent when moving=false")
{
    HorizonWorld world;

    Entity nmEntity = world.createEntity("NavMeshEntity");
    NavMeshComponent nmc;
    nmc.geometry = makeFlatFloor();
    REQUIRE(NavigationSystem::bake(nmc));
    world.addComponent(nmEntity, nmc);

    Entity agentE = world.createEntity("Agent");
    TransformComponent tc; tc.position = {}; tc.rotation = {}; tc.scale = glm::vec3(1.0f);
    world.addComponent(agentE, tc);
    NavAgentComponent na;
    na.targetPos = glm::vec3(3.0f, 0.0f, 0.0f);
    na.speed     = 5.0f;
    na.moving    = false; // NOT moving
    world.addComponent(agentE, na);

    NavigationSystem::update(world, 1.0f);

    const auto& updTc = world.registry().get<TransformComponent>(agentE);
    CHECK(updTc.position.x == doctest::Approx(0.0f).epsilon(0.01f));
}
