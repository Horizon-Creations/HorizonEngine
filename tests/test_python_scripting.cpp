#include "doctest.h"

// These tests only compile when the engine was built with CPython embedding.
// Nothing here includes <Python.h>, and since the backend became the
// runtime-loaded HorizonPython plugin, nothing here can name its class either:
// the tests reach it through exactly the ABI the engine uses (see PyBackend
// below), so they now cover the plugin contract as well as the interpreter.
#ifdef HE_HAVE_PYTHON

#include <Scripting/PythonPluginAbi.h>
#include <Platform/DynLib.h>
#include <Types/TypeRegistry.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/AudioEngine.h>   // a host service the plugin can only read across the module boundary
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <filesystem>   // the save-backed container round trip needs a sandbox root
#include <limits>
#include <memory>
#include <system_error>

// The plugin under test, at the path CMake built it to (HE_PYTHON_PLUGIN_PATH).
// The engine finds it next to HorizonScene; a test executable lives elsewhere,
// so it is told rather than made to guess.
namespace {

// One handle for the whole test run, never unloaded — the same rule the engine
// follows, and for the same reason: CPython holds pointers into this module and
// the interpreter is never finalised. Giving each test its own handle made the
// second one abort with "PyImport_AppendInittab() may not be called after
// Py_Initialize()", which is exactly the bug this arrangement prevents.
HE::DynLib& pluginLib()
{
    static HE::DynLib lib;
    return lib;
}

struct PyBackend
{
    IScriptBackend*   be      = nullptr;
    HePythonDestroyFn destroy = nullptr;

    explicit PyBackend(HorizonWorld& world)
    {
        HE::DynLib& lib = pluginLib();
        if (!lib.isLoaded())
            REQUIRE_MESSAGE(lib.load(HE_PYTHON_PLUGIN_PATH),
                            "could not load the HorizonPython plugin");
        auto create = reinterpret_cast<HePythonCreateFn>(
            lib.getSymbol(HE::PythonPlugin::kCreateSymbol));
        destroy = reinterpret_cast<HePythonDestroyFn>(
            lib.getSymbol(HE::PythonPlugin::kDestroySymbol));
        REQUIRE(create);
        REQUIRE(destroy);
        be = create(&world);
        REQUIRE(be);
    }
    ~PyBackend() { reset(); }

    PyBackend(const PyBackend&)            = delete;
    PyBackend& operator=(const PyBackend&) = delete;

    // Destroy the backend early, while the library is still mapped — the
    // teardown-order test below depends on being able to do exactly that.
    void reset()
    {
        if (be && destroy) destroy(be);
        be = nullptr;
    }

    IScriptBackend* operator->() const { return be; }
};

} // namespace

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

// Constructing PyBackend already REQUIREs that the plugin loads, that both
// entry points resolve and that the factory returns a backend — which is the
// whole availability contract, asserted on every one of these tests rather than
// once in a boolean.
TEST_CASE("PyScriptBackend: available and constructs")
{
    HorizonWorld world;
    PyBackend py(world);
    CHECK(py->loadedScriptCount() == 0);
    CHECK(py->instanceCount() == 0);
}

TEST_CASE("PyScriptBackend: loads a Behavior subclass")
{
    HorizonWorld world;
    PyBackend py(world);
    CHECK(py->loadScript("mover", kMover));
    CHECK(py->isScriptLoaded("mover"));
    CHECK(py->loadedScriptCount() == 1);
    CHECK(py->lastError().empty());
}

TEST_CASE("PyScriptBackend: rejects a syntax error")
{
    HorizonWorld world;
    PyBackend py(world);
    CHECK_FALSE(py->loadScript("bad", kBadSyntax));
    CHECK_FALSE(py->isScriptLoaded("bad"));
    CHECK_FALSE(py->lastError().empty());
}

TEST_CASE("PyScriptBackend: rejects source with no Behavior subclass")
{
    HorizonWorld world;
    PyBackend py(world);
    CHECK_FALSE(py->loadScript("plain", kNoBehavior));
    CHECK_FALSE(py->lastError().empty());
}

// ─── Lifecycle + horizon API round-trip ─────────────────────────────────────

TEST_CASE("PyScriptBackend: on_start runs and reaches the world")
{
    HorizonWorld world;
    PyBackend py(world);
    REQUIRE(py->loadScript("mover", kMover));

    auto e  = makeEntity(world, "Hero");
    auto id = py->createInstance("mover", static_cast<uint32_t>(e));
    REQUIRE(id != IScriptBackend::kInvalidInstance);
    CHECK(py->instanceCount() == 1);

    CHECK(py->callOnStart(id));
    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(7.0f));
    CHECK(t.position.y == doctest::Approx(8.0f));
    CHECK(t.position.z == doctest::Approx(9.0f));
}

TEST_CASE("PyScriptBackend: on_update integrates dt")
{
    HorizonWorld world;
    PyBackend py(world);
    REQUIRE(py->loadScript("mover", kMover));

    auto e  = makeEntity(world, "Hero");
    auto id = py->createInstance("mover", static_cast<uint32_t>(e));
    REQUIRE(py->callOnStart(id));           // → x = 7
    CHECK(py->callOnUpdate(id, 0.5f));      // → x = 7.5
    CHECK(py->callOnUpdate(id, 0.25f));     // → x = 7.75

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(7.75f));
}

TEST_CASE("PyScriptBackend: missing handler is a no-op success")
{
    HorizonWorld world;
    PyBackend py(world);
    REQUIRE(py->loadScript("echo", kSpeedEcho)); // no on_update defined
    auto e  = makeEntity(world, "E");
    auto id = py->createInstance("echo", static_cast<uint32_t>(e));
    CHECK(py->callOnUpdate(id, 0.1f));       // no on_update → true, no error
    CHECK(py->lastError().empty());
}

TEST_CASE("PyScriptBackend: a raising handler reports the error")
{
    HorizonWorld world;
    PyBackend py(world);
    REQUIRE(py->loadScript("boom", kRaises));
    auto e  = makeEntity(world, "E");
    auto id = py->createInstance("boom", static_cast<uint32_t>(e));
    CHECK_FALSE(py->callOnStart(id));
    CHECK(py->lastError().find("kaboom") != std::string::npos);
}

// ─── Properties ─────────────────────────────────────────────────────────────

TEST_CASE("PyScriptBackend: getScriptProperties reads typed class attributes")
{
    HorizonWorld world;
    PyBackend py(world);
    REQUIRE(py->loadScript("mover", kMover));

    auto defs = py->getScriptProperties("mover");
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
    PyBackend py(world);
    REQUIRE(py->loadScript("echo", kSpeedEcho));

    auto e  = makeEntity(world, "E");
    auto id = py->createInstance("echo", static_cast<uint32_t>(e));

    ScriptPropValue v; v.type = ScriptPropType::Float; v.f = 42.0f;
    py->injectProperties(id, {{"speed", v}});
    REQUIRE(py->callOnStart(id));            // echoes self.speed into x

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(42.0f));
}

// ─── Hot reload ─────────────────────────────────────────────────────────────

TEST_CASE("PyScriptBackend: hotReload swaps behavior, preserves instance data")
{
    HorizonWorld world;
    PyBackend py(world);

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

    REQUIRE(py->loadScript("s", v1));
    auto e  = makeEntity(world, "E");
    auto id = py->createInstance("s", static_cast<uint32_t>(e));
    REQUIRE(py->callOnStart(id));            // self.hp = 100 (data)

    CHECK(py->hotReloadScript("s", v2));     // new class, keep __dict__
    CHECK(py->callOnUpdate(id, 0.0f));       // v2: x = hp + 1

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(101.0f)); // 100 preserved + v2 code ran
}

// ─── Teardown ───────────────────────────────────────────────────────────────

TEST_CASE("PyScriptBackend: unload destroys instances")
{
    HorizonWorld world;
    PyBackend py(world);
    REQUIRE(py->loadScript("mover", kMover));
    auto e = makeEntity(world, "E");
    py->createInstance("mover", static_cast<uint32_t>(e));
    CHECK(py->instanceCount() == 1);
    py->unloadScript("mover");
    CHECK_FALSE(py->isScriptLoaded("mover"));
    CHECK(py->instanceCount() == 0);
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
    REQUIRE(ctx.loadScript("pym", kMover, HE::ScriptLanguage::Python));
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
    REQUIRE(ctx.loadScript("pymath", kPyMath, HE::ScriptLanguage::Python));

    auto e  = makeEntity(world, "MathHero");
    auto id = ctx.createInstance("pymath", e);
    REQUIRE(id != ScriptEngine::kInvalidInstance);
    REQUIRE(ctx.callOnStart(id));

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(3.0f));   // clamp(5, 0, 3)
    CHECK(t.position.y == doctest::Approx(5.0f));   // lerp(0, 10, 0.5)
    CHECK(t.position.z == doctest::Approx(9.0f));   // max(2, 9)
}

// Behavior that writes random-library results into its entity's position.
static const char* kPyRandom = R"py(
import horizon

class RandUser(horizon.Behavior):
    def on_start(self):
        horizon.random.seed(5)
        x = 1.0 if horizon.random.chance(1.0) else 0.0   # always true
        y = horizon.random.range(2, 2)                   # degenerate → 2
        z = float(horizon.random.rangeInt(7, 7))         # degenerate → 7
        horizon.setPosition(self.entity_id, x, y, z)
)py";

TEST_CASE("ScriptContext: registry-driven horizon.random.* (Python)")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("pyrand", kPyRandom, HE::ScriptLanguage::Python));

    auto e  = makeEntity(world, "RandHero");
    auto id = ctx.createInstance("pyrand", e);
    REQUIRE(id != ScriptEngine::kInvalidInstance);
    REQUIRE(ctx.callOnStart(id));

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(1.0f));   // chance(1.0)
    CHECK(t.position.y == doctest::Approx(2.0f));   // range(2, 2)
    CHECK(t.position.z == doctest::Approx(7.0f));   // rangeInt(7, 7)
}

// Behavior that turns a mixer bus down. Nothing about it is observable from
// inside Python — the point is entirely on the C++ side of the boundary.
static const char* kPyAudio = R"py(
import horizon

class BusUser(horizon.Behavior):
    def on_start(self):
        horizon.audio.setBusVolume("SFX", 0.25)
)py";

TEST_CASE("PyScriptBackend: an audio call from Python reaches the host's engine")
{
    // The audio rows read Ctx::audio, and this plugin's Ctx used to be built out
    // of the three file-statics the backend is handed (world, physics, content) —
    // so every one of them was a no-op in Python, silently. It now reads the
    // host's services from the engine module, which is what makes this test
    // worth more than its Lua twin: ScriptContext::hostServices() is resolved
    // ACROSS THE MODULE BOUNDARY, out of a dylib loaded at run time, exactly as
    // hostQuitHandler() already was. A missing export would not fail the build.
    //
    // Headless mode is required, not incidental: setBusVolume early-outs on
    // !isInitialized(), so an uninitialised engine answers 1.0 either way.
    HorizonWorld world;
    AudioEngine  audio;
    REQUIRE(audio.init(/*noDevice=*/true));

    {
        ScriptContext ctx(world);
        ScriptContext::HostServices hs;
        hs.audio = &audio;
        ctx.setHostServices(std::move(hs));

        REQUIRE(ctx.loadScript("pyaudio", kPyAudio, HE::ScriptLanguage::Python));
        auto e  = makeEntity(world, "BusHero");
        auto id = ctx.createInstance("pyaudio", e);
        REQUIRE(id != ScriptEngine::kInvalidInstance);
        REQUIRE(ctx.callOnStart(id));

        // BEFORE THE CHANGE: the plugin's Ctx had audio == nullptr and this was
        // still 1.0 — a Python game with a full soundtrack and no sound.
        CHECK(audio.hasBus("SFX"));
        CHECK(audio.getBusVolume("SFX") == doctest::Approx(0.25f));
    }
    CHECK(ScriptContext::hostServices().audio == nullptr);   // withdrawn with the context
    audio.shutdown();
}

// Behavior that reads the time + input snapshot into its entity's position.
static const char* kPyTimeInput = R"py(
import horizon

class TimeInputUser(horizon.Behavior):
    def on_start(self):
        dt = horizon.time.deltaTime()
        sp = 1.0 if horizon.input.keyDown("Space") else 0.0
        mx, my = horizon.input.mousePosition()
        horizon.setPosition(self.entity_id, dt, sp, mx)
)py";

TEST_CASE("ScriptContext: registry-driven horizon.time.*/input.* (Python)")
{
    HE::api::time::reset();
    HE::api::time::advance(0.5f);
    HE::api::input::setKeysDown({ "Space" });
    HE::api::input::setMouse({ 7.0f, 8.0f }, { 0.0f, 0.0f }, 0u, 0.0f);

    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("pyti", kPyTimeInput, HE::ScriptLanguage::Python));

    auto e  = makeEntity(world, "TimeInputHero");
    auto id = ctx.createInstance("pyti", e);
    REQUIRE(id != ScriptEngine::kInvalidInstance);
    REQUIRE(ctx.callOnStart(id));

    const auto& t = world.registry().get<TransformComponent>(e);
    CHECK(t.position.x == doctest::Approx(0.5f));   // time.deltaTime()
    CHECK(t.position.y == doctest::Approx(1.0f));   // input.keyDown("Space")
    CHECK(t.position.z == doctest::Approx(7.0f));   // input.mousePosition().x
    HE::api::input::clear();
}

TEST_CASE("ScriptContext: Lua and Python coexist without id collision")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("pym", kMover,   HE::ScriptLanguage::Python));
    REQUIRE(ctx.loadScript("lm",  kLuaSetX, HE::ScriptLanguage::Lua));
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
    REQUIRE(ctx.loadScript("boom", kRaises, HE::ScriptLanguage::Python));
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
    PyBackend py(world);
    static const char* kSurrogate = R"py(
import horizon
class P(horizon.Behavior):
    good = 5
    tag  = '\udce9'
)py";
    REQUIRE(py->loadScript("p", kSurrogate));
    auto defs = py->getScriptProperties("p");   // must return, not segfault
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
    PyBackend py(world);
    static const char* kBigInts = R"py(
import horizon
class Q(horizon.Behavior):
    big   = 10**40
    col   = 0xFFFFFFFF
    small = 7
)py";
    REQUIRE(py->loadScript("q", kBigInts));
    auto defs = py->getScriptProperties("q");
    auto find = [&](const char* n) -> const ScriptPropDef* {
        for (auto& d : defs) if (d.name == n) return &d; return nullptr;
    };
    REQUIRE(find("small")); CHECK(find("small")->defaultVal.i == 7);
    REQUIRE(find("col"));   CHECK(find("col")->defaultVal.i == std::numeric_limits<int>::max());
    REQUIRE(find("big"));   CHECK(find("big")->defaultVal.i == 0); // overflowed long long → 0, no wrap
    // A follow-up call proves no stale error leaked from the overflow.
    CHECK(py->getScriptProperties("q").size() == defs.size());
}

// #4: two entities sharing a moduleName across languages each route to their own
// backend when the caller passes the language (createInstance/isScriptLoaded 3-arg).
TEST_CASE("ScriptContext: same moduleName in two languages routes by language")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("shared", kLuaSetX, HE::ScriptLanguage::Lua));
    REQUIRE(ctx.loadScript("shared", kMover,   HE::ScriptLanguage::Python));
    CHECK(ctx.isScriptLoaded("shared", HE::ScriptLanguage::Lua));
    CHECK(ctx.isScriptLoaded("shared", HE::ScriptLanguage::Python));

    auto eL = makeEntity(world, "L");
    auto eP = makeEntity(world, "P");
    auto idL = ctx.createInstance("shared", eL, HE::ScriptLanguage::Lua);
    auto idP = ctx.createInstance("shared", eP, HE::ScriptLanguage::Python);
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
    REQUIRE(ctx.loadScript("dup", kLuaSetX, HE::ScriptLanguage::Lua));    // Lua onStart → x=3
    REQUIRE(ctx.loadScript("dup", kMover,   HE::ScriptLanguage::Python)); // Py on_start → (7,8,9)

    auto eL = makeEntity(world, "L");
    auto eP = makeEntity(world, "P");
    auto idL = ctx.createInstance("dup", eL, HE::ScriptLanguage::Lua);
    auto idP = ctx.createInstance("dup", eP, HE::ScriptLanguage::Python);
    REQUIRE(idL != ScriptEngine::kInvalidInstance);
    REQUIRE(idP != ScriptEngine::kInvalidInstance);

    // Reload the LUA "dup" → x=9. Name-only routing (Python-first) would send this
    // Lua source to the Python backend and fail; language routing sends it to Lua.
    static const char* kLuaV2 =
        "local M={}\nfunction M.onStart(self) horizon.setPosition(self.entityId, 9,0,0) end\nreturn M\n";
    CHECK(ctx.hotReloadScript("dup", kLuaV2, HE::ScriptLanguage::Lua));

    static const char* kPyV2 = R"py(
import horizon
class Mover(horizon.Behavior):
    def on_start(self):
        horizon.setPosition(self.entity_id, 42.0, 0.0, 0.0)
)py";
    CHECK(ctx.hotReloadScript("dup", kPyV2, HE::ScriptLanguage::Python));

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
    PyBackend py(world);

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

// NOTE: the #endif used to sit HERE, above a test case that constructs a
// PyBackend — so a build without CPython would have failed to compile this file
// rather than skipping it. It closes at the end of the file now.

TEST_CASE("PyScriptBackend: horizon.enums constants + horizon.structs constructors")
{
    // User-defined types bootstrap from the TypeRegistry at backend creation.
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
        stats.fields = { hp, w };
    }
    reg.registerStruct(stats);

    {
        HorizonWorld world;
        PyBackend py(world);
        // Report through the entity transform (the harness's readback channel):
        // x = the Bow constant, y = the constructed struct's hp default, z = 1
        // when __type round-trips.
        static const char* kSrc = R"(
import horizon
class Reporter(horizon.Behavior):
    def on_start(self):
        s = horizon.structs.PlayerStats()
        ok = 1.0 if s["__type"] == "Content/T/PlayerStats.hasset" else 0.0
        horizon.setPosition(self.entity_id, float(horizon.enums.Weapon.Bow), s["hp"], ok)
)";
        REQUIRE(py->loadScript("reporter", kSrc));
        auto e  = makeEntity(world, "Hero");
        auto id = py->createInstance("reporter", static_cast<uint32_t>(e));
        REQUIRE(id != IScriptBackend::kInvalidInstance);
        REQUIRE(py->callOnStart(id));
        const auto& t = world.registry().get<TransformComponent>(e);
        CHECK(t.position.x == doctest::Approx(7.0f));
        CHECK(t.position.y == doctest::Approx(100.0f));
        CHECK(t.position.z == doctest::Approx(1.0f));
    }

    reg.removeType(weapon.assetPath);
    reg.removeType(stats.assetPath);
}

TEST_CASE("PyScriptBackend: Set and Map fields cross into Python and back, in order")
{
    // Python needs no `__keys` sidecar the way Lua does: a dict is
    // insertion-ordered by the language, so a map's order survives the round
    // trip exactly. A Set crosses as a LIST, not a `set` — a `set` would throw
    // the order away, which is the one thing the container promises to keep.
    namespace save = HE::api::save;
    using P = HorizonCode::PinType;
    using CK = HorizonCode::ContainerKind;
    using V = HorizonCode::Value;
    auto& reg = HE::TypeRegistry::instance();
    const char* kBag = "Content/T/PyBag.hasset";
    const auto root = std::filesystem::temp_directory_path() / "he_py_ctr_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    HE::api::fs::setSandboxRoot(root.string());
    save::close();

    HE::StructDef bag;
    bag.name = "PyBag"; bag.assetPath = kBag;
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
        a.name = "PyBagTemplate"; a.path = "mem://py_bag_tpl";
        a.json = HE::TypeRegistry::structToJson(tpl);
        cm.registerSaveGameTemplate(std::move(a));
        save::setDefaultTemplate("mem://py_bag_tpl");
    }
    REQUIRE(save::create("pybag", &cm));
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
        PyBackend py(world);
        // x = 1 when the set arrived as an ordered list, y = 1 when the dict
        // iterated in insertion order (NOT alphabetically), z = its first value.
        static const char* kSrc = R"(
import horizon
class Bagger(horizon.Behavior):
    def on_start(self):
        b = horizon.save.getStruct("bag")
        ordered = 1.0 if b["tags"] == ["zeta", "alpha"] else 0.0
        keys = list(b["ammo"].keys())
        keyorder = 1.0 if keys == ["zeta", "alpha"] else 0.0
        first = float(b["ammo"]["zeta"])
        b["tags"].append("zeta")   # a duplicate — the set drops it
        b["tags"].append("mid")
        b["ammo"]["omega"] = 5
        horizon.save.setStruct("bag", b)
        horizon.setPosition(self.entity_id, ordered, keyorder, first)
)";
        REQUIRE(py->loadScript("bagger", kSrc));
        auto e  = makeEntity(world, "Bag");
        auto id = py->createInstance("bagger", static_cast<uint32_t>(e));
        REQUIRE(id != IScriptBackend::kInvalidInstance);
        REQUIRE(py->callOnStart(id));
        const auto& t = world.registry().get<TransformComponent>(e);
        CHECK(t.position.x == doctest::Approx(1.0f));
        CHECK(t.position.y == doctest::Approx(1.0f));
        CHECK(t.position.z == doctest::Approx(9.0f));
    }

    const V back = save::getStructV("bag");
    REQUIRE(back.items.size() == 2);
    CHECK(back.items[0].kind() == CK::Set);
    REQUIRE(back.items[0].items.size() == 3);          // "zeta" appended twice, kept once
    CHECK(back.items[0].items[0].s == "zeta");
    CHECK(back.items[0].items[1].s == "alpha");
    CHECK(back.items[0].items[2].s == "mid");
    CHECK(back.items[1].kind() == CK::Map);
    REQUIRE(back.items[1].keys.size() == 3);
    CHECK(back.items[1].keys[0].s == "zeta");          // dict insertion order,
    CHECK(back.items[1].keys[1].s == "alpha");         // straight through
    CHECK(back.items[1].keys[2].s == "omega");
    CHECK(back.items[1].items[2].i == 5);

    save::close();
    save::setDefaultTemplate("");
    reg.removeType(kBag);
    std::filesystem::remove_all(root, ec);
}

// The Python half of "the application groups reach both text languages" (the Lua
// half lives in test_scripting_binding.cpp). Same eleven, same one function
// each, same reason for checking presence rather than effect.
static const char* kPyAppGroups = R"py(
import horizon

class AppGroups(horizon.Behavior):
    def on_start(self):
        names = [('widget', 'setListCount'), ('theme', 'getMode'),
                 ('dialog', 'confirm'),      ('clipboard', 'hasText'),
                 ('process', 'which'),       ('json', 'getNumber'),
                 ('prefs', 'has'),           ('datetime', 'year'),
                 ('timer', 'cancel'),        ('db', 'lastError'),
                 ('print', 'available'),     ('window', 'open')]
        found = 0
        for g, f in names:
            ns = getattr(horizon, g, None)
            if ns is not None and callable(getattr(ns, f, None)):
                found += 1
        horizon.setPosition(self.entity_id, found,
                            horizon.json.getNumber('{"a":42}', 'a', 0), 0)
)py";

TEST_CASE("ScriptContext: the application groups reach Python")
{
    HorizonWorld world;
    ScriptContext ctx(world);
    REQUIRE(ctx.loadScript("pygroups", kPyAppGroups, HE::ScriptLanguage::Python));

    auto e  = makeEntity(world, "GroupHero");
    auto id = ctx.createInstance("pygroups", e);
    REQUIRE(id != ScriptEngine::kInvalidInstance);
    REQUIRE(ctx.callOnStart(id));

    const auto& t = world.registry().get<TransformComponent>(e);
    // BEFORE THE CHANGE: 0 — bootstrapEngineApiGroups skipped every one of
    // their rows, exactly as the Lua side did.
    CHECK(t.position.x == doctest::Approx(12.0f));
    CHECK(t.position.y == doctest::Approx(42.0f));   // …and one of them dispatches
}

#endif // HE_HAVE_PYTHON
