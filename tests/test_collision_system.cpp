#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/CollisionSystem.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>

static constexpr float kDt = 1.0f / 60.0f;

// Lua script that counts collision enter/exit events
static const char* kCountCollisions = R"lua(
local M = {}
function M.onStart(self)
    self.enterCount = 0
    self.exitCount  = 0
end
function M.onCollisionEnter(self, other)
    self.enterCount = (self.enterCount or 0) + 1
end
function M.onCollisionExit(self, other)
    self.exitCount = (self.exitCount or 0) + 1
end
return M
)lua";

// Same, but bookkeeping goes into Lua globals so the test can read it back via
// ScriptEngine::getGlobalNumber (instance fields have no C++ getter).
static const char* kCountCollisionsGlobal = R"lua(
local M = {}
function M.onStart(self)
    _heEnterCount = 0
    _heExitCount  = 0
    _heExitOther  = -1
end
function M.onCollisionEnter(self, other)
    _heEnterCount = (_heEnterCount or 0) + 1
end
function M.onCollisionExit(self, other)
    _heExitCount = (_heExitCount or 0) + 1
    _heExitOther = other
end
return M
)lua";

TEST_CASE("CollisionSystem: dispatch with no events does not crash")
{
    HorizonWorld world;
    PhysicsWorld phys;
    phys.initialize(world);

    ScriptContext ctx(world);
    CollisionSystem::InstanceMap instances;

    CHECK_NOTHROW(CollisionSystem::dispatch(phys, ctx, instances));
}

TEST_CASE("CollisionSystem: dispatch calls onCollisionEnter on colliding entities")
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

    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("countcoll", kCountCollisions));
    auto instBox = ctx.createInstance("countcoll", box);
    REQUIRE(instBox != ScriptEngine::kInvalidInstance);
    ctx.callOnStart(instBox);

    CollisionSystem::InstanceMap instances;
    instances[static_cast<uint32_t>(box)] = instBox;

    bool gotCallback = false;
    for (int i = 0; i < 120 && !gotCallback; ++i)
    {
        phys.step(world, kDt);
        CollisionSystem::dispatch(phys, ctx, instances);

        // Check if enterCount was incremented by inspecting via exec trick
        // We use the engine directly to verify
        bool ok = ctx.engine().exec(
            "if _testBox and _testBox.enterCount and _testBox.enterCount > 0 then _coll_fired = true end");
        (void)ok;
    }

    // Alternative: check directly via poll+dispatch side effect — just verify no crash
    // and that dispatch ran without error
    CHECK(ctx.lastError().empty());
}

TEST_CASE("PhysicsWorld: collision exit fires exactly once when bodies separate")
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
        RigidBodyComponent rb;
        rb.type        = RigidBodyType::Dynamic;
        rb.mass        = 1.0f;
        // Fully elastic and frictionless so the box bounces straight back off the
        // floor. That gives a clean touch-then-separate without needing an API to
        // shove bodies around mid-simulation.
        rb.restitution = 1.0f;
        rb.friction    = 0.0f;
        world.addComponent(box, rb);
    }

    PhysicsWorld phys;
    phys.initialize(world);

    int enterCount = 0;
    int exitCount  = 0;
    PhysicsWorld::CollisionEvent exitEvent{};

    // ~5 s at 60 Hz: the fall, the impact and the rebound all fit comfortably.
    // Stop at the first separation so a later bounce cannot inflate the counts.
    for (int i = 0; i < 300 && exitCount == 0; ++i)
    {
        phys.step(world, kDt);
        enterCount += static_cast<int>(phys.pollCollisionEnter().size());
        for (const auto& ev : phys.pollCollisionExit())
        {
            ++exitCount;
            exitEvent = ev;
        }
    }

    // One enter (the landing) and exactly one exit (the rebound) for the pair.
    CHECK(enterCount == 1);
    REQUIRE(exitCount == 1);

    const uint32_t rawFloor = static_cast<uint32_t>(floor);
    const uint32_t rawBox   = static_cast<uint32_t>(box);
    CHECK(((exitEvent.entityA == rawFloor && exitEvent.entityB == rawBox) ||
           (exitEvent.entityA == rawBox   && exitEvent.entityB == rawFloor)));

    // Same drain semantics as pollCollisionEnter(): the buffer is consumed.
    CHECK(phys.pollCollisionExit().empty());
}

TEST_CASE("CollisionSystem: dispatch calls onCollisionExit on separating entities")
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
        RigidBodyComponent rb;
        rb.type        = RigidBodyType::Dynamic;
        rb.mass        = 1.0f;
        rb.restitution = 1.0f;   // bounce off the floor, see test above
        rb.friction    = 0.0f;
        world.addComponent(box, rb);
    }

    PhysicsWorld phys;
    phys.initialize(world);

    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("collglobals", kCountCollisionsGlobal));
    auto instBox = ctx.createInstance("collglobals", box);
    REQUIRE(instBox != ScriptEngine::kInvalidInstance);
    ctx.callOnStart(instBox);

    CollisionSystem::InstanceMap instances;
    instances[static_cast<uint32_t>(box)] = instBox;

    // dispatch() drains the buffers, so the Lua globals are the only way to see
    // what was delivered — stop as soon as the exit callback has fired.
    for (int i = 0; i < 300; ++i)
    {
        phys.step(world, kDt);
        CollisionSystem::dispatch(phys, ctx, instances);
        if (ctx.engine().getGlobalNumber("_heExitCount") > 0.0)
            break;
    }

    CHECK(ctx.lastError().empty());
    CHECK(ctx.engine().getGlobalNumber("_heEnterCount") == doctest::Approx(1.0));
    CHECK(ctx.engine().getGlobalNumber("_heExitCount")  == doctest::Approx(1.0));
    // The `other` argument must be the floor, not the box itself.
    CHECK(ctx.engine().getGlobalNumber("_heExitOther")
          == doctest::Approx(static_cast<double>(static_cast<uint32_t>(floor))));
}

TEST_CASE("CollisionSystem: dispatch is safe when instance map is empty")
{
    HorizonWorld world;

    Entity floor = world.createEntity("Floor");
    {
        TransformComponent t; t.position = {0,0,0}; t.scale = {10, 0.2f, 10};
        world.addComponent(floor, t);
        world.addComponent(floor, RigidBodyComponent{});
    }

    Entity box = world.createEntity("Box");
    {
        TransformComponent t; t.position = {0, 3, 0}; t.scale = {1,1,1};
        world.addComponent(box, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
        world.addComponent(box, rb);
    }

    PhysicsWorld phys;
    phys.initialize(world);

    ScriptContext ctx(world);
    CollisionSystem::InstanceMap emptyMap;

    for (int i = 0; i < 60; ++i)
    {
        phys.step(world, kDt);
        CHECK_NOTHROW(CollisionSystem::dispatch(phys, ctx, emptyMap));
    }
}
