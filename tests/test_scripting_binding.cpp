#include "doctest.h"
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/EngineApi.h>
#include <Types/TypeRegistry.h>
#include <cstdint>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <filesystem>   // the save-backed container round trips need a sandbox root
#include <system_error>

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

TEST_CASE("ScriptContext: registry-driven horizon.random.* (Lua)")
{
    // The Random group reaches Lua through the same registry-driven dispatcher.
    HorizonWorld world;
    ScriptContext ctx(world);
    auto& engine = ctx.engine();
    REQUIRE(engine.exec(
        "horizon.random.seed(99)\n"
        "_G._rr = horizon.random.range(3, 3)\n"       // degenerate → 3
        "_G._ri = horizon.random.rangeInt(4, 4)\n"    // degenerate → 4
        "_G._ct = horizon.random.chance(1.0) and 1 or 0\n"
        "_G._cf = horizon.random.chance(0.0) and 1 or 0\n"
        "_G._v  = horizon.random.value()\n"));
    CHECK(engine.getGlobalNumber("_rr") == doctest::Approx(3.0));
    CHECK(engine.getGlobalNumber("_ri") == doctest::Approx(4.0));
    CHECK(engine.getGlobalNumber("_ct") == doctest::Approx(1.0));
    CHECK(engine.getGlobalNumber("_cf") == doctest::Approx(0.0));
    const double v = engine.getGlobalNumber("_v");
    CHECK(v >= 0.0);
    CHECK(v < 1.0);
}

TEST_CASE("ScriptContext: registry-driven horizon.time.*/input.* (Lua)")
{
    // Push a frame's timing + input snapshot, then read it back through Lua.
    HE::api::time::reset();
    HE::api::time::advance(0.5f);
    HE::api::input::setKeysDown({ "Space" });
    HE::api::input::setMouse({ 7.0f, 8.0f }, { 0.0f, 0.0f }, (1u << 1), 0.0f); // right button

    HorizonWorld world;
    ScriptContext ctx(world);
    auto& engine = ctx.engine();
    REQUIRE(engine.exec(
        "_G._dt = horizon.time.deltaTime()\n"
        "_G._sp = horizon.input.keyDown('Space') and 1 or 0\n"
        "_G._rb = horizon.input.mouseButton(1) and 1 or 0\n"
        "_G._mx, _G._my = horizon.input.mousePosition()\n"));   // Vec2 → 2 return values
    CHECK(engine.getGlobalNumber("_dt") == doctest::Approx(0.5));
    CHECK(engine.getGlobalNumber("_sp") == doctest::Approx(1.0));
    CHECK(engine.getGlobalNumber("_rb") == doctest::Approx(1.0));
    CHECK(engine.getGlobalNumber("_mx") == doctest::Approx(7.0));
    CHECK(engine.getGlobalNumber("_my") == doctest::Approx(8.0));
    HE::api::input::clear();
}

TEST_CASE("ScriptContext: horizon.time.setTimeScale drives the game clock (Lua)")
{
    // The registry-driven binding is the whole point: nothing was hand-written
    // for these three, so this is what proves a new Time row reaches scripts.
    HE::api::time::reset();

    HorizonWorld world;
    ScriptContext ctx(world);
    auto& engine = ctx.engine();

    REQUIRE(engine.exec("horizon.time.setTimeScale(0.25)\n"
                        "_G._s = horizon.time.timeScale()\n"));
    CHECK(engine.getGlobalNumber("_s") == doctest::Approx(0.25));

    HE::api::time::advance(0.4f);
    REQUIRE(engine.exec("_G._dt = horizon.time.deltaTime()\n"
                        "_G._raw = horizon.time.unscaledDeltaTime()\n"));
    CHECK(engine.getGlobalNumber("_dt")  == doctest::Approx(0.1));  // slow motion
    CHECK(engine.getGlobalNumber("_raw") == doctest::Approx(0.4));  // real frame

    // The clamp lives in C++, so a script cannot fast-forward past the cap.
    REQUIRE(engine.exec("horizon.time.setTimeScale(99)\n"
                        "_G._s = horizon.time.timeScale()\n"));
    CHECK(engine.getGlobalNumber("_s") == doctest::Approx(HE::api::time::kMaxTimeScale));

    HE::api::time::reset();
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

TEST_CASE("ScriptContext: horizon.enums constants + horizon.structs constructors (Lua)")
{
    // User-defined types bootstrap from the TypeRegistry at context creation:
    // enum entries become plain int constants, struct constructors return the
    // boundary-table form (named fields + __type) seeded with the defaults.
    auto& reg = HE::TypeRegistry::instance();
    HE::EnumDef weapon;
    weapon.name = "Weapon"; weapon.assetPath = "Content/T/Weapon.hasset";
    weapon.entries = { { "Sword", 0 }, { "Bow", 7 } };
    reg.registerEnum(weapon);
    HE::StructDef stats;
    stats.name = "PlayerStats"; stats.assetPath = "Content/T/PlayerStats.hasset";
    {
        HE::StructField hp; hp.name = "hp"; hp.type = HorizonCode::PinType::Float;
        hp.defaultValue = HorizonCode::Value::ofFloat(100.0f);
        HE::StructField w; w.name = "weapon"; w.type = HorizonCode::PinType::Enum;
        w.typeName = weapon.assetPath; w.defaultValue.s = "Bow";
        // An array field with AUTHORED slots — they must reach the constructor.
        HE::StructField tags; tags.name = "tags"; tags.type = HorizonCode::PinType::String;
        tags.isArray = true;
        tags.defaultValue.isArray = true;
        tags.defaultValue.type = HorizonCode::PinType::String;
        tags.defaultValue.items = { HorizonCode::Value::ofString("starter"),
                                    HorizonCode::Value::ofString("melee") };
        stats.fields = { hp, w, tags };
    }
    reg.registerStruct(stats);

    {
        HorizonWorld world;
        ScriptContext ctx(world);
        auto& engine = ctx.engine();
        REQUIRE(engine.exec(
            "_G._bow = horizon.enums.Weapon.Bow\n"
            "local s = horizon.structs.PlayerStats()\n"
            "_G._hp = s.hp\n"
            "_G._w  = s.weapon\n"
            "_G._t  = s.__type == \"Content/T/PlayerStats.hasset\" and 1 or 0\n"
            // Array fields arrive with their authored slots, as a 1-based list.
            "_G._n    = #s.tags\n"
            "_G._tag1 = s.tags[1] == \"starter\" and 1 or 0\n"
            "_G._tag2 = s.tags[2] == \"melee\" and 1 or 0\n"
            // …and the list is not fixed-length: it grows like any Lua table.
            "table.insert(s.tags, \"extra\")\n"
            "_G._grown = #s.tags\n"
            // A second construction is a FRESH table (no shared default state).
            "local s2 = horizon.structs.PlayerStats()\n"
            "s2.hp = 1\n"
            "_G._fresh = horizon.structs.PlayerStats().hp\n"
            "_G._freshN = #horizon.structs.PlayerStats().tags\n"));
        CHECK(engine.getGlobalNumber("_bow") == doctest::Approx(7.0));
        CHECK(engine.getGlobalNumber("_hp") == doctest::Approx(100.0));
        CHECK(engine.getGlobalNumber("_w") == doctest::Approx(7.0));
        CHECK(engine.getGlobalNumber("_t") == doctest::Approx(1.0));
        CHECK(engine.getGlobalNumber("_fresh") == doctest::Approx(100.0));
        CHECK(engine.getGlobalNumber("_n") == doctest::Approx(2.0));
        CHECK(engine.getGlobalNumber("_tag1") == doctest::Approx(1.0));
        CHECK(engine.getGlobalNumber("_tag2") == doctest::Approx(1.0));
        CHECK(engine.getGlobalNumber("_grown") == doctest::Approx(3.0));
        CHECK(engine.getGlobalNumber("_freshN") == doctest::Approx(2.0));   // untouched by the grow
    }

    reg.removeType(weapon.assetPath);
    reg.removeType(stats.assetPath);
}

TEST_CASE("ScriptContext: Set and Map fields cross into Lua and back, in order")
{
    // Lua tables have no order of their own, so a HorizonCode map crosses as the
    // pairs PLUS a `__keys` array carrying the iteration order. This drives both
    // directions through the real boundary: the engine seeds a save field, Lua
    // reads it, rewrites it, and the engine reads what came back.
    namespace save = HE::api::save;
    using P = HorizonCode::PinType;
    using CK = HorizonCode::ContainerKind;
    using V = HorizonCode::Value;
    auto& reg = HE::TypeRegistry::instance();
    const char* kBag = "Content/T/LuaBag.hasset";
    const auto root = std::filesystem::temp_directory_path() / "he_lua_ctr_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    HE::api::fs::setSandboxRoot(root.string());
    save::close();

    HE::StructDef bag;
    bag.name = "LuaBag"; bag.assetPath = kBag;
    {
        HE::StructField tags; tags.name = "tags"; tags.type = P::String;
        tags.isArray = true; tags.container = CK::Set;
        tags.defaultValue.isArray = true; tags.defaultValue.container = CK::Set;
        HE::StructField ammo; ammo.name = "ammo"; ammo.type = P::Int;
        ammo.isArray = true; ammo.container = CK::Map; ammo.keyType = P::String;
        ammo.defaultValue.isArray = true; ammo.defaultValue.container = CK::Map;
        bag.fields = { tags, ammo };
    }
    reg.registerStruct(bag);

    ContentManager cm;
    {
        HE::StructDef tpl;
        HE::StructField b; b.name = "bag"; b.type = P::Struct; b.typeName = kBag;
        tpl.fields = { b };
        SaveGameTemplateAsset a;
        a.name = "LuaBagTemplate"; a.path = "mem://lua_bag_tpl";
        a.json = HE::TypeRegistry::structToJson(tpl);
        cm.registerSaveGameTemplate(std::move(a));
        save::setDefaultTemplate("mem://lua_bag_tpl");
    }
    REQUIRE(save::create("luabag", &cm));
    {
        V v = save::getStructV("bag");
        REQUIRE(v.items.size() == 2);
        v.items[0] = V::ofSet(P::String);
        v.items[0].items = { V::ofString("zeta"), V::ofString("alpha") };
        v.items[1] = V::ofMap(P::String, P::Int);
        v.items[1].keys  = { V::ofString("zeta"), V::ofString("alpha") };
        v.items[1].items = { V::ofInt(9), V::ofInt(1) };
        REQUIRE(save::setStructV("bag", v));
    }

    {
        HorizonWorld world;
        ScriptContext ctx(world);
        auto& engine = ctx.engine();
        REQUIRE(engine.exec(
            "local b = horizon.save.getStruct(\"bag\")\n"
            // A set arrives as an ordered 1-based list.
            "_G._t1 = b.tags[1] == \"zeta\" and 1 or 0\n"
            "_G._t2 = b.tags[2] == \"alpha\" and 1 or 0\n"
            // A map arrives keyed, AND carries its order in __keys.
            "_G._byKey = b.ammo[\"zeta\"]\n"
            "_G._k1 = b.ammo.__keys[1] == \"zeta\" and 1 or 0\n"
            "_G._k2 = b.ammo.__keys[2] == \"alpha\" and 1 or 0\n"
            // Write back: append to both, and keep __keys in step.
            "table.insert(b.tags, \"zeta\")\n"        // a duplicate — the set drops it
            "table.insert(b.tags, \"mid\")\n"
            "b.ammo[\"omega\"] = 5\n"
            "table.insert(b.ammo.__keys, \"omega\")\n"
            "horizon.save.setStruct(\"bag\", b)\n"));
        CHECK(engine.getGlobalNumber("_t1") == doctest::Approx(1.0));
        CHECK(engine.getGlobalNumber("_t2") == doctest::Approx(1.0));
        CHECK(engine.getGlobalNumber("_byKey") == doctest::Approx(9.0));
        CHECK(engine.getGlobalNumber("_k1") == doctest::Approx(1.0));
        CHECK(engine.getGlobalNumber("_k2") == doctest::Approx(1.0));
    }

    const V back = save::getStructV("bag");
    REQUIRE(back.items.size() == 2);
    CHECK(back.items[0].kind() == CK::Set);
    REQUIRE(back.items[0].items.size() == 3);          // the repeated "zeta" collapsed
    CHECK(back.items[0].items[0].s == "zeta");
    CHECK(back.items[0].items[1].s == "alpha");
    CHECK(back.items[0].items[2].s == "mid");
    CHECK(back.items[1].kind() == CK::Map);
    REQUIRE(back.items[1].keys.size() == 3);
    CHECK(back.items[1].keys[0].s == "zeta");          // __keys decided the order,
    CHECK(back.items[1].keys[1].s == "alpha");         // not Lua's hash order and
    CHECK(back.items[1].keys[2].s == "omega");         // not the alphabet
    REQUIRE(back.items[1].items.size() == 3);
    CHECK(back.items[1].items[0].i == 9);
    CHECK(back.items[1].items[2].i == 5);

    save::close();
    save::setDefaultTemplate("");
    reg.removeType(kBag);
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("ScriptContext: a Lua map table with no __keys still crosses deterministically")
{
    // A table a script built by hand carries no order. `pairs` order is a hash
    // artefact, so the boundary SORTS the keys instead — arbitrary, but the same
    // on every run, which is the property a map actually needs.
    namespace save = HE::api::save;
    using P = HorizonCode::PinType;
    using CK = HorizonCode::ContainerKind;
    auto& reg = HE::TypeRegistry::instance();
    const char* kBag = "Content/T/LuaBag2.hasset";
    const auto root = std::filesystem::temp_directory_path() / "he_lua_ctr_test2";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    HE::api::fs::setSandboxRoot(root.string());
    save::close();

    HE::StructDef bag;
    bag.name = "LuaBag2"; bag.assetPath = kBag;
    HE::StructField ammo; ammo.name = "ammo"; ammo.type = P::Int;
    ammo.isArray = true; ammo.container = CK::Map; ammo.keyType = P::String;
    ammo.defaultValue.isArray = true; ammo.defaultValue.container = CK::Map;
    bag.fields = { ammo };
    reg.registerStruct(bag);

    ContentManager cm;
    {
        HE::StructDef tpl;
        HE::StructField b; b.name = "bag"; b.type = P::Struct; b.typeName = kBag;
        tpl.fields = { b };
        SaveGameTemplateAsset a;
        a.name = "LuaBag2Template"; a.path = "mem://lua_bag2_tpl";
        a.json = HE::TypeRegistry::structToJson(tpl);
        cm.registerSaveGameTemplate(std::move(a));
        save::setDefaultTemplate("mem://lua_bag2_tpl");
    }
    REQUIRE(save::create("luabag2", &cm));

    {
        HorizonWorld world;
        ScriptContext ctx(world);
        REQUIRE(ctx.engine().exec(
            "horizon.save.setStruct(\"bag\", { __type = \"Content/T/LuaBag2.hasset\",\n"
            "  ammo = { zeta = 9, alpha = 1, mid = 5 } })\n"));
    }

    const HorizonCode::Value back = save::getStructV("bag");
    REQUIRE(back.items.size() == 1);
    REQUIRE(back.items[0].keys.size() == 3);
    CHECK(back.items[0].keys[0].s == "alpha");
    CHECK(back.items[0].keys[1].s == "mid");
    CHECK(back.items[0].keys[2].s == "zeta");
    CHECK(back.items[0].items[0].i == 1);   // values follow their own keys
    CHECK(back.items[0].items[2].i == 9);

    save::close();
    save::setDefaultTemplate("");
    reg.removeType(kBag);
    std::filesystem::remove_all(root, ec);
}
