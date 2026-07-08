#include "doctest.h"

// These tests only compile when the engine was built with CPython embedding.
// Everything runs through the PyScriptBackend public API (no <Python.h> here),
// which also proves the interpreter boots and executes end to end.
#ifdef HE_HAVE_PYTHON

#include <HorizonScene/PyScriptBackend.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <limits>
#include <memory>

// ─── Test scripts ───────────────────────────────────────────────────────────

// Writes a fixed position on start, integrates dt on update — exercises the
// horizon module, self.entity_id injection, and ScriptApi delegation.
static const char* kMover = R"py(
import horizon

class Mover(horizon.Behavior):
    speed  = 2.5
    active = True
    label  = "hero"
    count  = 3

    def on_start(self):
        horizon.setPosition(self.entity_id, 7.0, 8.0, 9.0)

    def on_update(self, dt):
        x, y, z = horizon.getPosition(self.entity_id)
        horizon.setPosition(self.entity_id, x + dt, y, z)
)py";

// on_start echoes the (possibly injected) speed into the entity's X position.
static const char* kSpeedEcho = R"py(
import horizon

class Echo(horizon.Behavior):
    speed = 1.0
    def on_start(self):
        horizon.setPosition(self.entity_id, self.speed, 0.0, 0.0)
)py";

static const char* kBadSyntax = "class Broken(:\n    pass\n";
static const char* kNoBehavior = "import horizon\nclass Plain:\n    pass\n";
static const char* kRaises     = R"py(
import horizon
class Boom(horizon.Behavior):
    def on_start(self):
        raise ValueError("kaboom")
)py";

// Helper: an entity with a zeroed transform.
static entt::entity makeEntity(HorizonWorld& world, const char* name)
{
    auto e = world.createEntity(name);
    TransformComponent tc;
    tc.position = {0.0f, 0.0f, 0.0f};
    world.registry().emplace<TransformComponent>(e, tc);
    return e;
}

// ─── Boot / availability ────────────────────────────────────────────────────

TEST_CASE("PyScriptBackend: available and constructs")
{
    CHECK(PyScriptBackend::available());
    HorizonWorld world;
    PyScriptBackend py(world);
    CHECK(py.loadedScriptCount() == 0);
    CHECK(py.instanceCount() == 0);
}

TEST_CASE("PyScriptBackend: loads a Behavior subclass")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    CHECK(py.loadScript("mover", kMover));
    CHECK(py.isScriptLoaded("mover"));
    CHECK(py.loadedScriptCount() == 1);
    CHECK(py.lastError().empty());
}

TEST_CASE("PyScriptBackend: rejects a syntax error")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    CHECK_FALSE(py.loadScript("bad", kBadSyntax));
    CHECK_FALSE(py.isScriptLoaded("bad"));
    CHECK_FALSE(py.lastError().empty());
}

TEST_CASE("PyScriptBackend: rejects source with no Behavior subclass")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    CHECK_FALSE(py.loadScript("plain", kNoBehavior));
    CHECK_FALSE(py.lastError().empty());
}

// ─── Lifecycle + horizon API round-trip ─────────────────────────────────────

TEST_CASE("PyScriptBackend: on_start runs and reaches the world")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    REQUIRE(py.loadScript("mover", kMover));

    auto e  = makeEntity(world, "Hero");
    auto id = py.createInstance("mover", static_cast<uint32_t>(e));
    REQUIRE(id != IScriptBackend::kInvalidInstance);
    CHECK(py.instanceCount() == 1);

    CHECK(py.callOnStart(id));
    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(7.0f));
    CHECK(t.position.y == doctest::Approx(8.0f));
    CHECK(t.position.z == doctest::Approx(9.0f));
}

TEST_CASE("PyScriptBackend: on_update integrates dt")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    REQUIRE(py.loadScript("mover", kMover));

    auto e  = makeEntity(world, "Hero");
    auto id = py.createInstance("mover", static_cast<uint32_t>(e));
    REQUIRE(py.callOnStart(id));           // → x = 7
    CHECK(py.callOnUpdate(id, 0.5f));      // → x = 7.5
    CHECK(py.callOnUpdate(id, 0.25f));     // → x = 7.75

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(7.75f));
}

TEST_CASE("PyScriptBackend: missing handler is a no-op success")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    REQUIRE(py.loadScript("echo", kSpeedEcho)); // no on_update defined
    auto e  = makeEntity(world, "E");
    auto id = py.createInstance("echo", static_cast<uint32_t>(e));
    CHECK(py.callOnUpdate(id, 0.1f));       // no on_update → true, no error
    CHECK(py.lastError().empty());
}

TEST_CASE("PyScriptBackend: a raising handler reports the error")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    REQUIRE(py.loadScript("boom", kRaises));
    auto e  = makeEntity(world, "E");
    auto id = py.createInstance("boom", static_cast<uint32_t>(e));
    CHECK_FALSE(py.callOnStart(id));
    CHECK(py.lastError().find("kaboom") != std::string::npos);
}

// ─── Properties ─────────────────────────────────────────────────────────────

TEST_CASE("PyScriptBackend: getScriptProperties reads typed class attributes")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    REQUIRE(py.loadScript("mover", kMover));

    auto defs = py.getScriptProperties("mover");
    auto find = [&](const char* n) -> const ScriptPropDef* {
        for (auto& d : defs) if (d.name == n) return &d;
        return nullptr;
    };
    CHECK(defs.size() == 4);                 // speed, active, label, count (not methods/entity_id)

    REQUIRE(find("speed"));
    CHECK(find("speed")->defaultVal.type == ScriptPropType::Float);
    CHECK(find("speed")->defaultVal.f == doctest::Approx(2.5f));

    REQUIRE(find("active"));
    CHECK(find("active")->defaultVal.type == ScriptPropType::Bool);
    CHECK(find("active")->defaultVal.b == true);

    REQUIRE(find("count"));
    CHECK(find("count")->defaultVal.type == ScriptPropType::Int);
    CHECK(find("count")->defaultVal.i == 3);

    REQUIRE(find("label"));
    CHECK(find("label")->defaultVal.type == ScriptPropType::String);
    CHECK(find("label")->defaultVal.s == "hero");
}

TEST_CASE("PyScriptBackend: injectProperties overrides before on_start")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    REQUIRE(py.loadScript("echo", kSpeedEcho));

    auto e  = makeEntity(world, "E");
    auto id = py.createInstance("echo", static_cast<uint32_t>(e));

    ScriptPropValue v; v.type = ScriptPropType::Float; v.f = 42.0f;
    py.injectProperties(id, {{"speed", v}});
    REQUIRE(py.callOnStart(id));            // echoes self.speed into x

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(42.0f));
}

// ─── Hot reload ─────────────────────────────────────────────────────────────

TEST_CASE("PyScriptBackend: hotReload swaps behavior, preserves instance data")
{
    HorizonWorld world;
    PyScriptBackend py(world);

    static const char* v1 = R"py(
import horizon
class S(horizon.Behavior):
    def on_start(self):
        self.hp = 100
    def on_update(self, dt):
        horizon.setPosition(self.entity_id, self.hp, 0.0, 0.0)
)py";
    static const char* v2 = R"py(
import horizon
class S(horizon.Behavior):
    def on_update(self, dt):
        horizon.setPosition(self.entity_id, self.hp + 1, 0.0, 0.0)
)py";

    REQUIRE(py.loadScript("s", v1));
    auto e  = makeEntity(world, "E");
    auto id = py.createInstance("s", static_cast<uint32_t>(e));
    REQUIRE(py.callOnStart(id));            // self.hp = 100 (data)

    CHECK(py.hotReloadScript("s", v2));     // new class, keep __dict__
    CHECK(py.callOnUpdate(id, 0.0f));       // v2: x = hp + 1

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(101.0f)); // 100 preserved + v2 code ran
}

// ─── Teardown ───────────────────────────────────────────────────────────────

TEST_CASE("PyScriptBackend: unload destroys instances")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    REQUIRE(py.loadScript("mover", kMover));
    auto e = makeEntity(world, "E");
    py.createInstance("mover", static_cast<uint32_t>(e));
    CHECK(py.instanceCount() == 1);
    py.unloadScript("mover");
    CHECK_FALSE(py.isScriptLoaded("mover"));
    CHECK(py.instanceCount() == 0);
}

// ─── Routing through ScriptContext (tag → route → untag) ─────────────────────
// These exercise the language-tag path that the backend-direct tests above do
// NOT: a missing untag decode would pass every Lua test yet break only Python.

// Lua counterpart of kMover's on_start: sets X to 3.
static const char* kLuaSetX = R"lua(
local M = {}
function M.onStart(self)
    horizon.setPosition(self.entityId, 3, 0, 0)
end
return M
)lua";

TEST_CASE("ScriptContext: Python script routes and reaches the world")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("pym", kMover, ScriptLanguage::Python));
    CHECK(ctx.isScriptLoaded("pym"));

    auto e  = makeEntity(world, "Hero");
    auto id = ctx.createInstance("pym", e);
    REQUIRE(id != ScriptEngine::kInvalidInstance);

    REQUIRE(ctx.callOnStart(id));              // → (7,8,9)
    CHECK(ctx.callOnUpdate(id, 0.5f));         // → x 7.5 : proves the id untagged back to the Python instance
    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(7.5f));
    CHECK(t.position.z == doctest::Approx(9.0f));
}

// Behavior that writes math-library results into its entity's position.
static const char* kPyMath = R"py(
import horizon

class MathUser(horizon.Behavior):
    def on_start(self):
        c  = horizon.math.clamp(5, 0, 3)
        l  = horizon.math.lerp(0, 10, 0.5)
        mx = horizon.math.max(2, 9)
        horizon.setPosition(self.entity_id, c, l, mx)
)py";

TEST_CASE("ScriptContext: registry-driven horizon.math.* (Python)")
{
    // The Math library reaches Python through the HE::api registry (via the
    // generic _engineCall dispatcher) — proves the registry drives the Python
    // frontend, matching the Lua horizon.math.* exposure.
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("pymath", kPyMath, ScriptLanguage::Python));

    auto e  = makeEntity(world, "MathHero");
    auto id = ctx.createInstance("pymath", e);
    REQUIRE(id != ScriptEngine::kInvalidInstance);
    REQUIRE(ctx.callOnStart(id));

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(3.0f));   // clamp(5, 0, 3)
    CHECK(t.position.y == doctest::Approx(5.0f));   // lerp(0, 10, 0.5)
    CHECK(t.position.z == doctest::Approx(9.0f));   // max(2, 9)
}

TEST_CASE("ScriptContext: Lua and Python coexist without id collision")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("pym", kMover,   ScriptLanguage::Python));
    REQUIRE(ctx.loadScript("lm",  kLuaSetX, ScriptLanguage::Lua));
    CHECK(ctx.loadedScriptCount() == 2);

    auto ePy  = makeEntity(world, "Py");
    auto eLua = makeEntity(world, "Lua");
    auto idPy  = ctx.createInstance("pym", ePy);
    auto idLua = ctx.createInstance("lm",  eLua);
    REQUIRE(idPy  != ScriptEngine::kInvalidInstance);
    REQUIRE(idLua != ScriptEngine::kInvalidInstance);
    CHECK(idPy != idLua);
    CHECK(ctx.instanceCount() == 2);

    REQUIRE(ctx.callOnStart(idPy));   // Python → (7,8,9)
    REQUIRE(ctx.callOnStart(idLua));  // Lua    → (3,0,0)
    CHECK(ctx.callOnUpdate(idPy, 0.5f)); // Python instance advances, Lua untouched

    CHECK(world.registry().get<TransformComponent>(ePy).position.x  == doctest::Approx(7.5f));
    CHECK(world.registry().get<TransformComponent>(eLua).position.x == doctest::Approx(3.0f));
}

TEST_CASE("ScriptContext: Python runtime error surfaces via lastError")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("boom", kRaises, ScriptLanguage::Python));
    auto e  = makeEntity(world, "E");
    auto id = ctx.createInstance("boom", e);
    CHECK_FALSE(ctx.callOnStart(id));
    CHECK(ctx.lastError().find("kaboom") != std::string::npos);
}

TEST_CASE("ScriptContext: unloaded Python name falls back safely")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    // Nothing loaded: neither backend owns the name; must not crash.
    CHECK_FALSE(ctx.isScriptLoaded("ghost"));
    auto e  = makeEntity(world, "E");
    auto id = ctx.createInstance("ghost", e); // routes to Lua (default), fails cleanly
    CHECK(id == ScriptEngine::kInvalidInstance);
}

// ─── Regression tests for adversarial-review findings ────────────────────────

// #1: a valid str that fails strict UTF-8 (lone surrogate) must not crash the
// property scan (PyUnicode_AsUTF8 → NULL → std::string(NULL) was a strlen crash).
TEST_CASE("PyScriptBackend: non-UTF8 string property is skipped, not a crash")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    static const char* kSurrogate = R"py(
import horizon
class P(horizon.Behavior):
    good = 5
    tag  = '\udce9'
)py";
    REQUIRE(py.loadScript("p", kSurrogate));
    auto defs = py.getScriptProperties("p");   // must return, not segfault
    bool hasGood = false, hasTag = false;
    for (auto& d : defs) { if (d.name == "good") hasGood = true; if (d.name == "tag") hasTag = true; }
    CHECK(hasGood);       // the encodable property survives
    CHECK_FALSE(hasTag);  // the un-encodable one is dropped
}

// #2: int class attributes outside int32 must clamp (not wrap to -1 via C cast)
// and must not leave a pending Python error.
TEST_CASE("PyScriptBackend: out-of-range int properties clamp instead of wrapping")
{
    HorizonWorld world;
    PyScriptBackend py(world);
    static const char* kBigInts = R"py(
import horizon
class Q(horizon.Behavior):
    big   = 10**40
    col   = 0xFFFFFFFF
    small = 7
)py";
    REQUIRE(py.loadScript("q", kBigInts));
    auto defs = py.getScriptProperties("q");
    auto find = [&](const char* n) -> const ScriptPropDef* {
        for (auto& d : defs) if (d.name == n) return &d; return nullptr;
    };
    REQUIRE(find("small")); CHECK(find("small")->defaultVal.i == 7);
    REQUIRE(find("col"));   CHECK(find("col")->defaultVal.i == std::numeric_limits<int>::max());
    REQUIRE(find("big"));   CHECK(find("big")->defaultVal.i == 0); // overflowed long long → 0, no wrap
    // A follow-up call proves no stale error leaked from the overflow.
    CHECK(py.getScriptProperties("q").size() == defs.size());
}

// #4: two entities sharing a moduleName across languages each route to their own
// backend when the caller passes the language (createInstance/isScriptLoaded 3-arg).
TEST_CASE("ScriptContext: same moduleName in two languages routes by language")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("shared", kLuaSetX, ScriptLanguage::Lua));
    REQUIRE(ctx.loadScript("shared", kMover,   ScriptLanguage::Python));
    CHECK(ctx.isScriptLoaded("shared", ScriptLanguage::Lua));
    CHECK(ctx.isScriptLoaded("shared", ScriptLanguage::Python));

    auto eL = makeEntity(world, "L");
    auto eP = makeEntity(world, "P");
    auto idL = ctx.createInstance("shared", eL, ScriptLanguage::Lua);
    auto idP = ctx.createInstance("shared", eP, ScriptLanguage::Python);
    REQUIRE(idL != ScriptEngine::kInvalidInstance);
    REQUIRE(idP != ScriptEngine::kInvalidInstance);
    CHECK(idL != idP);

    REQUIRE(ctx.callOnStart(idL));  // Lua kLuaSetX  → x = 3
    REQUIRE(ctx.callOnStart(idP));  // Python kMover → (7,8,9)
    CHECK(world.registry().get<TransformComponent>(eL).position.x == doctest::Approx(3.0f));
    CHECK(world.registry().get<TransformComponent>(eP).position.x == doctest::Approx(7.0f));
    CHECK(world.registry().get<TransformComponent>(eP).position.z == doctest::Approx(9.0f));
}

// #1b: hotReload must route by language too — a name loaded in both backends
// would otherwise send (e.g.) Lua source to the Python backend and silently fail.
TEST_CASE("ScriptContext: hotReload routes by language when a name exists in both")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("dup", kLuaSetX, ScriptLanguage::Lua));    // Lua onStart → x=3
    REQUIRE(ctx.loadScript("dup", kMover,   ScriptLanguage::Python)); // Py on_start → (7,8,9)

    auto eL = makeEntity(world, "L");
    auto eP = makeEntity(world, "P");
    auto idL = ctx.createInstance("dup", eL, ScriptLanguage::Lua);
    auto idP = ctx.createInstance("dup", eP, ScriptLanguage::Python);
    REQUIRE(idL != ScriptEngine::kInvalidInstance);
    REQUIRE(idP != ScriptEngine::kInvalidInstance);

    // Reload the LUA "dup" → x=9. Name-only routing (Python-first) would send this
    // Lua source to the Python backend and fail; language routing sends it to Lua.
    static const char* kLuaV2 =
        "local M={}\nfunction M.onStart(self) horizon.setPosition(self.entityId, 9,0,0) end\nreturn M\n";
    CHECK(ctx.hotReloadScript("dup", kLuaV2, ScriptLanguage::Lua));

    static const char* kPyV2 = R"py(
import horizon
class Mover(horizon.Behavior):
    def on_start(self):
        horizon.setPosition(self.entity_id, 42.0, 0.0, 0.0)
)py";
    CHECK(ctx.hotReloadScript("dup", kPyV2, ScriptLanguage::Python));

    REQUIRE(ctx.callOnStart(idL));  // Lua v2  → x = 9
    REQUIRE(ctx.callOnStart(idP));  // Py  v2  → x = 42
    CHECK(world.registry().get<TransformComponent>(eL).position.x == doctest::Approx(9.0f));
    CHECK(world.registry().get<TransformComponent>(eP).position.x == doctest::Approx(42.0f));
}

// #3: an instance finalizer that calls physics must be safe even when the
// PhysicsWorld was freed before the backend is destroyed (g_physics nulled first).
// A hard failure here needs ASAN; without it this still asserts no crash.
TEST_CASE("PyScriptBackend: finalizer touching freed physics during teardown is safe")
{
    HorizonWorld world;
    auto e  = makeEntity(world, "E");
    auto py = std::make_unique<PyScriptBackend>(world);

    static const char* kFinalizer = R"py(
import horizon
class F(horizon.Behavior):
    def __del__(self):
        horizon.setVelocity(self.entity_id, 1.0, 2.0, 3.0)
        horizon.isGrounded(self.entity_id)
)py";
    REQUIRE(py->loadScript("f", kFinalizer));
    {
        PhysicsWorld phys; phys.initialize(world);
        py->setPhysicsWorld(&phys);
        py->createInstance("f", static_cast<uint32_t>(e));
    } // phys destroyed here → g_physics now dangles
    py.reset(); // backend dtor runs the finalizer; must not touch freed physics
    CHECK(true);
}

#endif // HE_HAVE_PYTHON
