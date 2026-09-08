// The physics + input service tables, driven through a REAL loaded game-logic
// library rather than through the wrappers linked into this binary.
//
// That distinction is the point of the file. test_engine_api.cpp exercises the
// save table by calling the he::* wrappers compiled into he_tests itself, which
// proves the lambdas work but says nothing about the boundary: the fixture here
// is a separate shared library that links NOTHING and sees only
// src/HE_Core/include, exactly like a generated C++ game project. Every value
// below crossed a dylib boundary as plain C.
#include "doctest.h"
#include "fixtures/test_gamelogic_probe.h"
#include "TestFsUtil.h"
#include <Application/GameLogicLoader.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <filesystem>
#include <glm/glm.hpp>

#ifndef HE_TEST_GAMELOGIC_SERVICES_LIB
#  define HE_TEST_GAMELOGIC_SERVICES_LIB ""
#endif
#ifndef HE_TEST_GAMELOGIC_LIB
#  define HE_TEST_GAMELOGIC_LIB ""
#endif

namespace {

// World + physics + content + the four tables under one umbrella, wired the way
// GameApplication wires them: resolvers for world and physics (both are replaced
// on a scene switch), a raw pointer for content (the manager outlives every
// switch).
//
// The content root is a real temporary directory with real .hasset files in it,
// because the module's load() reads the disk — and because a SECOND load is what
// moves the asset pool, which is the thing this fixture has to be able to
// provoke.
struct ServicesRig
{
    std::filesystem::path contentRoot;
    HorizonWorld   world;
    PhysicsWorld   physics;
    ContentManager content;

    HE::api::GameServicesBinding binding;
    HeSaveServices               save{};
    HePhysicsServices            phys{};
    HeInputServices              input{};
    HeContentServices            assets{};
    HeEngineServices             umbrella{};

    ServicesRig()
        : contentRoot(std::filesystem::temp_directory_path() / "he_gl_services_content" / "Content")
    {
        he_test::removeAllQuiet(contentRoot.parent_path());
        std::filesystem::create_directories(contentRoot);
        content.setContentRoot(contentRoot.string());
        writeTexture("Rock.hasset", "Rock");
        writeTexture("Moss.hasset", "Moss");
        StaticMeshAsset mesh;
        mesh.type = HE::AssetType::StaticMesh;
        mesh.name = "Cube"; mesh.path = "Cube.hasset";
        mesh.vertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
        mesh.indices  = { 0, 1, 2 };
        content.saveAsset(mesh);
        // saveAsset registers what it wrote; drop it all again so the module
        // starts from "on disk, not resident" and its load() really loads.
        for (const HE::UUID id : content.enumerateIds()) content.unloadAsset(id);

        binding.world   = [this]() { return &world; };
        binding.physics = [this]() { return &physics; };
        binding.content = &content;
        HE::api::fillSaveServices(save, &binding);
        HE::api::fillPhysicsServices(phys, &binding);
        HE::api::fillInputServices(input, &binding);
        HE::api::fillContentServices(assets, &binding);
        umbrella.abiVersion = HE_SERVICES_ABI_VERSION;
        umbrella.save       = &save;
        umbrella.physics    = &phys;
        umbrella.input      = &input;
        umbrella.content    = &assets;
    }
    ~ServicesRig() { he_test::removeAllQuiet(contentRoot.parent_path()); }

    void writeTexture(const std::string& path, const std::string& name)
    {
        TextureAsset t;
        t.type = HE::AssetType::Texture;
        t.name = name; t.path = path;
        t.width = 1; t.height = 1; t.channels = 4;
        t.data = { 255, 255, 255, 255 };
        content.saveAsset(t);
    }
};

HE::api::Entity spawnStaticBody(HorizonWorld& world, const glm::vec3& pos,
                                const char* name = "Body")
{
    auto e = world.createEntity(name);
    TransformComponent t;
    t.position = pos;
    t.scale    = { 1.0f, 1.0f, 1.0f };
    world.addComponent(e, t);
    RigidBodyComponent rb;
    rb.type = RigidBodyType::Static;
    world.addComponent(e, rb);
    return static_cast<HE::api::Entity>(e);
}

HE::api::Entity spawnDynamicBody(HorizonWorld& world, const glm::vec3& pos)
{
    auto e = world.createEntity("Dynamic");
    TransformComponent t;
    t.position = pos;
    t.scale    = { 1.0f, 1.0f, 1.0f };
    world.addComponent(e, t);
    RigidBodyComponent rb;
    rb.type = RigidBodyType::Dynamic;
    rb.mass = 1.0f;
    world.addComponent(e, rb);
    return static_cast<HE::api::Entity>(e);
}

ITestServicesProbe* probeOf(HE::GameLogicLoader& loader)
{
    return static_cast<ITestServicesProbe*>(loader.logic());
}

} // namespace

TEST_CASE("GameLogic services: physics reaches a loaded C++ module through the C ABI")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_SERVICES_LIB;
    REQUIRE(!libPath.empty());
    REQUIRE(std::filesystem::exists(libPath));

    ServicesRig rig;
    const auto floorBody = spawnStaticBody(rig.world, { 0.0f, 0.0f, 0.0f }, "Target");
    const auto neighbour = spawnStaticBody(rig.world, { 1.5f, 0.0f, 0.0f }, "Neighbour");
    const auto pushable  = spawnDynamicBody(rig.world, { 0.0f, 20.0f, 0.0f });
    rig.physics.initialize(rig.world);
    REQUIRE(rig.physics.hasPhysics(static_cast<uint32_t>(floorBody)));

    HE::GameLogicLoader loader;
    REQUIRE(loader.load(libPath));
    auto* probe = probeOf(loader);
    REQUIRE(probe != nullptr);

    // Before injection the module has nothing and says so — no crash, defaults.
    CHECK_FALSE(probe->physicsAvailable());
    CHECK_FALSE(probe->doRaycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 50.0f).hit);
    CHECK_FALSE(probe->doAddImpulse((uint32_t)pushable, { 0.0f, 1.0f, 0.0f }));
    CHECK(probe->doGetGravity().y == doctest::Approx(0.0f));

    // Injection happens BEFORE onStart, which is what makes a game able to raycast
    // from its very first native line.
    REQUIRE(loader.injectServices(&rig.umbrella));
    loader.logic()->onStart(rig.world);
    CHECK(probe->servicesAvailableAtStart());
    CHECK(probe->physicsAvailableAtStart());
    CHECK(probe->inputAvailableAtStart());
    loader.logic()->onUpdate(rig.world, 1.0f / 60.0f);
    CHECK(probe->updateCount() == 1);
    CHECK(probe->physicsAvailableAtUpdate());

    SUBCASE("raycast and sphereCast report world hits")
    {
        const he::RaycastHit hit =
            probe->doRaycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 50.0f);
        REQUIRE(hit.hit);
        CHECK(hit.entity == (uint32_t)floorBody);
        CHECK(hit.distance > 0.0f);
        CHECK(hit.point.y < 10.0f);
        CHECK(hit.normal.y > 0.5f);   // pointing back up at the ray

        // A miss is a zeroed hit, not stale bytes from the previous call.
        const he::RaycastHit miss =
            probe->doRaycast({ 500.0f, 10.0f, 500.0f }, { 0.0f, -1.0f, 0.0f }, 5.0f);
        CHECK_FALSE(miss.hit);
        CHECK(miss.entity == 0u);
        CHECK(miss.distance == doctest::Approx(0.0f));

        const he::RaycastHit swept =
            probe->doSphereCast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 0.4f, 50.0f);
        CHECK(swept.hit);
    }

    SUBCASE("overlapSphere reports the full count even into a short buffer")
    {
        uint32_t buf[8] = {};
        const int all = probe->doOverlapSphere({ 0.0f, 0.0f, 0.0f }, 5.0f, buf, 8);
        REQUIRE(all >= 2);

        // Every id that came back is one of the two bodies in range — nothing
        // invented itself on the way across.
        for (int i = 0; i < all && i < 8; ++i)
            CHECK((buf[i] == (uint32_t)floorBody || buf[i] == (uint32_t)neighbour));

        // The two-stage buffer contract: `cap` entries written, the FULL count
        // returned, so a caller that guessed too small can grow and ask again.
        uint32_t one[1] = { 0u };
        const int again = probe->doOverlapSphere({ 0.0f, 0.0f, 0.0f }, 5.0f, one, 1);
        CHECK(again == all);
        CHECK((one[0] == (uint32_t)floorBody || one[0] == (uint32_t)neighbour));

        // Nothing out at distance.
        uint32_t none[4] = {};
        CHECK(probe->doOverlapSphere({ 900.0f, 900.0f, 900.0f }, 1.0f, none, 4) == 0);
    }

    SUBCASE("forces need a dynamic body and say so when there is none")
    {
        CHECK(probe->doHasPhysics((uint32_t)pushable));
        CHECK(probe->doAddImpulse((uint32_t)pushable, { 0.0f, 0.0f, 4.0f }));
        CHECK(probe->doAddForce((uint32_t)pushable, { 0.0f, 0.0f, 10.0f }));
        CHECK(probe->doAddTorque((uint32_t)pushable, { 0.0f, 1.0f, 0.0f }));

        // The impulse actually landed — read the velocity back over the same
        // boundary it was pushed across.
        const he::Vec3 v = probe->doGetVelocity((uint32_t)pushable);
        CHECK(v.z > 0.0f);

        // A static body is not pushable, and an entity that does not exist is
        // neither — both are false, not a crash.
        CHECK_FALSE(probe->doAddImpulse((uint32_t)floorBody, { 0.0f, 0.0f, 1.0f }));
        CHECK_FALSE(probe->doHasPhysics(999999u));
        CHECK_FALSE(probe->doAddForce(999999u, { 1.0f, 0.0f, 0.0f }));
        CHECK_FALSE(probe->doIsGrounded(999999u));

        probe->doSetVelocity((uint32_t)pushable, { 0.0f, 0.0f, 0.0f });
        CHECK(probe->doGetVelocity((uint32_t)pushable).z == doctest::Approx(0.0f));
    }

    SUBCASE("gravity crosses in both directions")
    {
        probe->doSetGravity({ 0.0f, -3.5f, 0.0f });
        const he::Vec3 g = probe->doGetGravity();
        CHECK(g.y == doctest::Approx(-3.5f));
        CHECK(g.x == doctest::Approx(0.0f));
    }

    SUBCASE("setPosition teleports and AndReset stops the body")
    {
        REQUIRE(probe->doSetPosition((uint32_t)pushable, { 4.0f, 30.0f, 0.0f }));
        const glm::vec3 after =
            rig.world.registry().get<TransformComponent>(
                static_cast<entt::entity>(pushable)).position;
        CHECK(after.x == doctest::Approx(4.0f));
        CHECK(after.y == doctest::Approx(30.0f));

        probe->doSetVelocity((uint32_t)pushable, { 0.0f, 0.0f, 6.0f });
        REQUIRE(probe->doSetPositionAndReset((uint32_t)pushable, { 0.0f, 30.0f, 0.0f }));
        CHECK(probe->doGetVelocity((uint32_t)pushable).z == doctest::Approx(0.0f));

        // No body, no teleport — and no crash either.
        CHECK_FALSE(probe->doSetPosition(999999u, { 1.0f, 1.0f, 1.0f }));
    }

    loader.unload(rig.world);
}

// The single test that proves Decision B — that the C boundary routes through
// HE::api and inherits its one local↔world conversion instead of doing its own.
// A rotated parent is what makes it worth having: with a merely translated
// parent, the composed position and the sum of the two positions agree and
// broken math passes.
TEST_CASE("GameLogic services: physics.setPosition across the C ABI is LOCAL, hits are WORLD")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_SERVICES_LIB;
    REQUIRE(std::filesystem::exists(libPath));

    ServicesRig rig;
    // Parent 10 along X, turned a quarter turn about Y: a child 2 in front of it
    // (local +Z) therefore stands 2 further along X, at (12, 0, 0).
    auto parentE = rig.world.createEntity("Parent");
    {
        TransformComponent t;
        t.position = { 10.0f, 0.0f, 0.0f };
        t.rotation = { 0.0f, 90.0f, 0.0f };
        t.scale    = { 1.0f, 1.0f, 1.0f };
        rig.world.addComponent(parentE, t);
    }
    const auto child = spawnStaticBody(rig.world, { 0.0f, 0.0f, 0.0f }, "ParentedBody");
    REQUIRE(rig.world.reparentEntity(static_cast<entt::entity>(child), parentE));
    rig.physics.initialize(rig.world);
    REQUIRE(rig.physics.hasPhysics(static_cast<uint32_t>(child)));

    HE::GameLogicLoader loader;
    REQUIRE(loader.load(libPath));
    REQUIRE(loader.injectServices(&rig.umbrella));
    auto* probe = probeOf(loader);

    REQUIRE(probe->doSetPosition((uint32_t)child, { 0.0f, 0.0f, 2.0f }));

    // Where the child is DRAWN — the composed world position — is where its
    // collider has to be, and that is where the module's own raycast finds it.
    const he::RaycastHit atWorld =
        probe->doRaycast({ 12.0f, 50.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
    REQUIRE(atWorld.hit);
    CHECK(atWorld.entity == (uint32_t)child);

    // And nothing sits at the raw local value. This is the half that fails
    // without the conversion: an unconverted (0,0,2) would park the body here,
    // 12 m from its mesh.
    CHECK_FALSE(probe->doRaycast({ 0.0f, 50.0f, 2.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f).hit);

    // Read back in the space it was written in.
    const glm::vec3 local =
        rig.world.registry().get<TransformComponent>(
            static_cast<entt::entity>(child)).position;
    CHECK(local.x == doctest::Approx(0.0f));
    CHECK(local.z == doctest::Approx(2.0f));

    loader.unload(rig.world);
}

TEST_CASE("GameLogic services: input reaches a loaded C++ module through the C ABI")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_SERVICES_LIB;
    REQUIRE(std::filesystem::exists(libPath));

    ServicesRig rig;
    HE::GameLogicLoader loader;
    REQUIRE(loader.load(libPath));
    auto* probe = probeOf(loader);

    // Uninjected: every query is its default, including the mode.
    HE::api::input::clear();
    CHECK_FALSE(probe->inputAvailable());
    CHECK_FALSE(probe->doKeyDown("W"));
    CHECK(probe->doScrollDelta() == doctest::Approx(0.0f));
    CHECK(probe->doMode() == (int)he::input::Mode::GameAndUI);

    REQUIRE(loader.injectServices(&rig.umbrella));
    REQUIRE(probe->inputAvailable());

    HE::api::input::setKeysDown({ "W", "Space" });
    HE::api::input::setMouse({ 320.0f, 240.0f }, { -4.0f, 7.0f },
                             (1u << 0) | (1u << 2), 2.5f);
    CHECK(probe->doKeyDown("W"));
    CHECK(probe->doKeyDown("Space"));
    CHECK_FALSE(probe->doKeyDown("Escape"));
    CHECK(probe->doMouseButton(0));
    CHECK_FALSE(probe->doMouseButton(1));
    CHECK(probe->doMouseButton(2));
    CHECK(probe->doMousePosition().x == doctest::Approx(320.0f));
    CHECK(probe->doMousePosition().y == doctest::Approx(240.0f));
    CHECK(probe->doMouseDelta().x == doctest::Approx(-4.0f));
    CHECK(probe->doMouseDelta().y == doctest::Approx(7.0f));
    CHECK(probe->doScrollDelta() == doctest::Approx(2.5f));

    // Gamepad. SDL axis order: leftx, lefty, rightx, righty, lefttrigger, righttrigger.
    CHECK_FALSE(probe->doGamepadConnected());
    float axes[6] = { 0.5f, -0.25f, 0.0f, 0.0f, 1.0f, 0.0f };
    bool  buttons[16] = {};
    buttons[0]  = true;   // "a" (south)
    buttons[11] = true;   // "dpup"
    HE::api::input::setGamepad(true, axes, 6, buttons, 16);
    CHECK(probe->doGamepadConnected());
    CHECK(probe->doGamepadButton("a"));
    CHECK(probe->doGamepadButton("dpup"));
    CHECK_FALSE(probe->doGamepadButton("b"));
    CHECK_FALSE(probe->doGamepadButton("not_a_button"));
    CHECK(probe->doGamepadAxis("leftx") == doctest::Approx(0.5f));
    CHECK(probe->doGamepadAxis("lefty") == doctest::Approx(-0.25f));
    CHECK(probe->doGamepadAxis("lefttrigger") == doctest::Approx(1.0f));
    CHECK(probe->doGamepadAxis("bogus") == doctest::Approx(0.0f));

    SUBCASE("the module can switch the input mode, and cannot invent one")
    {
        probe->doSetModeUIOnly();
        CHECK(HE::api::input::mode() == HE::api::input::Mode::UIOnly);
        CHECK(probe->doMode() == (int)he::input::Mode::UIOnly);

        // Straight at the table with a value no enumerator has: ignored, so the
        // routing switch never sees a mode it has no case for.
        probe->doSetModeRaw(7);
        CHECK(HE::api::input::mode() == HE::api::input::Mode::UIOnly);
        probe->doSetModeRaw(-1);
        CHECK(HE::api::input::mode() == HE::api::input::Mode::UIOnly);

        probe->doSetModeGameAndUI();
        CHECK(HE::api::input::mode() == HE::api::input::Mode::GameAndUI);
    }

    loader.unload(rig.world);
    HE::api::input::clear();
    HE::api::input::setMode(HE::api::input::Mode::GameAndUI);
}

TEST_CASE("GameLogic services: a table with too small an ABI version is dropped alone")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_SERVICES_LIB;
    REQUIRE(std::filesystem::exists(libPath));

    ServicesRig rig;
    HE::GameLogicLoader loader;
    REQUIRE(loader.load(libPath));
    auto* probe = probeOf(loader);

    // A physics table the module is too new for is refused — it would read past
    // what the engine filled — while save and input come through untouched.
    HeEngineServices mixed = rig.umbrella;
    HePhysicsServices tooOld = rig.phys;
    tooOld.abiVersion = HE_PHYSICS_ABI_VERSION - 1;
    mixed.physics = &tooOld;
    REQUIRE(loader.injectServices(&mixed));
    CHECK_FALSE(probe->physicsAvailable());
    CHECK(probe->saveAvailable());
    CHECK(probe->inputAvailable());

    // A NEWER engine table is accepted: append-only growth makes the module's
    // struct a prefix of the engine's, so reading it is safe.
    HePhysicsServices newer = rig.phys;
    newer.abiVersion = HE_PHYSICS_ABI_VERSION + 5;
    mixed.physics = &newer;
    REQUIRE(loader.injectServices(&mixed));
    CHECK(probe->physicsAvailable());

    // The umbrella carries the rule PER POINTER, at the version each pointer was
    // appended at — not once for the whole struct. An older umbrella is still a
    // valid prefix: what it does contain is readable, and only what came later
    // has to be refused. Dropping everything instead would mean a module built
    // today loses its savegame API on last month's engine, which is the failure
    // the V2 export exists to avoid in the first place.
    HeEngineServices olderUmbrella = rig.umbrella;
    olderUmbrella.abiVersion = 1u;   // save/physics/input, no content
    REQUIRE(loader.injectServices(&olderUmbrella));
    CHECK(probe->saveAvailable());
    CHECK(probe->physicsAvailable());
    CHECK(probe->inputAvailable());
    CHECK_FALSE(probe->contentAvailable());

    // Below every version there is nothing left to read, and the whole struct
    // goes.
    HeEngineServices badUmbrella = rig.umbrella;
    badUmbrella.abiVersion = 0u;
    REQUIRE(loader.injectServices(&badUmbrella));
    CHECK_FALSE(probe->saveAvailable());
    CHECK_FALSE(probe->physicsAvailable());
    CHECK_FALSE(probe->inputAvailable());
    CHECK_FALSE(probe->contentAvailable());

    // A null table pointer is a state, not a crash.
    HeEngineServices sparse{};
    sparse.abiVersion = HE_SERVICES_ABI_VERSION;
    sparse.input      = &rig.input;
    REQUIRE(loader.injectServices(&sparse));
    CHECK_FALSE(probe->saveAvailable());
    CHECK_FALSE(probe->physicsAvailable());
    CHECK(probe->inputAvailable());

    loader.unload(rig.world);
}

TEST_CASE("GameLogic services: a library without the receiving exports is a state, not an error")
{
    // The older fixture implements IGameLogic and nothing else — no
    // HE_SetEngineServices, no …V2. That is exactly the shape of a project
    // scaffolded before this header existed, and the loader has to survive it.
    const std::filesystem::path oldLib = HE_TEST_GAMELOGIC_LIB;
    REQUIRE(std::filesystem::exists(oldLib));

    ServicesRig rig;
    HE::GameLogicLoader loader;
    REQUIRE(loader.load(oldLib));
    CHECK_FALSE(loader.injectServices(&rig.umbrella));   // neither export found
    CHECK_FALSE(loader.injectServices(&rig.save));       // v1 form, same answer

    // It still runs; it simply cannot reach the engine.
    loader.logic()->onStart(rig.world);
    loader.unload(rig.world);
}

TEST_CASE("GameLogic services: content reaches a loaded C++ module as ids, never pointers")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_SERVICES_LIB;
    REQUIRE(std::filesystem::exists(libPath));

    ServicesRig rig;
    HE::GameLogicLoader loader;
    REQUIRE(loader.load(libPath));
    auto* probe = probeOf(loader);
    REQUIRE(probe != nullptr);

    // Before injection: no table, no crash, an invalid id.
    CHECK_FALSE(probe->contentAvailable());
    CHECK_FALSE(probe->doLoadAsset("Rock.hasset").valid());

    REQUIRE(loader.injectServices(&rig.umbrella));
    loader.logic()->onStart(rig.world);
    CHECK(probe->contentAvailableAtStart());   // a warm-up load belongs in onStart
    CHECK(probe->contentAvailable());

    char name[64] = {};

    SUBCASE("load, type, unload — every value a copy")
    {
        const he::AssetId rock = probe->doLoadAsset("Rock.hasset");
        REQUIRE(rock.valid());
        CHECK(probe->doIsAssetLoaded(rock));
        // The engine agrees with what crossed: same asset, same identity.
        CHECK(rig.content.isLoaded(HE::UUID{ rock.hi, rock.lo }));
        CHECK(rig.content.idForPath("Rock.hasset") == HE::UUID{ rock.hi, rock.lo });

        REQUIRE(probe->doAssetTypeName(rock, name, (int)sizeof name) == 7);
        CHECK(std::string(name) == "Texture");

        CHECK(probe->doUnloadAsset(rock));
        CHECK_FALSE(probe->doIsAssetLoaded(rock));
        CHECK_FALSE(rig.content.isLoaded(HE::UUID{ rock.hi, rock.lo }));
        CHECK_FALSE(probe->doUnloadAsset(rock));   // already gone
    }

    // The reason this table trades ids: a pointer taken here would be dangling
    // after the very next load, and the module's call order is not something the
    // engine can see. An id is a value and simply keeps answering.
    SUBCASE("an id keeps answering across the loads that move the asset pool")
    {
        const he::AssetId rock = probe->doLoadAsset("Rock.hasset");
        REQUIRE(rock.valid());
        REQUIRE(probe->doAssetTypeName(rock, name, (int)sizeof name) == 7);
        CHECK(std::string(name) == "Texture");

        // Two more registrations — this is what invalidates every pointer the
        // ContentManager ever handed out, and every string those assets own.
        const he::AssetId moss = probe->doLoadAsset("Moss.hasset");
        const he::AssetId cube = probe->doLoadAsset("Cube.hasset");
        REQUIRE(moss.valid());
        REQUIRE(cube.valid());
        CHECK_FALSE(moss == rock);

        CHECK(probe->doIsAssetLoaded(rock));
        char again[64] = {};
        CHECK(probe->doAssetTypeName(rock, again, (int)sizeof again) == 7);
        CHECK(std::string(again) == "Texture");
        CHECK(probe->doAssetTypeName(cube, again, (int)sizeof again) == 10);
        CHECK(std::string(again) == "StaticMesh");

        // Unloading a DIFFERENT asset swap-and-pops the pool. Same answer.
        CHECK(probe->doUnloadAsset(moss));
        CHECK(probe->doIsAssetLoaded(rock));
        CHECK(probe->doAssetTypeName(rock, again, (int)sizeof again) == 7);
        CHECK(std::string(again) == "Texture");
    }

    SUBCASE("the name getter reports the full length into a buffer that is too small")
    {
        const he::AssetId cube = probe->doLoadAsset("Cube.hasset");
        REQUIRE(cube.valid());
        char tiny[4] = { 'x', 'x', 'x', 'x' };
        // Straight at the table: cap-1 bytes plus a NUL, the FULL length back.
        CHECK(probe->doAssetTypeNameRaw(cube, tiny, (int)sizeof tiny) == 10);
        CHECK(std::string(tiny) == "Sta");
        // Through the wrapper, which grows and retries for the caller.
        CHECK(probe->doAssetTypeName(cube, name, (int)sizeof name) == 10);
        CHECK(std::string(name) == "StaticMesh");
    }

    SUBCASE("an unknown path and a zero id are answers, not crashes")
    {
        CHECK_FALSE(probe->doLoadAsset("NoSuchThing.hasset").valid());
        CHECK_FALSE(probe->doLoadAsset("").valid());
        const he::AssetId zero{};
        CHECK_FALSE(probe->doIsAssetLoaded(zero));
        CHECK_FALSE(probe->doUnloadAsset(zero));
        CHECK(probe->doAssetTypeName(zero, name, (int)sizeof name) == 0);
        const he::AssetId invented{ 0xDEAD, 0xBEEF };
        CHECK_FALSE(probe->doIsAssetLoaded(invented));
        CHECK(probe->doAssetTypeName(invented, name, (int)sizeof name) == 0);
    }

    SUBCASE("an umbrella from before the content table costs only content")
    {
        HeEngineServices old = rig.umbrella;
        old.abiVersion = 1u;   // save/physics/input only
        REQUIRE(loader.injectServices(&old));
        CHECK(probe->saveAvailable());
        CHECK(probe->physicsAvailable());
        CHECK(probe->inputAvailable());
        CHECK_FALSE(probe->contentAvailable());
        CHECK_FALSE(probe->doLoadAsset("Rock.hasset").valid());

        // A null content pointer under a current umbrella is the same state.
        HeEngineServices sparse = rig.umbrella;
        sparse.content = nullptr;
        REQUIRE(loader.injectServices(&sparse));
        CHECK(probe->saveAvailable());
        CHECK_FALSE(probe->contentAvailable());
    }

    loader.unload(rig.world);
}

// ── The hot-swap the editor's "Build and Reload" runs ────────────────────────
// The failure this pins is the quiet one: a reload that forgets to inject
// leaves an image whose every he::* call is a no-op with a default return. No
// crash, no log, just a module that stopped doing anything — which is why the
// sequence lives in the loader and both halves are checked here.
TEST_CASE("GameLogic services: a hot swap re-injects, an unassisted reload does not")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_SERVICES_LIB;
    REQUIRE(std::filesystem::exists(libPath));

    ServicesRig rig;
    spawnStaticBody(rig.world, { 0.0f, 0.0f, 0.0f }, "Target");
    rig.physics.initialize(rig.world);

    HE::GameLogicLoader loader;
    REQUIRE(loader.loadAndStart(libPath, rig.world, &rig.umbrella));
    auto* first = probeOf(loader);
    REQUIRE(first != nullptr);
    CHECK(first->servicesAvailableAtStart());
    CHECK(first->physicsAvailableAtStart());
    CHECK(first->contentAvailableAtStart());
    loader.logic()->onUpdate(rig.world, 1.0f / 60.0f);
    CHECK(first->updateCount() == 1);

    SUBCASE("reloadAndStart: the new image is live from ITS onStart on")
    {
        REQUIRE(loader.reloadAndStart(libPath, rig.world, &rig.umbrella));
        auto* fresh = probeOf(loader);
        REQUIRE(fresh != nullptr);
        // A new object in a new image: the counter starts over.
        CHECK(fresh->updateCount() == 0);
        // …and the tables were there before onStart, not after it.
        CHECK(fresh->servicesAvailableAtStart());
        CHECK(fresh->physicsAvailableAtStart());
        CHECK(fresh->inputAvailableAtStart());
        CHECK(fresh->contentAvailableAtStart());
        // Still answering real values across the boundary after the swap.
        CHECK(fresh->doRaycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 50.0f).hit);
        CHECK(fresh->doLoadAsset("Rock.hasset").valid());
    }

    SUBCASE("raw reload() + onStart: the module is silent, and that is the trap")
    {
        REQUIRE(loader.reload(libPath, rig.world));
        auto* fresh = probeOf(loader);
        REQUIRE(fresh != nullptr);
        loader.logic()->onStart(rig.world);
        CHECK_FALSE(fresh->servicesAvailableAtStart());
        CHECK_FALSE(fresh->physicsAvailable());
        CHECK_FALSE(fresh->contentAvailable());
        // Not a crash — a default. Which is exactly what makes it hard to see.
        CHECK_FALSE(fresh->doRaycast({ 0.0f, 10.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 50.0f).hit);
        CHECK_FALSE(fresh->doLoadAsset("Rock.hasset").valid());
    }

    loader.unload(rig.world);
}
