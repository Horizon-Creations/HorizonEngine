#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/WeatherSystem.h>
#include <HorizonScene/Components/WeatherComponent.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderPass.h>
#include <HorizonRendering/CommandBuffer.h>
#include <cmath>

// Build a world with an EnvironmentComponent on the root and a WeatherComponent
// (also on the root, where weather is authored).
static WeatherComponent& setupWeatherWorld(HorizonWorld& world)
{
    auto& reg = world.registry();
    reg.emplace_or_replace<EnvironmentComponent>(world.rootEntity());
    return reg.emplace_or_replace<WeatherComponent>(world.rootEntity());
}

TEST_CASE("WeatherComponent defaults")
{
    WeatherComponent w;
    CHECK(w.currentKind == WeatherKind::Clear);
    CHECK(w.targetKind  == WeatherKind::Clear);
    CHECK(w.intensity   == doctest::Approx(1.0f));
    CHECK(w.transitionDuration == doctest::Approx(8.0f));
    CHECK(!w.autoCycle);
}

TEST_CASE("weatherPreset table is ordered clear -> storm")
{
    CHECK(weatherPreset(WeatherKind::Clear).cloudCoverage <
          weatherPreset(WeatherKind::Storm).cloudCoverage);
    CHECK(weatherPreset(WeatherKind::Storm).precipType == PrecipType::Rain);
    CHECK(weatherPreset(WeatherKind::Snow).precipType  == PrecipType::Snow);
    CHECK(weatherPreset(WeatherKind::Storm).lightning);
    CHECK(!weatherPreset(WeatherKind::Clear).lightning);
}

TEST_CASE("settled weather snaps outputs to the target preset")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Overcast;

    WeatherSystem::update(world, 0.016f);

    const WeatherPreset p = weatherPreset(WeatherKind::Overcast);
    CHECK(w.curCloudCoverage == doctest::Approx(p.cloudCoverage));
    CHECK(w.curFogDensity    == doctest::Approx(p.fogDensity));
}

TEST_CASE("weather transition blends then settles after the duration")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.transitionDuration = 8.0f;

    WeatherSystem::update(world, 0.0f);             // settle on Clear
    const float clearCloud = w.curCloudCoverage;

    w.targetKind = WeatherKind::Storm;
    WeatherSystem::update(world, 4.0f);             // halfway
    CHECK(w.currentKind == WeatherKind::Clear);     // not done yet
    CHECK(w.curCloudCoverage > clearCloud);
    CHECK(w.curCloudCoverage < weatherPreset(WeatherKind::Storm).cloudCoverage);

    WeatherSystem::update(world, 4.0f);             // complete
    CHECK(w.currentKind == WeatherKind::Storm);
    CHECK(w.curCloudCoverage == doctest::Approx(weatherPreset(WeatherKind::Storm).cloudCoverage));
}

TEST_CASE("weather writes through into the EnvironmentComponent")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Rain;

    WeatherSystem::update(world, 0.016f);

    auto* env = world.registry().try_get<EnvironmentComponent>(world.rootEntity());
    REQUIRE(env != nullptr);
    CHECK(env->cloudCoverage == doctest::Approx(w.curCloudCoverage));
    CHECK(env->fogDensity    == doctest::Approx(w.curFogDensity));
    CHECK(env->windSpeed     == doctest::Approx(w.curWindSpeed).epsilon(0.5)); // gust-modulated
}

TEST_CASE("manual env edits are respected; a new preset reclaims them")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    auto* env = world.registry().try_get<EnvironmentComponent>(world.rootEntity());
    w.currentKind = w.targetKind = WeatherKind::Overcast;

    WeatherSystem::update(world, 0.016f); // selecting Overcast applies its values
    CHECK(env->cloudCoverage == doctest::Approx(weatherPreset(WeatherKind::Overcast).cloudCoverage));

    // The user drags the cloud slider — weather must not stomp it the next frame.
    env->cloudCoverage = 0.123f;
    WeatherSystem::update(world, 0.016f);
    CHECK(env->cloudCoverage == doctest::Approx(0.123f));

    // Picking a new preset reclaims every value, overriding the manual edit.
    w.targetKind = WeatherKind::Clear;
    for (int i = 0; i < 200; ++i) WeatherSystem::update(world, 0.1f); // run the transition out
    CHECK(env->cloudCoverage ==
          doctest::Approx(weatherPreset(WeatherKind::Clear).cloudCoverage).epsilon(0.02));
}

TEST_CASE("rain stops once the rain amount returns to zero")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    auto* env = world.registry().try_get<EnvironmentComponent>(world.rootEntity());
    w.currentKind = w.targetKind = WeatherKind::Rain;
    w.transitionDuration = 0.1f;

    WeatherSystem::update(world, 0.5f, glm::vec3(0.0f));
    REQUIRE(env->rainAmount > 0.0f);
    REQUIRE(!w.precip.empty());

    // Manually zero the rain — the precipitation must drain out (the reported bug).
    env->rainAmount = 0.0f;
    for (int i = 0; i < 60; ++i)
        WeatherSystem::update(world, 0.2f, glm::vec3(0.0f));
    CHECK(env->rainAmount == doctest::Approx(0.0f));
    CHECK(w.precip.empty());
}

TEST_CASE("intensity 0 collapses the weather to a calm sky")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Storm;
    w.intensity   = 0.0f;

    WeatherSystem::update(world, 0.016f);

    CHECK(w.curCloudCoverage == doctest::Approx(0.0f));
    CHECK(w.curFogDensity    == doctest::Approx(0.0f));
    CHECK(w.curWindSpeed     == doctest::Approx(1.0f)); // no wind boost at intensity 0
}

TEST_CASE("retargeting mid-transition stays continuous (no jump)")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.transitionDuration = 8.0f;

    WeatherSystem::update(world, 0.0f);     // settle on Clear
    w.targetKind = WeatherKind::Storm;
    WeatherSystem::update(world, 4.0f);     // halfway to Storm
    const float mid = w.curCloudCoverage;

    w.targetKind = WeatherKind::Foggy;      // retarget mid-transition
    WeatherSystem::update(world, 0.001f);   // tiny step — must not jump
    CHECK(w.curCloudCoverage == doctest::Approx(mid).epsilon(0.02));
}

TEST_CASE("WeatherSystem is a no-op without a WeatherComponent")
{
    HorizonWorld world;
    auto& reg = world.registry();
    reg.remove<WeatherComponent>(world.rootEntity()); // World root carries one by default
    auto& env = reg.emplace_or_replace<EnvironmentComponent>(world.rootEntity());
    env.cloudCoverage = 0.33f;

    WeatherSystem::update(world, 0.5f);     // no WeatherComponent present

    CHECK(env.cloudCoverage == doctest::Approx(0.33f)); // untouched
}

TEST_CASE("every World root has a WeatherComponent by default")
{
    HorizonWorld world;
    CHECK(world.registry().all_of<WeatherComponent>(world.rootEntity()));

    world.clear(); // New Scene must keep weather on the root
    CHECK(world.registry().all_of<WeatherComponent>(world.rootEntity()));
}

TEST_CASE("rain spawns a camera-following precipitation volume")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Rain;

    WeatherSystem::update(world, 0.5f, glm::vec3(0.0f)); // settles + emits in one tick

    CHECK(!w.precip.empty());
    CHECK(w.curPrecipType == PrecipType::Rain);
}

TEST_CASE("clear weather produces no precipitation")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Clear;

    WeatherSystem::update(world, 0.5f, glm::vec3(0.0f));

    CHECK(w.precip.empty());
}

TEST_CASE("snow precipitation uses the snow type")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Snow;

    WeatherSystem::update(world, 0.5f, glm::vec3(0.0f));

    CHECK(!w.precip.empty());
    CHECK(w.curPrecipType == PrecipType::Snow);
}

TEST_CASE("precipitation falls downward over time")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Rain;

    WeatherSystem::update(world, 0.2f, glm::vec3(0.0f)); // spawn
    REQUIRE(!w.precip.empty());
    const float y0 = w.precip.front().position.y;

    WeatherSystem::update(world, 0.1f, glm::vec3(0.0f)); // integrate
    CHECK(w.precip.front().position.y < y0);
}

TEST_CASE("precipitation drains after the weather clears")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Rain;
    w.transitionDuration = 0.1f;

    WeatherSystem::update(world, 0.5f, glm::vec3(0.0f)); // build up rain
    REQUIRE(!w.precip.empty());

    w.targetKind = WeatherKind::Clear;
    for (int i = 0; i < 60; ++i)                          // let drops fall out
        WeatherSystem::update(world, 0.2f, glm::vec3(0.0f));

    CHECK(w.precip.empty());
}

TEST_CASE("settled wind is steady so the clouds do not wobble")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Storm;
    auto* env = world.registry().try_get<EnvironmentComponent>(world.rootEntity());

    WeatherSystem::update(world, 0.3f, glm::vec3(0.0f));
    const float wind0 = env->windSpeed;
    WeatherSystem::update(world, 0.3f, glm::vec3(0.0f));
    const float wind1 = env->windSpeed;

    CHECK(wind0 == doctest::Approx(wind1)); // no per-frame gust → smooth cloud drift
    CHECK(wind0 > 0.0f);
}

TEST_CASE("lightning strikes during a storm and writes the flash")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Storm; // lightningCountdown defaults to 0 → strikes now
    auto* env = world.registry().try_get<EnvironmentComponent>(world.rootEntity());

    WeatherSystem::update(world, 0.016f, glm::vec3(0.0f));

    CHECK(w.flashTriggered);
    CHECK(w.flashIntensity > 0.0f);
    CHECK(env->flash > 0.0f);
}

TEST_CASE("no lightning in clear weather")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Clear;

    for (int i = 0; i < 20; ++i)
    {
        WeatherSystem::update(world, 0.1f, glm::vec3(0.0f));
        CHECK(!w.flashTriggered);
    }
    CHECK(w.flashIntensity == doctest::Approx(0.0f));
}

TEST_CASE("lightning flash decays after a strike")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Storm;

    WeatherSystem::update(world, 0.016f, glm::vec3(0.0f)); // strike
    REQUIRE(w.flashIntensity > 0.0f);
    const float peak = w.flashIntensity;

    w.lightningCountdown = 100.0f;                         // suppress further strikes
    WeatherSystem::update(world, 0.1f, glm::vec3(0.0f));
    CHECK(w.flashIntensity < peak);                        // fading
}

TEST_CASE("RenderExtractor emits rain as vertical-streak billboards")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Rain;
    WeatherSystem::update(world, 0.3f, glm::vec3(0.0f)); // spawn rain drops
    REQUIRE(!w.precip.empty());
    const size_t drops = w.precip.size();

    RenderWorld rw;
    RenderExtractor extractor;
    extractor.extract(world, rw, 16.0f / 9.0f);

    // Each live drop becomes one billboard RenderObject (empty scene has no meshes).
    CHECK(rw.objects.size() == drops);

    // Transforms must be finite, and a rain streak is longer (Y axis) than it is wide.
    const glm::mat4& m = rw.objects.front().transform;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(std::isfinite(m[c][r]));
    const float widthAxis  = glm::length(glm::vec3(m[0]));
    const float lengthAxis = glm::length(glm::vec3(m[1]));
    CHECK(lengthAxis > widthAxis);

    // Precipitation billboards opt out of the shadow + SSAO passes (perf + correctness).
    CHECK_FALSE(rw.objects.front().castsShadow);
    CHECK_FALSE(rw.objects.front().contributesAO);
}

TEST_CASE("ShadowPass skips non-shadow-casting billboards")
{
    RenderWorld rw;
    rw.shadow.enabled  = true;
    rw.shadow.viewProj = glm::mat4(1.0f); // identity → NDC-cube light frustum

    RenderObject caster;     // default castsShadow = true; invalid bounds = never culled
    rw.objects.push_back(caster);
    RenderObject billboard;  // a precipitation-style billboard
    billboard.castsShadow = false;
    rw.objects.push_back(billboard);

    CommandBuffer cmds;
    std::vector<uint32_t> sorted; // ShadowPass iterates world.objects, ignores this
    ShadowPass pass;
    pass.execute(rw, sorted, cmds);

    CHECK(cmds.drawCalls().size() == 1); // only the caster is recorded
}

TEST_CASE("precipitation respects the max-particle cap")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Rain;
    w.maxRainParticles = 40;

    for (int i = 0; i < 40; ++i)
        WeatherSystem::update(world, 0.1f, glm::vec3(0.0f));

    CHECK(static_cast<int>(w.precip.size()) <= 40);
}

TEST_CASE("precipitation dies at the ground level without physics")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Rain;
    w.groundLevel = 10.0f;

    for (int i = 0; i < 20; ++i)
        WeatherSystem::update(world, 0.1f, glm::vec3(0.0f)); // camera at origin, no physics

    REQUIRE(!w.precip.empty());
    for (const Particle& p : w.precip)
        CHECK(p.position.y > w.groundLevel); // nothing survives below the ground plane
}

TEST_CASE("rain velocity leans with the wind direction")
{
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    auto* env = world.registry().try_get<EnvironmentComponent>(world.rootEntity());
    env->windDirection = 90.0f; // blows toward +X
    w.currentKind = w.targetKind = WeatherKind::Rain;

    WeatherSystem::update(world, 0.3f, glm::vec3(0.0f));

    REQUIRE(!w.precip.empty());
    CHECK(w.precip.front().velocity.x != doctest::Approx(0.0f)); // horizontal wind slant
}
