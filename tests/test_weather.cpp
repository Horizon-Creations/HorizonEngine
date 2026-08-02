#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/WeatherSystem.h>
#include <HorizonScene/Components/WeatherComponent.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/EnvironmentLightComponent.h>
#include <HorizonScene/EnvironmentPush.h>   // HE::makeEnvironmentSettings
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
    // A bare world has no weather; give it a sky to check weather leaves env alone.
    auto& env = reg.emplace_or_replace<EnvironmentComponent>(world.rootEntity());
    env.cloudCoverage = 0.33f;

    WeatherSystem::update(world, 0.5f);     // no WeatherComponent present

    CHECK(env.cloudCoverage == doctest::Approx(0.33f)); // untouched
}

static int envLightCount(HorizonWorld& w)
{
    int n = 0;
    for (auto e : w.registry().view<EnvironmentLightComponent>()) { (void)e; ++n; }
    return n;
}

TEST_CASE("a bare world has no Sky/Weather; add/remove create and destroy them")
{
    HorizonWorld world;
    // A fresh world starts empty — Sky/Weather come from the project templates or
    // the Environment window, not by default.
    CHECK((world.environmentEntity() == entt::null));
    CHECK((world.weatherEntity()     == entt::null));
    CHECK(envLightCount(world) == 0);

    const Entity sky = world.addSky();
    CHECK(sky == world.environmentEntity());
    CHECK(world.registry().all_of<EnvironmentComponent>(sky));
    CHECK(envLightCount(world) == 2); // sun + moon, as children of the Sky entity

    const Entity weather = world.addWeather();
    CHECK(weather == world.weatherEntity());
    CHECK(world.registry().all_of<WeatherComponent>(weather));

    // add* are idempotent — never a second entity.
    CHECK(world.addSky()     == sky);
    CHECK(world.addWeather() == weather);

    // removeSky takes the sun/moon child lights with it.
    world.removeSky();
    CHECK((world.environmentEntity() == entt::null));
    CHECK(envLightCount(world) == 0);
    world.removeWeather();
    CHECK((world.weatherEntity() == entt::null));

    // New Scene (clear) leaves a bare world too.
    world.addSky();
    world.addWeather();
    world.clear();
    CHECK((world.environmentEntity() == entt::null));
    CHECK((world.weatherEntity()     == entt::null));
    CHECK(envLightCount(world) == 0);
}

TEST_CASE("legacy root Environment/Weather migrate onto dedicated entities on load")
{
    HorizonWorld world;
    auto& reg = world.registry();
    // Simulate a legacy scene: Environment + Weather sitting on the World root.
    reg.emplace_or_replace<EnvironmentComponent>(world.rootEntity());
    reg.emplace_or_replace<WeatherComponent>(world.rootEntity());

    world.migrateLegacyRootEnvironment();

    CHECK_FALSE(reg.all_of<EnvironmentComponent>(world.rootEntity()));
    CHECK_FALSE(reg.all_of<WeatherComponent>(world.rootEntity()));
    const Entity sky = world.environmentEntity();
    const Entity weather = world.weatherEntity();
    CHECK((sky     != entt::null));
    CHECK((weather != entt::null));
    CHECK(sky     != world.rootEntity());
    CHECK(weather != world.rootEntity());
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

// ─────────────────────────────────────────────────────────────────────────────
// HE::makeEnvironmentSettings — the single EnvironmentComponent → renderer field
// map shared by the editor viewport (EditorApplication::pushEnvironment) and the
// packaged game runtime (GameApplication::OnRender). Both used to carry their own
// copy of these ~45 lines; these tests pin the mapping AND the day/night
// side effect so a future sky knob cannot be added to one side only.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("makeEnvironmentSettings maps every EnvironmentComponent field")
{
    EnvironmentComponent env;
    // Distinct values so a mis-wired field shows up as the wrong number, not zero.
    env.dayNightCycle      = true;
    env.timeOfDay          = 0.31f;
    env.sunColor           = glm::vec3(0.11f, 0.12f, 0.13f);
    env.sunIntensity       = 2.71f;
    env.moonColor          = glm::vec3(0.21f, 0.22f, 0.23f);
    env.moonIntensity      = 0.41f;
    env.moonPhase          = 0.37f;
    env.cloudCoverage      = 0.61f;
    env.fogDensity         = 0.071f;
    env.fogHeightFalloff   = 0.081f;
    env.auroraIntensity    = 0.91f;
    env.milkyWayIntensity  = 0.51f;
    env.nebulaIntensity    = 0.52f;
    env.nebulaColor        = glm::vec3(0.31f, 0.32f, 0.33f);
    env.nebulaColor2       = glm::vec3(0.41f, 0.42f, 0.43f);
    env.nebulaColor3       = glm::vec3(0.51f, 0.52f, 0.53f);
    env.nebulaSeed         = 17.0f;
    env.nebulaCoverage     = 0.62f;
    env.nebulaQuality      = 2;
    env.auroraColor        = glm::vec3(0.61f, 0.62f, 0.63f);
    env.auroraColorTop     = glm::vec3(0.71f, 0.72f, 0.73f);
    env.auroraHeight       = 0.26f;
    env.auroraFragmentation = 0.36f;
    env.windDirection      = 123.0f;
    env.windSpeed          = 2.5f;
    env.flash              = 0.83f;
    env.wetness            = 0.44f;
    env.snowAmount         = 0.55f;
    env.rainAmount         = 0.66f;
    env.cloudMode          = 1;
    env.cloudHeight        = 321.0f;
    env.cloudQuality       = 2;
    env.lowResClouds       = true;
    env.cloudDensity       = 1.7f;
    env.cloudFluffiness    = 0.77f;
    env.cloudTint          = glm::vec3(0.81f, 0.82f, 0.83f);
    env.contrailAmount     = 0.24f;
    env.cirrusAmount       = 0.34f;
    env.cirrusSeed         = 9.0f;
    env.godRays            = 0.45f;
    env.shootingStars      = 0.56f;
    env.lensFlare          = 0.67f;
    env.starBrightness     = 1.3f;
    env.starColor          = glm::vec3(0.91f, 0.92f, 0.93f);
    env.starSize           = 1.4f;
    env.starSizeVariation  = 0.28f;
    env.starGlow           = 1.5f;
    env.starTwinkle        = 0.38f;
    env.starDensity        = 0.48f;
    // Auto-advance off → the mapping is observed without any mutation.
    env.autoAdvance = false;

    const IRenderer::EnvironmentSettings s = HE::makeEnvironmentSettings(env, 0.016f);

    // A Sky entity exists here, so the sky pass stays on (the callers push
    // EnvironmentSettings{ .skyEnabled = false } themselves when there is none).
    CHECK(s.skyEnabled);
    CHECK(s.dayNightCycle);
    CHECK(s.timeOfDay         == doctest::Approx(0.31f));
    CHECK(s.sunColor.r        == doctest::Approx(0.11f));
    CHECK(s.sunColor.b        == doctest::Approx(0.13f));
    CHECK(s.sunIntensity      == doctest::Approx(2.71f));
    CHECK(s.moonColor.g       == doctest::Approx(0.22f));
    CHECK(s.moonIntensity     == doctest::Approx(0.41f));
    CHECK(s.moonPhase         == doctest::Approx(0.37f));
    CHECK(s.cloudCoverage     == doctest::Approx(0.61f));
    CHECK(s.fogDensity        == doctest::Approx(0.071f));
    CHECK(s.fogHeightFalloff  == doctest::Approx(0.081f));
    CHECK(s.auroraIntensity   == doctest::Approx(0.91f));
    CHECK(s.milkyWayIntensity == doctest::Approx(0.51f));
    CHECK(s.nebulaIntensity   == doctest::Approx(0.52f));
    CHECK(s.nebulaColor.r     == doctest::Approx(0.31f));
    CHECK(s.nebulaColor2.g    == doctest::Approx(0.42f));
    CHECK(s.nebulaColor3.b    == doctest::Approx(0.53f));
    CHECK(s.nebulaSeed        == doctest::Approx(17.0f));
    CHECK(s.nebulaCoverage    == doctest::Approx(0.62f));
    CHECK(s.nebulaQuality     == 2);
    CHECK(s.auroraColor.r     == doctest::Approx(0.61f));
    CHECK(s.auroraColorTop.b  == doctest::Approx(0.73f));
    CHECK(s.auroraHeight      == doctest::Approx(0.26f));
    CHECK(s.auroraFragmentation == doctest::Approx(0.36f));
    CHECK(s.windDirection     == doctest::Approx(123.0f));
    CHECK(s.windSpeed         == doctest::Approx(2.5f));
    CHECK(s.flash             == doctest::Approx(0.83f));
    CHECK(s.wetness           == doctest::Approx(0.44f));
    CHECK(s.snowAmount        == doctest::Approx(0.55f));
    CHECK(s.rainAmount        == doctest::Approx(0.66f));
    CHECK(s.cloudMode         == 1);
    CHECK(s.cloudHeight       == doctest::Approx(321.0f));
    CHECK(s.cloudQuality      == 2);
    CHECK(s.lowResClouds);
    CHECK(s.cloudDensity      == doctest::Approx(1.7f));
    CHECK(s.cloudFluffiness   == doctest::Approx(0.77f));
    CHECK(s.cloudTint.g       == doctest::Approx(0.82f));
    CHECK(s.contrailAmount    == doctest::Approx(0.24f));
    CHECK(s.cirrusAmount      == doctest::Approx(0.34f));
    CHECK(s.cirrusSeed        == doctest::Approx(9.0f));
    CHECK(s.godRays           == doctest::Approx(0.45f));
    CHECK(s.shootingStars     == doctest::Approx(0.56f));
    CHECK(s.lensFlare         == doctest::Approx(0.67f));
    CHECK(s.starBrightness    == doctest::Approx(1.3f));
    CHECK(s.starColor.b       == doctest::Approx(0.93f));
    CHECK(s.starSize          == doctest::Approx(1.4f));
    CHECK(s.starSizeVariation == doctest::Approx(0.28f));
    CHECK(s.starGlow          == doctest::Approx(1.5f));
    CHECK(s.starTwinkle       == doctest::Approx(0.38f));
    CHECK(s.starDensity       == doctest::Approx(0.48f));
}

TEST_CASE("makeEnvironmentSettings advances the day-night cycle by exactly dt, once per call")
{
    EnvironmentComponent env;
    env.dayNightCycle = true;
    env.autoAdvance   = true;
    env.cycleSeconds  = 100.0f;   // 1 s of real time = 1/100 of a day
    env.timeOfDay     = 0.25f;
    env.moonPhaseAuto = false;    // isolated in the next case

    const IRenderer::EnvironmentSettings s1 = HE::makeEnvironmentSettings(env, 1.0f);
    CHECK(env.timeOfDay  == doctest::Approx(0.26f)); // advanced by dt / cycleSeconds
    CHECK(s1.timeOfDay   == doctest::Approx(0.26f)); // and the pushed value is post-advance

    // It is a TICK, not a getter: a second call in the same frame would advance the
    // world's clock twice. This is why each app calls it exactly once per frame.
    const IRenderer::EnvironmentSettings s2 = HE::makeEnvironmentSettings(env, 1.0f);
    CHECK(env.timeOfDay  == doctest::Approx(0.27f));
    CHECK(s2.timeOfDay   == doctest::Approx(0.27f));
}

TEST_CASE("makeEnvironmentSettings does not advance without the cycle, autoAdvance or dt")
{
    EnvironmentComponent base;
    base.cycleSeconds = 100.0f;
    base.timeOfDay    = 0.25f;
    base.moonPhase    = 0.5f;

    SUBCASE("day-night cycle off")
    {
        EnvironmentComponent env = base; env.dayNightCycle = false; env.autoAdvance = true;
        HE::makeEnvironmentSettings(env, 1.0f);
        CHECK(env.timeOfDay == doctest::Approx(0.25f));
    }
    SUBCASE("autoAdvance off (the editor scrubs time by hand)")
    {
        EnvironmentComponent env = base; env.dayNightCycle = true; env.autoAdvance = false;
        HE::makeEnvironmentSettings(env, 1.0f);
        CHECK(env.timeOfDay == doctest::Approx(0.25f));
    }
    SUBCASE("dt == 0 (the headless dump path pushes without advancing)")
    {
        EnvironmentComponent env = base; env.dayNightCycle = true; env.autoAdvance = true;
        HE::makeEnvironmentSettings(env, 0.0f);
        CHECK(env.timeOfDay == doctest::Approx(0.25f));
        CHECK(env.moonPhase == doctest::Approx(0.5f));
    }
}

TEST_CASE("makeEnvironmentSettings wraps timeOfDay and advances the lunar phase")
{
    EnvironmentComponent env;
    env.dayNightCycle = true;
    env.autoAdvance   = true;
    env.cycleSeconds  = 10.0f;
    env.timeOfDay     = 0.95f;
    env.moonPhaseAuto = true;
    env.moonCycleDays = 2.0f;     // half a lunar cycle per day
    env.moonPhase     = 0.0f;

    // dt = 1 s → +0.1 day: timeOfDay wraps past 1.0 back into [0,1).
    const IRenderer::EnvironmentSettings s = HE::makeEnvironmentSettings(env, 1.0f);
    CHECK(env.timeOfDay == doctest::Approx(0.05f));
    CHECK(s.timeOfDay   == doctest::Approx(0.05f));
    // moonPhase advances dayFrac / moonCycleDays = 0.1 / 2.
    CHECK(env.moonPhase == doctest::Approx(0.05f));
    CHECK(s.moonPhase   == doctest::Approx(0.05f));

    SUBCASE("moonPhaseAuto off freezes the phase while time keeps flowing")
    {
        env.moonPhaseAuto = false;
        const float phase = env.moonPhase;
        HE::makeEnvironmentSettings(env, 1.0f);
        CHECK(env.moonPhase == doctest::Approx(phase));
        CHECK(env.timeOfDay == doctest::Approx(0.15f));
    }
}

TEST_CASE("makeEnvironmentSettings carries the WeatherSystem's outputs to the renderer")
{
    // The whole reason the game runtime pushes EnvironmentSettings at all: without
    // it the weather sky / clouds / fog / flash would not show in-game.
    HorizonWorld world;
    WeatherComponent& w = setupWeatherWorld(world);
    w.currentKind = w.targetKind = WeatherKind::Storm;
    WeatherSystem::update(world, 0.016f);

    auto* env = world.registry().try_get<EnvironmentComponent>(world.rootEntity());
    REQUIRE(env != nullptr);
    const IRenderer::EnvironmentSettings s = HE::makeEnvironmentSettings(*env, 0.016f);

    const WeatherPreset p = weatherPreset(WeatherKind::Storm);
    CHECK(s.cloudCoverage == doctest::Approx(p.cloudCoverage));
    CHECK(s.fogDensity    == doctest::Approx(env->fogDensity));
    CHECK(s.rainAmount    == doctest::Approx(env->rainAmount));
    CHECK(s.wetness       == doctest::Approx(env->wetness));
    CHECK(s.flash         == doctest::Approx(env->flash));
    CHECK(s.windDirection == doctest::Approx(env->windDirection));
}
