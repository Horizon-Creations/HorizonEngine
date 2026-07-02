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
