#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/NavigationSystem.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <HorizonScene/Components/NavAgentComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <ContentManager/ContentManager.h>
#include <DebugDraw/DebugDraw.h>
#include <glm/glm.hpp>
#include <cmath>

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

TEST_CASE("NavigationSystem::extractNavMeshWireframe is empty when not baked")
{
    NavMeshComponent nmc;
    DebugDrawBuffer dbg;
    NavigationSystem::extractNavMeshWireframe(nmc, dbg);
    CHECK(dbg.lines().empty());
}

TEST_CASE("NavigationSystem::extractNavMeshWireframe emits lines after a successful bake")
{
    NavMeshComponent nmc;
    nmc.geometry = makeFlatFloor();
    REQUIRE(NavigationSystem::bake(nmc));

    DebugDrawBuffer dbg;
    NavigationSystem::extractNavMeshWireframe(nmc, dbg);
    CHECK(!dbg.lines().empty());

    // Every emitted segment should stay close to the flat floor's Y=0 plane
    // (Recast snaps the walkable surface to the voxel grid, so it is not
    // exactly 0 — within a couple of cellHeight steps is the right bound)
    // and within the floor's XZ footprint (half=5, generous margin).
    for (const auto& l : dbg.lines())
    {
        CHECK(std::abs(l.start.y) < nmc.config.cellHeight * 3.0f);
        CHECK(std::abs(l.end.y)   < nmc.config.cellHeight * 3.0f);
        CHECK(std::abs(l.start.x) < 6.0f);
        CHECK(std::abs(l.start.z) < 6.0f);
    }
}

// ─── An agent that is a body, not a ghost ─────────────────────────────────────
//
// Everything below runs the pair of ticks the real frame runs — navigation
// writes a velocity, the physics step turns it into motion — because that is the
// whole shape of the fix. A nav test that only calls update() would have passed
// before it too, back when the system wrote tc.position and left the collider at
// the spawn point.

namespace
{
    constexpr float kNavDt = 1.0f / 60.0f;

    // A world with a baked NavMesh, a physics floor beneath it, and one agent
    // built the way a walking NPC is: a character controller for the walking and
    // a kinematic body as the collider everything else sees.
    struct NavFixture
    {
        HorizonWorld world;
        PhysicsWorld phys;
        Entity       navMesh   = entt::null;
        Entity       agent     = entt::null;
        uint32_t     agentId   = 0;

        void build(const glm::vec3& spawn, bool withCharacter = true)
        {
            navMesh = world.createEntity("NavMeshEntity");
            {
                NavMeshComponent nmc;
                nmc.geometry = makeFlatFloor(10.0f);
                REQUIRE(NavigationSystem::bake(nmc));
                world.addComponent(navMesh, nmc);
            }

            // Top surface at y=0, where the nav mesh is: half-extents are
            // scale/2, so a 0.5-thick slab centred at -0.25 has its top at 0.
            Entity floor = world.createEntity("Floor");
            {
                TransformComponent t;
                t.position = { 0.0f, -0.25f, 0.0f };
                t.scale    = { 40.0f, 0.5f, 40.0f };
                world.addComponent(floor, t);
                RigidBodyComponent rb; rb.type = RigidBodyType::Static;
                world.addComponent(floor, rb);
            }

            agent = world.createEntity("Walker");
            {
                TransformComponent t;
                t.position = spawn;
                t.scale    = glm::vec3(1.0f);
                world.addComponent(agent, t);
                if (withCharacter)
                {
                    world.addComponent(agent, CharacterControllerComponent{});
                    RigidBodyComponent rb; rb.type = RigidBodyType::Kinematic;
                    world.addComponent(agent, rb);
                }
                NavAgentComponent na;
                na.speed        = 3.0f;
                na.stoppingDist = 0.3f;
                world.addComponent(agent, na);
            }
            agentId = static_cast<uint32_t>(agent);

            phys.initialize(world);
            // Settle onto the floor first: an agent still falling has a vertical
            // velocity that would muddle the horizontal measurements below.
            for (int i = 0; i < 90; ++i)
                phys.step(world, kNavDt);
        }

        // One frame, in the order SceneSystems runs it.
        void tick(int frames = 1)
        {
            for (int i = 0; i < frames; ++i)
            {
                NavigationSystem::update(world, kNavDt, &phys);
                phys.step(world, kNavDt);
            }
        }

        glm::vec3 pos() { return HE::worldPositionOf(world, agent); }
        NavAgentComponent& na() { return world.registry().get<NavAgentComponent>(agent); }

        float flatDistToTarget()
        {
            const glm::vec3 p = pos();
            const glm::vec3 t = na().targetPos;
            return glm::length(glm::vec2(t.x - p.x, t.z - p.z));
        }
    };
}

// THE CORE OF B4. Red twice over without the change: NavigationSystem::moveTo
// did not exist, so nothing outside the Inspector could start an agent, and the
// `moving` flag it sets was the flag update() bails on in its first line.
//
// MUTATION: in NavigationSystem::moveTo, drop `agent->moving = true` before the
// return — the route is found, the call still reports success, and the agent
// never takes a step. The distance check fails.
TEST_CASE("NavigationSystem: an agent told to move actually walks there")
{
    NavFixture f;
    f.build({ -6.0f, 1.0f, 0.0f });

    const glm::vec3 target{ 6.0f, 0.0f, 0.0f };
    // The answer comes back from THIS call, not from the next tick: the route is
    // searched here, which is what lets a script pick another destination in the
    // same breath when this one is unreachable.
    REQUIRE(NavigationSystem::moveTo(f.world, f.agent, target));
    REQUIRE(f.na().moving);
    REQUIRE(f.na().hasPath);

    const float startDist = f.flatDistToTarget();
    REQUIRE(startDist > 10.0f);

    f.tick(240);   // four seconds at 3 m/s — more than the twelve metres needs

    CHECK(f.flatDistToTarget() < startDist - 5.0f);
    // …and it arrived rather than merely set off. Arrival is what clears the
    // flags, so this is also the "does it ever stop" half.
    CHECK(f.flatDistToTarget() < 0.6f);
    CHECK_FALSE(f.na().moving);
}

// THE GHOST COLLIDER from the audit. Before the change the system wrote
// tc.position directly; with a physics body on the entity the collider stayed
// where it spawned (or the write-back undid the motion), so a raycast found the
// agent at a place it had visibly left.
//
// MUTATION: in NavigationSystem::update, comment out the
// `physics->setCharacterVelocity(...)` call in the character branch (keeping the
// `drivingCharacter = true` line and the `continue`) — the agent stands still and
// the raycast at the destination misses.
TEST_CASE("NavigationSystem: the agent's collider travels with it")
{
    NavFixture f;
    f.build({ -6.0f, 1.0f, 0.0f });

    const glm::vec3 down{ 0.0f, -1.0f, 0.0f };
    const glm::vec3 spawn = f.pos();

    // It starts out where a ray says it is — otherwise the check below proves
    // nothing about having MOVED.
    {
        const auto atSpawn = f.phys.raycast({ spawn.x, spawn.y + 5.0f, spawn.z }, down, 20.0f);
        REQUIRE(atSpawn.hit);
        REQUIRE(atSpawn.entityId == f.agentId);
    }

    REQUIRE(NavigationSystem::moveTo(f.world, f.agent, { 6.0f, 0.0f, 0.0f }));
    f.tick(240);

    const glm::vec3 now = f.pos();
    REQUIRE(now.x > 4.0f);   // it really is somewhere else

    const auto atNew = f.phys.raycast({ now.x, now.y + 5.0f, now.z }, down, 20.0f);
    CHECK(atNew.hit);
    CHECK(atNew.entityId == f.agentId);

    // And nothing of it is left behind at the spawn point — which is exactly
    // what the old transform-writing path did leave there.
    const auto stillAtSpawn = f.phys.raycast({ spawn.x, spawn.y + 5.0f, spawn.z }, down, 20.0f);
    if (stillAtSpawn.hit)
        CHECK(stillAtSpawn.entityId != f.agentId);
}

// MUTATION for the stop half: in NavigationSystem::update's `!agent.moving`
// branch, delete the `physics->setCharacterVelocity(id, ...)` zero-write — Jolt
// keeps the last velocity it was given and the agent glides on, so the
// "went nowhere after stopping" check fails.
//
// MUTATION for the distance half: in NavigationSystem::remainingDistance, return
// the straight line `glm::length(agent->targetPos - worldPositionOf(...))`
// instead of walking the waypoints — the shrink check still passes, so the
// second CHECK (that it is measured along the path) is the one that fails. Use
// the simpler `return -1.0f` to see the shrink check itself go red.
TEST_CASE("NavigationSystem: stopping halts the agent, and the distance left shrinks on the way")
{
    NavFixture f;
    f.build({ -6.0f, 1.0f, 0.0f });

    // No path, no measurement: -1 rather than 0, because 0 means arrived.
    CHECK(NavigationSystem::remainingDistance(f.world, f.agent) == doctest::Approx(-1.0f));

    REQUIRE(NavigationSystem::moveTo(f.world, f.agent, { 6.0f, 0.0f, 0.0f }));

    const float atStart = NavigationSystem::remainingDistance(f.world, f.agent);
    CHECK(atStart > 10.0f);

    f.tick(60);   // one second at 3 m/s
    const float afterOneSecond = NavigationSystem::remainingDistance(f.world, f.agent);
    CHECK(afterOneSecond < atStart - 2.0f);
    CHECK(afterOneSecond > 0.0f);

    // Stop where it stands.
    f.na().moving = false;
    const glm::vec3 whereItStopped = f.pos();

    // The first tick after stopping is the one that releases the velocity; the
    // rest prove it stays released rather than coasting.
    f.tick(120);
    const glm::vec3 twoSecondsLater = f.pos();
    CHECK(glm::length(glm::vec2(twoSecondsLater.x - whereItStopped.x,
                                twoSecondsLater.z - whereItStopped.z)) < 0.15f);
}

// The header comment NavAgentComponent.h has carried since it was written, and
// which the code never honoured: a pursuer that writes the player's position
// into targetPos every frame followed its first path to the end regardless.
//
// MUTATION: in NavigationSystem.cpp set `kRepathDistance = 1e9f` — the staleness
// test can never fire, the agent keeps walking to the original corner, and the
// check that it ends up near the NEW target fails.
TEST_CASE("NavigationSystem: moving the target plans a new path")
{
    NavFixture f;
    f.build({ -6.0f, 1.0f, -6.0f });

    REQUIRE(NavigationSystem::moveTo(f.world, f.agent, { -6.0f, 0.0f, 6.0f }));
    f.tick(30);   // half a second up the original leg

    const glm::vec3 firstTarget = f.na().targetPos;
    REQUIRE(f.na().hasPath);
    const glm::vec3 plannedFor = f.na().pathTarget;
    CHECK(plannedFor.z == doctest::Approx(firstTarget.z).epsilon(0.01));

    // Somewhere else entirely — written straight onto the component, the way a
    // chase script does it, WITHOUT calling moveTo. This is the path that used
    // to be ignored.
    const glm::vec3 newTarget{ 6.0f, 0.0f, -6.0f };
    f.na().targetPos = newTarget;

    f.tick(1);
    // The plan was redone for the new goal in the very next tick.
    CHECK(f.na().pathTarget.x == doctest::Approx(newTarget.x).epsilon(0.2));

    f.tick(300);
    const glm::vec3 end = f.pos();
    CHECK(glm::length(glm::vec2(end.x - newTarget.x, end.z - newTarget.z)) < 0.8f);
    // …and nowhere near where it was originally sent.
    CHECK(glm::length(glm::vec2(end.x - firstTarget.x, end.z - firstTarget.z)) > 5.0f);
}

// The other half of "nothing moves in a packaged game", and the only test here
// that goes through the real frame entry point. It locks TWO things that are
// invisible from a direct NavigationSystem::update call: that SceneSystems
// forwards the PhysicsWorld at all (without it every agent silently falls back
// to the ghost path and autoStart never fires), and that autoStart is what
// starts an agent where no Inspector button exists.
//
// MUTATION: in SceneSystems.cpp drop the `const_cast<PhysicsWorld*>(physics)`
// argument back to `NavigationSystem::update(world, dt)` — autoStart never
// latches, the agent never moves, and this test fails while every other test in
// this file still passes.
TEST_CASE("SceneSystems::tickWorld starts an autoStart agent and steers it through physics")
{
    NavFixture f;
    f.build({ -6.0f, 1.0f, 0.0f });

    ContentManager cm;

    auto& na = f.na();
    na.targetPos = { 6.0f, 0.0f, 0.0f };
    na.autoStart = true;
    // Nothing else says go: no Inspector, no script, no serialized `moving`.
    REQUIRE_FALSE(na.moving);

    const float startDist = f.flatDistToTarget();

    for (int i = 0; i < 240; ++i)
    {
        SceneSystems::tickWorld(f.world, cm, nullptr, glm::vec3(0.0f), kNavDt, &f.phys);
        f.phys.step(f.world, kNavDt);
    }

    CHECK(f.flatDistToTarget() < startDist - 5.0f);

    // It latched once. Stopping the agent afterwards must not send it off again
    // on the next tick, or a script could never keep an NPC still.
    f.na().moving    = false;
    f.na().autoStart = true;
    SceneSystems::tickWorld(f.world, cm, nullptr, glm::vec3(0.0f), kNavDt, &f.phys);
    CHECK_FALSE(f.na().moving);
}

// NOT a proof of anything new — it would have passed before the change too. It
// is here because the ghost path is still the right one for a bodyless agent (a
// camera dolly, a marker on a route), and nothing else covers that it survived
// the rewrite.
TEST_CASE("NavigationSystem: an agent with no body still moves by transform")
{
    NavFixture f;
    f.build({ -6.0f, 0.0f, 0.0f }, /*withCharacter=*/false);

    REQUIRE(NavigationSystem::moveTo(f.world, f.agent, { 6.0f, 0.0f, 0.0f }));
    for (int i = 0; i < 240; ++i)
        NavigationSystem::update(f.world, kNavDt, &f.phys);

    CHECK(f.pos().x > 4.0f);
}
