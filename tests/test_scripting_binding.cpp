#include "doctest.h"
#include <HorizonScene/ScriptContext.h>
#include <cstdint>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Script that reads its own position and stores it in self.x/y/z
static const char* kPositionReader = R"lua(
local M = {}
function M.onStart(self)
    local x, y, z = horizon.getPosition(self.entityId)
    self.x = x
    self.y = y
    self.z = z
end
return M
)lua";

// Script that moves the entity by (1,0,0) per onStart
static const char* kMover = R"lua(
local M = {}
function M.onStart(self)
    local x, y, z = horizon.getPosition(self.entityId)
    horizon.setPosition(self.entityId, x + 1, y, z)
end
return M
)lua";

// Script that accumulates dt into self.total each update
static const char* kDtAccum = R"lua(
local M = {}
function M.onStart(self)
    self.total = 0.0
end
function M.onUpdate(self, dt)
    self.total = self.total + dt
    horizon.setPosition(self.entityId, self.total, 0, 0)
end
return M
)lua";

// Script that spawns a child entity and records its id
static const char* kSpawner = R"lua(
local M = {}
function M.onStart(self)
    self.childId = horizon.spawn(self.entityId, "SpawnedChild")
end
return M
)lua";

// Script that reads entity name
static const char* kNameReader = R"lua(
local M = {}
function M.onStart(self)
    self.readName = horizon.getName(self.entityId)
end
return M
)lua";

// ─── Basic construction ───────────────────────────────────────────────────────

TEST_CASE("ScriptContext: constructs with a world")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    CHECK(ctx.loadedScriptCount() == 0);
    CHECK(ctx.instanceCount() == 0);
}

TEST_CASE("ScriptContext: loadScript delegates to engine")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    CHECK(ctx.loadScript("mover", kMover));
    CHECK(ctx.isScriptLoaded("mover"));
    CHECK(ctx.loadedScriptCount() == 1);
}

// ─── Entity binding ───────────────────────────────────────────────────────────

TEST_CASE("ScriptContext: createInstance stores entityId in self")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    // Use a simple script that reads self.entityId back via horizon.getName
    ctx.loadScript("nr", kNameReader);
    auto entity = world.createEntity("TestEntity");
    auto id = ctx.createInstance("nr", entity);
    CHECK(id != ScriptEngine::kInvalidInstance);
    CHECK(ctx.callOnStart(id)); // onStart reads name via horizon.getName(self.entityId)
    CHECK(ctx.lastError().empty());
}

// ─── Transform read/write ─────────────────────────────────────────────────────

TEST_CASE("ScriptContext: getPosition reads entity transform")
{
    HorizonWorld world;
    ScriptContext ctx(world);

    auto entity = world.createEntity("MovingEntity");
    TransformComponent tc;
    tc.position = {5.0f, 3.0f, 1.0f};
    world.registry().emplace<TransformComponent>(entity, tc);

    ctx.loadScript("reader", kPositionReader);
    auto id = ctx.createInstance("reader", entity);
    CHECK(ctx.callOnStart(id));
    CHECK(ctx.lastError().empty());

    // The script stored read values into self — verify no error = values were read
    // Additional verification via engine->exec:
    // Use the engine's exec to check what getPosition returns for the entity
    auto& engine = ctx.engine();
    std::string code = "function checkPos(eid)"
                       "  local x,y,z = horizon.getPosition(eid)"
                       "  _G._px = x; _G._py = y; _G._pz = z"
                       " end";
    CHECK(engine.exec(code));
    auto eId = static_cast<int64_t>(static_cast<uint32_t>(entity));
    engine.exec("checkPos(" + std::to_string(eId) + ")");
    CHECK(engine.getGlobalNumber("_px") == doctest::Approx(5.0));
    CHECK(engine.getGlobalNumber("_py") == doctest::Approx(3.0));
    CHECK(engine.getGlobalNumber("_pz") == doctest::Approx(1.0));
}

TEST_CASE("ScriptContext: registry-driven horizon.math.* (Lua)")
{
    // The Math library reaches Lua through the HE::api registry (no per-function
    // C shim) — proves the registry drives the Lua frontend.
    HorizonWorld world;
    ScriptContext ctx(world);
    auto& engine = ctx.engine();
    REQUIRE(engine.exec(
        "_G._c = horizon.math.clamp(5, 0, 3)\n"
        "_G._l = horizon.math.lerp(0, 10, 0.5)\n"
        "_G._m = horizon.math.max(2, 9)\n"
        "_G._s = horizon.math.sqrt(16)\n"
        "_G._d = horizon.math.distance(0, 0, 3, 4)\n"));   // vec2 spread: (0,0)-(3,4)=5
    CHECK(engine.getGlobalNumber("_c") == doctest::Approx(3.0));
    CHECK(engine.getGlobalNumber("_l") == doctest::Approx(5.0));
    CHECK(engine.getGlobalNumber("_m") == doctest::Approx(9.0));
    CHECK(engine.getGlobalNumber("_s") == doctest::Approx(4.0));
    CHECK(engine.getGlobalNumber("_d") == doctest::Approx(5.0));
}

TEST_CASE("ScriptContext: setPosition modifies entity transform")
{
    HorizonWorld world;
    ScriptContext ctx(world);

    auto entity = world.createEntity("Player");
    TransformComponent tc;
    tc.position = {0.0f, 0.0f, 0.0f};
    world.registry().emplace<TransformComponent>(entity, tc);

    ctx.loadScript("mover", kMover);
    auto id = ctx.createInstance("mover", entity);
    CHECK(ctx.callOnStart(id)); // onStart: setPosition(entity, 0+1, 0, 0)

    const auto& transform = world.registry().get<TransformComponent>(entity);
    CHECK(transform.position.x == doctest::Approx(1.0f));
    CHECK(transform.position.y == doctest::Approx(0.0f));
    CHECK(transform.position.z == doctest::Approx(0.0f));
}

TEST_CASE("ScriptContext: onUpdate accumulates position via dt")
{
    HorizonWorld world;
    ScriptContext ctx(world);

    auto entity = world.createEntity("Accum");
    world.registry().emplace<TransformComponent>(entity);

    ctx.loadScript("accum", kDtAccum);
    auto id = ctx.createInstance("accum", entity);
    CHECK(ctx.callOnStart(id));
    CHECK(ctx.callOnUpdate(id, 0.1f));
    CHECK(ctx.callOnUpdate(id, 0.1f));
    CHECK(ctx.callOnUpdate(id, 0.1f));

    const auto& t = world.registry().get<TransformComponent>(entity);
    CHECK(t.position.x == doctest::Approx(0.3f).epsilon(0.001f));
}

TEST_CASE("ScriptContext: setRotation and getRotation round-trip")
{
    HorizonWorld world;
    ScriptContext ctx(world);

    auto entity = world.createEntity("Rotator");
    world.registry().emplace<TransformComponent>(entity);

    auto& engine = ctx.engine();
    auto eId = static_cast<int64_t>(static_cast<uint32_t>(entity));
    // Set rotation via Lua, then read it back
    engine.exec("horizon.setRotation(" + std::to_string(eId) + ", 45, 90, 180)");
    engine.exec("_rx, _ry, _rz = horizon.getRotation(" + std::to_string(eId) + ")");

    CHECK(engine.getGlobalNumber("_rx") == doctest::Approx(45.0));
    CHECK(engine.getGlobalNumber("_ry") == doctest::Approx(90.0));
    CHECK(engine.getGlobalNumber("_rz") == doctest::Approx(180.0));

    const auto& t = world.registry().get<TransformComponent>(entity);
    CHECK(t.rotation.x == doctest::Approx(45.0f));
    CHECK(t.rotation.y == doctest::Approx(90.0f));
    CHECK(t.rotation.z == doctest::Approx(180.0f));
}

TEST_CASE("ScriptContext: setScale and getScale round-trip")
{
    HorizonWorld world;
    ScriptContext ctx(world);

    auto entity = world.createEntity("Scaler");
    world.registry().emplace<TransformComponent>(entity);

    auto& engine = ctx.engine();
    auto eId = static_cast<int64_t>(static_cast<uint32_t>(entity));
    engine.exec("horizon.setScale(" + std::to_string(eId) + ", 2, 3, 4)");
    engine.exec("_sx, _sy, _sz = horizon.getScale(" + std::to_string(eId) + ")");

    CHECK(engine.getGlobalNumber("_sx") == doctest::Approx(2.0));
    CHECK(engine.getGlobalNumber("_sy") == doctest::Approx(3.0));
    CHECK(engine.getGlobalNumber("_sz") == doctest::Approx(4.0));
}

// ─── Spawn / destroy ──────────────────────────────────────────────────────────

TEST_CASE("ScriptContext: spawn creates a new entity")
{
    HorizonWorld world;
    size_t before = 0;
    {
        auto view = world.registry().view<NameComponent>();
        before = std::distance(view.begin(), view.end());
    }

    ScriptContext ctx(world);
    auto entity = world.createEntity("Spawner");
    ctx.loadScript("spawner", kSpawner);
    auto id = ctx.createInstance("spawner", entity);
    CHECK(ctx.callOnStart(id)); // spawns "SpawnedChild"

    auto view = world.registry().view<NameComponent>();
    size_t after = std::distance(view.begin(), view.end());
    CHECK(after > before); // at least one new entity was created
}

TEST_CASE("ScriptContext: destroy removes entity from world")
{
    HorizonWorld world;
    auto entity = world.createEntity("ToDestroy");
    CHECK(world.registry().valid(entity));

    ScriptContext ctx(world);
    auto& engine = ctx.engine();
    auto eId = static_cast<int64_t>(static_cast<uint32_t>(entity));
    engine.exec("horizon.destroy(" + std::to_string(eId) + ")");

    CHECK(!world.registry().valid(entity));
}

// ─── getName ─────────────────────────────────────────────────────────────────

TEST_CASE("ScriptContext: getName returns entity name")
{
    HorizonWorld world;
    auto entity = world.createEntity("Protagonist");

    ScriptContext ctx(world);
    auto& engine = ctx.engine();
    auto eId = static_cast<int64_t>(static_cast<uint32_t>(entity));
    engine.exec("_nm = horizon.getName(" + std::to_string(eId) + ")");
    CHECK(engine.getGlobalString("_nm") == "Protagonist");
}

// ─── Multiple contexts ────────────────────────────────────────────────────────

TEST_CASE("ScriptContext: invalid entity operations are safe (no crash)")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    auto& engine = ctx.engine();
    // 99999 is almost certainly not a valid entity
    engine.exec("horizon.setPosition(99999, 1, 2, 3)");
    engine.exec("horizon.destroy(99999)");
    engine.exec("_n = horizon.getName(99999)");
    CHECK(engine.getGlobalString("_n") == ""); // safe default
}

// ─── Material parameters from scripts ──────────────────────────────────────────

TEST_CASE("ScriptContext: horizon.setMaterialParam drives the entity's material")
{
    // A node-graph material with two exposed params, referenced by an entity.
    ContentManager cm;
    MaterialAsset mat;
    mat.type = HE::AssetType::Material; mat.name = "scripted";
    mat.graphParamNames = { "K", "Tint" };
    mat.shaderParamData = { 0,0,0,0,  0,0,0,0 };
    const HE::UUID matId = cm.registerMaterial(std::move(mat));

    HorizonWorld world;
    ScriptContext ctx(world);
    ctx.setContentManager(&cm); // enable horizon.setMaterialParam / getMaterialParam

    auto entity = world.createEntity("Painted");
    world.registry().emplace<MaterialComponent>(entity, MaterialComponent{ matId });

    // onStart sets the scalar 'K'; onUpdate writes the vec4 'Tint' (module-table
    // style, matching the other binding tests — self.entityId is injected).
    const char* kPainter = R"lua(
local M = {}
function M.onStart(self) horizon.setMaterialParam(self.entityId, 'K', 0.7) end
function M.onUpdate(self, dt) horizon.setMaterialParam(self.entityId, 'Tint', 0.1, 0.9, 0.2, 1.0) end
return M
)lua";
    REQUIRE(ctx.loadScript("painter", kPainter));
    auto id = ctx.createInstance("painter", entity);
    REQUIRE(id != ScriptEngine::kInvalidInstance);

    CHECK(ctx.callOnStart(id));
    CHECK(cm.getMaterial(matId)->shaderParamData[0] == doctest::Approx(0.7f)); // 'K' slot 0

    CHECK(ctx.callOnUpdate(id, 0.016f));
    float tint[4] = { 0, 0, 0, 0 };
    REQUIRE(cm.getMaterialParam(matId, "Tint", tint));
    CHECK(tint[1] == doctest::Approx(0.9f)); // 'Tint' green
    CHECK(tint[3] == doctest::Approx(1.0f));

    // getMaterialParam round-trips back into the script.
    auto& engine = ctx.engine();
    auto eId = static_cast<int64_t>(static_cast<uint32_t>(entity));
    engine.exec("_k = ({horizon.getMaterialParam(" + std::to_string(eId) + ", 'K')})[1]");
    CHECK(engine.getGlobalNumber("_k") == doctest::Approx(0.7));
}

TEST_CASE("ScriptContext: setMaterialParam is safe without a ContentManager")
{
    HorizonWorld world;
    ScriptContext ctx(world); // no setContentManager → calls must no-op, not crash
    auto& engine = ctx.engine();
    engine.exec("_ok = horizon.setMaterialParam(1, 'K', 0.5) and 1 or 0");
    CHECK(engine.getGlobalNumber("_ok") == doctest::Approx(0.0)); // false, no crash
}
