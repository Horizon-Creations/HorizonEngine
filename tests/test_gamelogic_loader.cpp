#include "doctest.h"
#include <Application/GameLogicLoader.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <filesystem>

// HE_TEST_GAMELOGIC_LIB is defined by the test CMakeLists as the full path to
// the test_gamelogic fixture library built alongside he_tests.
#ifndef HE_TEST_GAMELOGIC_LIB
#  define HE_TEST_GAMELOGIC_LIB ""
#endif

namespace {

bool hasEntityNamed(HorizonWorld& world, const std::string& n)
{
    for (auto [e, name] : world.registry().view<NameComponent>().each())
        if (name.name == n) return true;
    return false;
}

int countEntitiesNamed(HorizonWorld& world, const std::string& n)
{
    int count = 0;
    for (auto [e, name] : world.registry().view<NameComponent>().each())
        if (name.name == n) ++count;
    return count;
}

float nativeEntityX(HorizonWorld& world)
{
    for (auto [e, name, t] :
         world.registry().view<NameComponent, TransformComponent>().each())
        if (name.name == "FromNativeLogic") return t.position.x;
    return -1.0f;
}

} // namespace

TEST_CASE("GameLogicLoader: full lifecycle against a real dylib")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_LIB;
    REQUIRE(!libPath.empty());
    REQUIRE(std::filesystem::exists(libPath));

    HorizonWorld world;
    HE::GameLogicLoader loader;
    CHECK(!loader.isLoaded());
    CHECK(loader.logic() == nullptr);

    // Load resolves the exports and constructs the logic object.
    REQUIRE(loader.load(libPath));
    REQUIRE(loader.isLoaded());
    REQUIRE(loader.logic() != nullptr);

    // onStart creates the marker entity.
    loader.logic()->onStart(world);
    REQUIRE(hasEntityNamed(world, "FromNativeLogic"));
    CHECK(nativeEntityX(world) == doctest::Approx(0.0f));

    // onUpdate mutates world state each call (what GameLoop::tick drives).
    loader.logic()->onUpdate(world, 1.0f / 60.0f);
    loader.logic()->onUpdate(world, 1.0f / 60.0f);
    CHECK(nativeEntityX(world) == doctest::Approx(2.0f));

    // unload runs onStop first, then tears down.
    loader.unload(world);
    CHECK(!loader.isLoaded());
    CHECK(loader.logic() == nullptr);
    CHECK(hasEntityNamed(world, "NativeLogicStopped"));
}

TEST_CASE("GameLogicLoader: reload works and unique hot-copies do not collide")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_LIB;
    REQUIRE(std::filesystem::exists(libPath));

    HorizonWorld world;
    HE::GameLogicLoader loader;
    REQUIRE(loader.load(libPath));
    loader.logic()->onStart(world);

    // reload = onStop → new load; both images were distinct hot-copies.
    REQUIRE(loader.reload(libPath, world));
    REQUIRE(loader.isLoaded());
    CHECK(hasEntityNamed(world, "NativeLogicStopped")); // onStop ran during reload

    loader.logic()->onStart(world);
    loader.logic()->onUpdate(world, 0.016f);
    CHECK(nativeEntityX(world) >= 0.0f);

    loader.unload(world);
}

// The editor's "Build and Reload" cycle, minus the compiler: the sequence the
// button runs is one call, so no caller can get half of it right.
TEST_CASE("GameLogicLoader: loadAndStart/reloadAndStart run the whole sequence")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_LIB;
    REQUIRE(std::filesystem::exists(libPath));

    HorizonWorld world;
    HE::GameLogicLoader loader;

    // loadAndStart fires onStart itself — the marker entity is there without the
    // caller having asked for it.
    REQUIRE(loader.loadAndStart(libPath, world, nullptr));
    REQUIRE(loader.isLoaded());
    CHECK(countEntitiesNamed(world, "FromNativeLogic") == 1);
    CHECK(countEntitiesNamed(world, "NativeLogicStopped") == 0);

    loader.logic()->onUpdate(world, 1.0f / 60.0f);
    CHECK(nativeEntityX(world) == doctest::Approx(1.0f));

    // The hot swap: onStop on the outgoing image, onStart on the incoming one.
    // The play session is what stays — the world is the same object throughout.
    REQUIRE(loader.reloadAndStart(libPath, world, nullptr));
    REQUIRE(loader.isLoaded());
    CHECK(countEntitiesNamed(world, "NativeLogicStopped") == 1);
    CHECK(countEntitiesNamed(world, "FromNativeLogic") == 2);

    // A second swap on the same loader: the numbered hot-copies do not collide,
    // which is what makes the button pressable more than once per session.
    REQUIRE(loader.reloadAndStart(libPath, world, nullptr));
    CHECK(countEntitiesNamed(world, "NativeLogicStopped") == 2);
    CHECK(countEntitiesNamed(world, "FromNativeLogic") == 3);

    loader.unload(world);
    CHECK(!loader.isLoaded());
}

TEST_CASE("GameLogicLoader: a failed reloadAndStart still stopped the old image")
{
    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_LIB;
    REQUIRE(std::filesystem::exists(libPath));

    HorizonWorld world;
    HE::GameLogicLoader loader;
    REQUIRE(loader.loadAndStart(libPath, world, nullptr));

    // The library that was there is gone by the time the new one is found to be
    // missing — its code is exactly what a rebuild replaces, so keeping it would
    // be the dishonest outcome. onStop ran; nothing is loaded afterwards.
    CHECK_FALSE(loader.reloadAndStart("/nonexistent/NoSuchGameLogic.dylib", world, nullptr));
    CHECK_FALSE(loader.isLoaded());
    CHECK(loader.logic() == nullptr);
    CHECK(countEntitiesNamed(world, "NativeLogicStopped") == 1);
}

TEST_CASE("GameLogicLoader: missing file and double-load are rejected")
{
    HorizonWorld world;
    HE::GameLogicLoader loader;
    CHECK(!loader.load("/nonexistent/NoSuchGameLogic.dylib"));
    CHECK(!loader.isLoaded());

    const std::filesystem::path libPath = HE_TEST_GAMELOGIC_LIB;
    REQUIRE(loader.load(libPath));
    CHECK(!loader.load(libPath));   // second load while loaded → rejected
    loader.unload(world);
}
