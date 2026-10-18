#include "doctest.h"
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/WeatherAudio.h>
#include <HorizonScene/WeatherSystem.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <HorizonScene/Components/WeatherComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

// The sound of the weather, without an audio device: the curves are plain float
// functions, and the voices run on an AudioEngine in noDevice mode.

using namespace WeatherAudio;

namespace
{
    // A looping-friendly one second of mono PCM16 silence, registered under `id`
    // (a null id gets a fresh one). What is under test is which voice plays and how
    // loud, not what it sounds like.
    HE::UUID addClip(ContentManager& content, HE::UUID id = {})
    {
        AudioAsset a;
        a.id         = id;
        a.name       = "wx_test";
        a.sampleRate = 22050;
        a.channels   = 1;
        a.audioData  = std::vector<uint8_t>(22050 * 2, 0);
        return content.registerAudio(std::move(a));
    }

    // A world with a Sky and a Weather entity, the weather settled on `kind`.
    struct Rig
    {
        HorizonWorld     world;
        ContentManager   content;
        AudioEngine      engine;
        State            state { 1234u };
        WeatherComponent* weather = nullptr;
        EnvironmentComponent* sky = nullptr;

        explicit Rig(WeatherKind kind = WeatherKind::Clear, float intensity = 1.0f)
        {
            REQUIRE(engine.init(true));
            const Entity skyE = world.addSky();
            sky = &world.registry().get<EnvironmentComponent>(skyE);
            const Entity wxE = world.addWeather();
            weather = &world.registry().get<WeatherComponent>(wxE);
            weather->currentKind = weather->targetKind = kind;
            weather->intensity   = intensity;
            tickWeather(0.016f);
        }
        ~Rig() { stop(state, engine); engine.shutdown(); }

        void tickWeather(float dt) { WeatherSystem::update(world, dt); }

        // Advance weather and audio together for `seconds`.
        void run(float seconds, float step = 0.05f, bool audible = true)
        {
            for (float t = 0.0f; t < seconds - 1e-4f; t += step)
            {
                tickWeather(audible ? step : 0.0f);
                Frame f; f.realDt = step; f.gameDt = audible ? step : 0.0f; f.audible = audible;
                update(state, world, engine, content, f);
            }
        }
        void audio(float realDt, float gameDt, bool audible = true)
        {
            Frame f; f.realDt = realDt; f.gameDt = gameDt; f.audible = audible;
            update(state, world, engine, content, f);
        }
        State::Bed& bed(Layer l) { return state.beds[static_cast<size_t>(l)]; }
    };

    LoopGains settled(WeatherKind kind, float intensity = 1.0f)
    {
        HorizonWorld world;
        const Entity skyE = world.addSky();
        const Entity wxE  = world.addWeather();
        auto& w = world.registry().get<WeatherComponent>(wxE);
        w.currentKind = w.targetKind = kind;
        w.intensity = intensity;
        WeatherSystem::update(world, 0.016f);
        return loopGains(w, &world.registry().get<EnvironmentComponent>(skyE));
    }
}

// ── Component ───────────────────────────────────────────────────────────────

TEST_CASE("WeatherComponent sound fields default to the engine's sounds, on, through SFX")
{
    WeatherComponent w;
    CHECK(w.soundEnabled);
    CHECK(w.soundVolume == doctest::Approx(1.0f));
    CHECK(w.soundBus == "SFX");
    CHECK(w.rainSound  == HE::UUID{});
    CHECK(w.windSound  == HE::UUID{});
    CHECK(w.snowSound  == HE::UUID{});
    CHECK(w.stormSound == HE::UUID{});
    CHECK(w.thunderSound == HE::UUID{});
    CHECK(w.strikeCount == 0u);
}

TEST_CASE("an empty slot is the EngineContent default, a set one overrides just that sound")
{
    WeatherComponent w;
    CHECK(soundFor(w, Layer::Rain)  == HE::kEngineWeatherRainSoundId);
    CHECK(soundFor(w, Layer::Wind)  == HE::kEngineWeatherWindSoundId);
    CHECK(soundFor(w, Layer::Snow)  == HE::kEngineWeatherSnowSoundId);
    CHECK(soundFor(w, Layer::Storm) == HE::kEngineWeatherStormSoundId);
    CHECK(thunderSoundFor(w) == HE::kEngineWeatherThunderSoundId);

    const HE::UUID mine = HE::UUID::generate();
    w.windSound = mine;
    CHECK(soundFor(w, Layer::Wind) == mine);
    CHECK(soundFor(w, Layer::Rain) == HE::kEngineWeatherRainSoundId);
    w.thunderSound = mine;
    CHECK(thunderSoundFor(w) == mine);

    // Five different clips: a table that mapped two layers to one file would play
    // the same loop twice.
    const WeatherComponent d;
    std::vector<HE::UUID> all;
    for (int i = 0; i < kLayerCount; ++i) all.push_back(soundFor(d, static_cast<Layer>(i)));
    all.push_back(thunderSoundFor(d));
    for (size_t i = 0; i < all.size(); ++i)
    {
        CHECK(all[i] != HE::UUID{});
        for (size_t j = i + 1; j < all.size(); ++j) CHECK(all[i] != all[j]);
    }
}

TEST_CASE("the sound fields survive a scene round trip, defaults included")
{
    HorizonWorld world;
    const Entity e = world.addWeather();
    auto& w = world.registry().get<WeatherComponent>(e);
    w.rainSound     = HE::UUID::generate();
    w.soundVolume   = 0.4f;
    w.soundBus      = "Ambience";
    // …and one left at its defaults.

    SceneSerializer serializer;
    std::vector<uint8_t> snapshot;
    REQUIRE(serializer.saveToMemory(world, snapshot));
    HorizonWorld world2;
    REQUIRE(serializer.loadFromMemory(world2, snapshot));

    const Entity e2 = world2.weatherEntity();
    REQUIRE((e2 != entt::null));
    const auto& w2 = world2.registry().get<WeatherComponent>(e2);
    CHECK(w2.rainSound == w.rainSound);
    CHECK(w2.windSound == HE::UUID{});
    CHECK(w2.soundEnabled);
    CHECK(w2.soundVolume == doctest::Approx(0.4f));
    CHECK(w2.soundBus == "Ambience");
}

TEST_CASE("only the slots a project set are listed as scene assets, the defaults load on demand")
{
    HorizonWorld world;
    const Entity e = world.addWeather();
    auto& w = world.registry().get<WeatherComponent>(e);
    CHECK(SceneSystems::collectAssetRefs(world).empty());

    w.rainSound    = HE::UUID::generate();
    w.thunderSound = HE::UUID::generate();
    const auto refs = SceneSystems::collectAssetRefs(world);
    REQUIRE(refs.size() == 2);
    CHECK(std::find(refs.begin(), refs.end(), w.rainSound)    != refs.end());
    CHECK(std::find(refs.begin(), refs.end(), w.thunderSound) != refs.end());
}

// ── Curves ──────────────────────────────────────────────────────────────────

TEST_CASE("loudness: silent at nothing, full at everything, never quieter for more")
{
    CHECK(loudness(0.0f) == doctest::Approx(0.0f));
    CHECK(loudness(1.0f) == doctest::Approx(1.0f));
    CHECK(loudness(-3.0f) == doctest::Approx(0.0f));
    CHECK(loudness(7.0f)  == doctest::Approx(1.0f));
    CHECK(loudness(0.5f)  == doctest::Approx(std::sqrt(0.5f)));
    // The last trace of rain is not audible…
    CHECK(loudness(0.01f) < 0.01f);
    // …and the curve only ever rises.
    float prev = 0.0f;
    for (int i = 0; i <= 200; ++i)
    {
        const float l = loudness(static_cast<float>(i) / 200.0f);
        CHECK(l >= prev - 1e-6f);
        prev = l;
    }
}

TEST_CASE("loudness: rain fading into snow keeps its power, an equal-power cross-fade")
{
    for (float s = 0.15f; s <= 0.85f; s += 0.05f)
    {
        const float rain = loudness(1.0f - s);
        const float snow = loudness(s);
        CHECK(rain * rain + snow * snow == doctest::Approx(1.0f).epsilon(0.01));
    }
}

TEST_CASE("wind and storm amounts: calm is silent, the storm preset is full")
{
    CHECK(windAmount(0.4f) == doctest::Approx(0.0f));   // Foggy
    CHECK(windAmount(0.8f) == doctest::Approx(0.0f));   // Clear
    CHECK(windAmount(1.0f) == doctest::Approx(0.0f));   // calm
    CHECK(windAmount(2.6f) == doctest::Approx(1.0f));   // Storm
    CHECK(windAmount(9.0f) == doctest::Approx(1.0f));
    CHECK(windAmount(1.6f) > windAmount(1.2f));

    CHECK(stormAmount(0.7f, 1.6f) == doctest::Approx(0.0f));   // the rain preset is not a storm
    CHECK(stormAmount(1.0f, 2.6f) == doctest::Approx(1.0f));   // the storm preset is
    CHECK(stormAmount(0.0f, 2.6f) == doctest::Approx(0.0f));   // wind without rain is just wind
}

TEST_CASE("wind gusts swell within their band and never stop")
{
    float lo = 10.0f, hi = -10.0f;
    for (int i = 0; i < 4000; ++i)
    {
        const float g = windGust(static_cast<float>(i) * 0.05f);
        lo = std::min(lo, g);
        hi = std::max(hi, g);
    }
    CHECK(lo >= 0.82f - 1e-4f);
    CHECK(hi <= 1.0f + 1e-4f);
    CHECK(hi - lo > 0.1f);   // it actually moves
}

TEST_CASE("slew: constant rate, no overshoot, standing still without time")
{
    CHECK(slew(0.0f, 1.0f, 0.6f, 1.2f, 2.0f) == doctest::Approx(0.5f));   // 0.6 s of a 1.2 s rise
    CHECK(slew(1.0f, 0.0f, 1.0f, 1.2f, 2.0f) == doctest::Approx(0.5f));   // 1 s of a 2 s fall
    CHECK(slew(0.0f, 0.3f, 10.0f, 1.2f, 2.0f) == doctest::Approx(0.3f));  // never past the target
    CHECK(slew(0.9f, 0.2f, 10.0f, 1.2f, 2.0f) == doctest::Approx(0.2f));
    CHECK(slew(0.4f, 1.0f, 0.0f, 1.2f, 2.0f) == doctest::Approx(0.4f));
    CHECK(slew(0.4f, 1.0f, -1.0f, 1.2f, 2.0f) == doctest::Approx(0.4f));
    CHECK(slew(0.4f, 0.4f, 1.0f, 1.2f, 2.0f) == doctest::Approx(0.4f));
}

// ── The gains the weather asks for ──────────────────────────────────────────

TEST_CASE("clear weather is quiet, every other kind sounds like itself")
{
    const LoopGains clear = settled(WeatherKind::Clear);
    CHECK(clear.rain  == doctest::Approx(0.0f));
    CHECK(clear.snow  == doctest::Approx(0.0f));
    CHECK(clear.wind  == doctest::Approx(0.0f));
    CHECK(clear.storm == doctest::Approx(0.0f));

    const LoopGains foggy = settled(WeatherKind::Foggy);
    CHECK(foggy.wind == doctest::Approx(0.0f));

    const LoopGains cloudy = settled(WeatherKind::Cloudy);
    CHECK(cloudy.wind > 0.0f);
    CHECK(cloudy.rain == doctest::Approx(0.0f));

    const LoopGains rain = settled(WeatherKind::Rain);
    CHECK(rain.rain > 0.3f);
    CHECK(rain.wind > 0.0f);
    CHECK(rain.snow  == doctest::Approx(0.0f));
    CHECK(rain.storm == doctest::Approx(0.0f));

    const LoopGains snow = settled(WeatherKind::Snow);
    CHECK(snow.snow > 0.3f);
    CHECK(snow.rain  == doctest::Approx(0.0f));
    CHECK(snow.storm == doctest::Approx(0.0f));

    const LoopGains storm = settled(WeatherKind::Storm);
    CHECK(storm.rain  > rain.rain);
    CHECK(storm.wind  > rain.wind);
    CHECK(storm.storm > 0.5f);
    CHECK(storm.snow  == doctest::Approx(0.0f));
}

TEST_CASE("every bed stays under the headroom, so all of them together do not clip")
{
    for (int k = 0; k < static_cast<int>(WeatherKind::Count); ++k)
    {
        const LoopGains g = settled(static_cast<WeatherKind>(k));
        for (int i = 0; i < kLayerCount; ++i)
        {
            CHECK(g.of(static_cast<Layer>(i)) <= kHeadroom + 1e-4f);
            CHECK(g.of(static_cast<Layer>(i)) >= 0.0f);
        }
    }
}

TEST_CASE("the weather's intensity makes it louder")
{
    float prev = -1.0f;
    for (float intensity : { 0.2f, 0.4f, 0.6f, 0.8f, 1.0f })
    {
        const float r = settled(WeatherKind::Rain, intensity).rain;
        CHECK(r > prev);
        prev = r;
    }
    CHECK(settled(WeatherKind::Storm, 0.3f).storm < settled(WeatherKind::Storm, 1.0f).storm);
}

TEST_CASE("volume scales every bed, and the enabled switch silences them")
{
    HorizonWorld world;
    const Entity skyE = world.addSky();
    const Entity wxE  = world.addWeather();
    auto& w = world.registry().get<WeatherComponent>(wxE);
    w.currentKind = w.targetKind = WeatherKind::Storm;
    WeatherSystem::update(world, 0.016f);
    const auto* env = &world.registry().get<EnvironmentComponent>(skyE);

    const LoopGains full = loopGains(w, env);
    w.soundVolume = 0.5f;
    const LoopGains half = loopGains(w, env);
    CHECK(half.rain == doctest::Approx(full.rain * 0.5f));
    CHECK(half.wind == doctest::Approx(full.wind * 0.5f));
    w.soundVolume = 7.0f;   // past 1 is 1, not a boost that clips
    CHECK(loopGains(w, env).rain == doctest::Approx(full.rain));

    w.soundVolume  = 1.0f;
    w.soundEnabled = false;
    const LoopGains off = loopGains(w, env);
    CHECK(off.rain == doctest::Approx(0.0f));
    CHECK(off.wind == doctest::Approx(0.0f));
    CHECK(off.storm == doctest::Approx(0.0f));
}

TEST_CASE("a slider the user moved is what is heard: the sky's rain amount drives the bed")
{
    HorizonWorld world;
    const Entity skyE = world.addSky();
    const Entity wxE  = world.addWeather();
    auto& w = world.registry().get<WeatherComponent>(wxE);
    auto& env = world.registry().get<EnvironmentComponent>(skyE);
    w.currentKind = w.targetKind = WeatherKind::Clear;
    WeatherSystem::update(world, 0.016f);
    CHECK(loopGains(w, &env).rain == doctest::Approx(0.0f));

    env.rainAmount = 0.6f;   // the user dragged the Sky panel's rain
    CHECK(loopGains(w, &env).rain > 0.3f);
}

TEST_CASE("changing the weather cross-fades: the old bed falls while the new one rises")
{
    HorizonWorld world;
    const Entity skyE = world.addSky();
    const Entity wxE  = world.addWeather();
    auto& w = world.registry().get<WeatherComponent>(wxE);
    auto& env = world.registry().get<EnvironmentComponent>(skyE);
    w.currentKind = w.targetKind = WeatherKind::Rain;
    w.transitionDuration = 10.0f;
    WeatherSystem::update(world, 0.016f);
    const float rainStart = loopGains(w, &env).rain;
    REQUIRE(rainStart > 0.3f);

    w.targetKind = WeatherKind::Snow;
    float prevRain = rainStart, prevSnow = 0.0f;
    bool overlapped = false;
    for (int i = 0; i < 100; ++i)
    {
        WeatherSystem::update(world, 0.1f);
        const LoopGains g = loopGains(w, &env);
        CHECK(g.rain <= prevRain + 1e-4f);   // only falls
        CHECK(g.snow >= prevSnow - 1e-4f);   // only rises
        if (g.rain > 0.05f && g.snow > 0.05f) overlapped = true;
        prevRain = g.rain; prevSnow = g.snow;
    }
    CHECK(overlapped);                          // for a while both are heard
    CHECK(prevRain == doctest::Approx(0.0f));   // and it ends on snow alone
    CHECK(prevSnow > 0.3f);
}

// ── Thunder ─────────────────────────────────────────────────────────────────

TEST_CASE("thunder arrives at the speed of sound and quietens with distance")
{
    CHECK(thunderDelay(343.0f) == doctest::Approx(1.0f));
    CHECK(thunderDelay(0.0f)   == doctest::Approx(0.0f));
    CHECK(thunderDelay(-5.0f)  == doctest::Approx(0.0f));

    CHECK(thunderLevel(0.0f)   == doctest::Approx(1.0f));
    CHECK(thunderLevel(kThunderReferenceDist) == doctest::Approx(1.0f));
    CHECK(thunderLevel(kThunderMaxDistance) == doctest::Approx(kThunderReferenceDist / kThunderMaxDistance));
    CHECK(thunderLevel(1.0e6f) == doctest::Approx(kThunderFloorLevel));
    float prev = 2.0f;
    for (float d = 0.0f; d <= 5000.0f; d += 50.0f)
    {
        const float l = thunderLevel(d);
        CHECK(l <= prev + 1e-6f);
        CHECK(l >= kThunderFloorLevel - 1e-6f);
        prev = l;
    }
}

TEST_CASE("a strike lands at a random distance within its band, repeatably per seed")
{
    std::mt19937 a(99u), b(99u);
    float minD = 1.0e9f, maxD = 0.0f;
    for (int i = 0; i < 300; ++i)
    {
        const Strike s = makeStrike(a);
        CHECK(s.distance >= kThunderMinDistance);
        CHECK(s.distance <= kThunderMaxDistance);
        CHECK(s.delay == doctest::Approx(thunderDelay(s.distance)));
        CHECK(s.level == doctest::Approx(thunderLevel(s.distance)));
        minD = std::min(minD, s.distance);
        maxD = std::max(maxD, s.distance);
        CHECK(makeStrike(b).distance == doctest::Approx(s.distance));   // same seed, same sky
    }
    CHECK(maxD - minD > 1000.0f);   // and it really varies
}

// ── Voices, on an engine without a device ───────────────────────────────────

TEST_CASE("a bed starts quietly, follows the weather, and is gone when the weather is")
{
    Rig rig(WeatherKind::Rain);
    addClip(rig.content, HE::kEngineWeatherRainSoundId);
    addClip(rig.content, HE::kEngineWeatherWindSoundId);

    rig.run(0.3f);
    State::Bed& rain = rig.bed(Layer::Rain);
    REQUIRE(rain.handle != 0);
    CHECK(rig.engine.isPlaying(rain.handle));
    CHECK(rain.asset == HE::kEngineWeatherRainSoundId);
    // Fading in, not jumping to full.
    CHECK(rain.gain > 0.0f);
    CHECK(rain.gain < settled(WeatherKind::Rain).rain);
    CHECK(rig.engine.getSoundVolume(rain.handle) == doctest::Approx(rain.gain).epsilon(0.01));

    // Given time it reaches what the weather asks for.
    rig.run(3.0f);
    CHECK(rain.gain == doctest::Approx(settled(WeatherKind::Rain).rain).epsilon(0.02));
    CHECK(rig.engine.getSoundVolume(rain.handle) == doctest::Approx(rain.gain).epsilon(0.01));
    // The snow bed was never asked for, so it has no voice.
    CHECK(rig.bed(Layer::Snow).handle == 0);

    // The weather clears: the bed fades out and the voice goes away.
    const uint64_t old = rain.handle;
    rig.weather->targetKind = WeatherKind::Clear;
    rig.weather->transitionDuration = 2.0f;
    rig.run(8.0f);
    CHECK(rain.handle == 0);
    CHECK(rain.gain == doctest::Approx(0.0f));
    CHECK_FALSE(rig.engine.isPlaying(old));
}

TEST_CASE("an unassigned bed plays the engine's clip, an assigned one replaces it")
{
    Rig rig(WeatherKind::Rain);
    addClip(rig.content, HE::kEngineWeatherRainSoundId);
    const HE::UUID mine = addClip(rig.content);

    rig.run(0.2f);
    REQUIRE(rig.bed(Layer::Rain).handle != 0);
    CHECK(rig.bed(Layer::Rain).asset == HE::kEngineWeatherRainSoundId);
    const uint64_t first = rig.bed(Layer::Rain).handle;

    rig.weather->rainSound = mine;
    rig.run(0.2f);
    REQUIRE(rig.bed(Layer::Rain).handle != 0);
    CHECK(rig.bed(Layer::Rain).asset == mine);
    CHECK(rig.bed(Layer::Rain).handle != first);
    CHECK_FALSE(rig.engine.isPlaying(first));   // the old voice is gone, not left running

    // Emptying the slot goes back to the engine's.
    rig.weather->rainSound = HE::UUID{};
    rig.run(0.2f);
    CHECK(rig.bed(Layer::Rain).asset == HE::kEngineWeatherRainSoundId);
}

TEST_CASE("a clip that cannot be found leaves that bed silent and breaks nothing")
{
    Rig rig(WeatherKind::Storm);
    rig.run(3.0f);   // no clips registered at all
    for (int i = 0; i < kLayerCount; ++i) CHECK(rig.state.beds[static_cast<size_t>(i)].handle == 0);

    // It is tried again, so a clip that shows up later (a download finishing) plays.
    addClip(rig.content, HE::kEngineWeatherRainSoundId);
    rig.run(2.0f);
    CHECK(rig.bed(Layer::Rain).handle != 0);
}

TEST_CASE("pause fades the weather out and silences it, resume brings it back")
{
    Rig rig(WeatherKind::Rain);
    addClip(rig.content, HE::kEngineWeatherRainSoundId);
    rig.run(4.0f);
    REQUIRE(rig.bed(Layer::Rain).handle != 0);
    const float before = rig.bed(Layer::Rain).gain;
    REQUIRE(before > 0.3f);

    // Wall-clock time passes while the game does not.
    rig.run(0.15f, 0.05f, /*audible=*/false);
    CHECK(rig.bed(Layer::Rain).gain < before);          // on its way down
    CHECK(rig.bed(Layer::Rain).gain > 0.0f);            // not cut
    rig.run(0.5f, 0.05f, /*audible=*/false);
    CHECK(rig.bed(Layer::Rain).handle == 0);            // and then no rain loop runs
    CHECK(rig.bed(Layer::Rain).gain == doctest::Approx(0.0f));

    rig.run(4.0f);
    CHECK(rig.bed(Layer::Rain).handle != 0);
    CHECK(rig.bed(Layer::Rain).gain == doctest::Approx(before).epsilon(0.05));
}

TEST_CASE("the enabled switch on the component silences the weather like a pause does")
{
    Rig rig(WeatherKind::Rain);
    addClip(rig.content, HE::kEngineWeatherRainSoundId);
    rig.run(3.0f);
    REQUIRE(rig.bed(Layer::Rain).handle != 0);

    rig.weather->soundEnabled = false;
    rig.run(1.0f);
    CHECK(rig.bed(Layer::Rain).handle == 0);
}

TEST_CASE("voices something else stopped are started again")
{
    Rig rig(WeatherKind::Rain);
    addClip(rig.content, HE::kEngineWeatherRainSoundId);
    rig.run(2.0f);
    const uint64_t first = rig.bed(Layer::Rain).handle;
    REQUIRE(first != 0);

    rig.engine.stopAll();   // play stopped, a scene change, a removed bus …
    rig.run(0.2f);
    CHECK(rig.bed(Layer::Rain).handle != 0);
    CHECK(rig.bed(Layer::Rain).handle != first);
    CHECK(rig.engine.isPlaying(rig.bed(Layer::Rain).handle));
}

TEST_CASE("without a WeatherComponent nothing plays, and nothing breaks on an engine without a device")
{
    Rig rig(WeatherKind::Rain);
    addClip(rig.content, HE::kEngineWeatherRainSoundId);
    rig.run(2.0f);
    const uint64_t h = rig.bed(Layer::Rain).handle;
    REQUIRE(h != 0);

    rig.world.removeWeather();
    rig.audio(0.05f, 0.05f);
    CHECK(rig.bed(Layer::Rain).handle == 0);
    CHECK_FALSE(rig.engine.isPlaying(h));

    // An engine that never started is a no-op, not a crash.
    AudioEngine idle;
    State s(1u);
    Frame f; f.realDt = 0.1f; f.gameDt = 0.1f;
    update(s, rig.world, idle, rig.content, f);
    CHECK(s.beds[0].handle == 0);
}

TEST_CASE("stop() ends every voice and forgets the ramps")
{
    Rig rig(WeatherKind::Storm);
    addClip(rig.content, HE::kEngineWeatherRainSoundId);
    addClip(rig.content, HE::kEngineWeatherWindSoundId);
    addClip(rig.content, HE::kEngineWeatherStormSoundId);
    addClip(rig.content, HE::kEngineWeatherThunderSoundId);
    rig.run(3.0f);

    std::vector<uint64_t> handles;
    for (auto& b : rig.state.beds) if (b.handle) handles.push_back(b.handle);
    REQUIRE(handles.size() >= 3);

    stop(rig.state, rig.engine);
    for (uint64_t h : handles) CHECK_FALSE(rig.engine.isPlaying(h));
    for (auto& b : rig.state.beds) { CHECK(b.handle == 0); CHECK(b.gain == doctest::Approx(0.0f)); }
    CHECK(rig.state.pending.empty());
    CHECK(rig.state.thunder.empty());
}

TEST_CASE("each lightning strike is one thunder roll, a distance away")
{
    Rig rig(WeatherKind::Storm);
    addClip(rig.content, HE::kEngineWeatherThunderSoundId);
    rig.audio(0.05f, 0.05f);   // first sight of the component

    rig.weather->strikeCount += 1;
    rig.audio(0.05f, 0.05f);
    REQUIRE(rig.state.pending.size() == 1);
    const float wait = rig.state.pending[0].delay;
    CHECK(wait >= thunderDelay(kThunderMinDistance) - 0.06f);
    CHECK(wait <= thunderDelay(kThunderMaxDistance) + 0.01f);
    CHECK(rig.state.thunder.empty());   // not yet: the sound is still on its way

    // Nothing new happens without a new strike.
    rig.audio(0.05f, 0.05f);
    CHECK(rig.state.pending.size() == 1);

    // Let the sound arrive.
    float t = 0.0f;
    while (!rig.state.pending.empty() && t < 12.0f) { rig.audio(0.05f, 0.05f); t += 0.05f; }
    CHECK(rig.state.pending.empty());
    REQUIRE(rig.state.thunder.size() == 1);
    const uint64_t h = rig.state.thunder[0];
    CHECK(rig.engine.isPlaying(h));
    const float v = rig.engine.getSoundVolume(h);
    CHECK(v >= kThunderFloorLevel - 1e-4f);
    CHECK(v <= 1.0f + 1e-4f);
    CHECK(t == doctest::Approx(wait).epsilon(0.1));
}

// MUTATION: in WeatherAudio::update's thunder prune, drop the `engine.stop(h)` call
// (the handle is forgotten but the finished voice stays in the engine) — the voice
// count below stays at 1 and the last CHECK fails.
TEST_CASE("a roll that has run out is reaped, not left in the engine holding its samples")
{
    Rig rig(WeatherKind::Storm);
    addClip(rig.content, HE::kEngineWeatherThunderSoundId);   // one second long
    rig.audio(0.05f, 0.05f);
    rig.weather->strikeCount += 1;
    for (int i = 0; i < 400 && rig.state.thunder.empty(); ++i) rig.audio(0.05f, 0.05f);
    REQUIRE(rig.state.thunder.size() == 1);
    CHECK(rig.engine.busVoiceCount("") == 1);

    // Let the mixer play the clip to its end, as a device would.
    const int channels = rig.engine.outputChannels();
    REQUIRE(channels > 0);
    std::vector<float> out(static_cast<size_t>(channels) * 48000u * 3u);
    rig.engine.readMixedFrames(out.data(), 48000u * 3u);
    CHECK_FALSE(rig.engine.isPlaying(rig.state.thunder[0]));

    rig.audio(0.05f, 0.05f);
    CHECK(rig.state.thunder.empty());
    CHECK(rig.engine.busVoiceCount("") == 0);
}

TEST_CASE("thunder follows the component's volume")
{
    Rig rig(WeatherKind::Storm);
    addClip(rig.content, HE::kEngineWeatherThunderSoundId);
    rig.weather->soundVolume = 0.0f;
    rig.audio(0.05f, 0.05f);
    rig.weather->strikeCount += 1;
    for (int i = 0; i < 300; ++i) rig.audio(0.05f, 0.05f);
    // Volume zero: the roll is requested but inaudible.
    for (uint64_t h : rig.state.thunder) CHECK(rig.engine.getSoundVolume(h) == doctest::Approx(0.0f));
}

TEST_CASE("a loaded scene does not replay old strikes, a strike during a pause is not heard late")
{
    Rig rig(WeatherKind::Storm);
    addClip(rig.content, HE::kEngineWeatherThunderSoundId);

    rig.weather->strikeCount = 40;   // a component that already has history
    rig.audio(0.05f, 0.05f);
    CHECK(rig.state.pending.empty());

    rig.weather->strikeCount = 41;   // a strike while the game is paused
    rig.audio(0.05f, 0.0f, /*audible=*/false);
    CHECK(rig.state.pending.empty());
    for (int i = 0; i < 200; ++i) rig.audio(0.05f, 0.05f);
    CHECK(rig.state.thunder.empty());

    rig.weather->strikeCount = 3;    // the scene was reloaded: the counter went back
    rig.audio(0.05f, 0.05f);
    CHECK(rig.state.pending.empty());
    rig.weather->strikeCount = 4;
    rig.audio(0.05f, 0.05f);
    CHECK(rig.state.pending.size() == 1);
}

TEST_CASE("a storm of strikes is bounded: few queued, few rolls at once")
{
    Rig rig(WeatherKind::Storm);
    addClip(rig.content, HE::kEngineWeatherThunderSoundId);
    rig.audio(0.05f, 0.05f);

    rig.weather->strikeCount += 50;
    rig.audio(0.05f, 0.05f);
    CHECK(static_cast<int>(rig.state.pending.size()) <= kMaxPendingStrikes);

    size_t mostAtOnce = 0;
    for (int i = 0; i < 400; ++i)
    {
        rig.audio(0.05f, 0.05f);
        mostAtOnce = std::max(mostAtOnce, rig.state.thunder.size());
    }
    CHECK(mostAtOnce >= 1);
    CHECK(static_cast<int>(mostAtOnce) <= kMaxThunderVoices);
}

TEST_CASE("pausing stops the thunder that is ringing and drops the rolls on their way")
{
    Rig rig(WeatherKind::Storm);
    addClip(rig.content, HE::kEngineWeatherThunderSoundId);
    rig.audio(0.05f, 0.05f);
    rig.weather->strikeCount += 1;
    for (int i = 0; i < 400 && rig.state.thunder.empty(); ++i) rig.audio(0.05f, 0.05f);
    REQUIRE_FALSE(rig.state.thunder.empty());
    const uint64_t h = rig.state.thunder[0];

    rig.audio(0.05f, 0.0f, /*audible=*/false);
    CHECK(rig.state.thunder.empty());
    CHECK_FALSE(rig.engine.isPlaying(h));
}

TEST_CASE("end to end: a storm that builds up is heard without any code asking for it")
{
    Rig rig(WeatherKind::Clear);
    for (WeatherAudio::Layer l : { Layer::Rain, Layer::Wind, Layer::Snow, Layer::Storm })
        addClip(rig.content, soundFor(*rig.weather, l));
    addClip(rig.content, HE::kEngineWeatherThunderSoundId);

    rig.run(2.0f);
    for (auto& b : rig.state.beds) CHECK(b.handle == 0);   // a clear sky is quiet

    rig.weather->targetKind = WeatherKind::Storm;
    rig.weather->transitionDuration = 6.0f;
    bool rainBeforeStorm = false;
    bool thunderHeard = false;
    for (int i = 0; i < 40 * 20; ++i)   // forty seconds
    {
        rig.run(0.05f);
        if (rig.bed(Layer::Rain).handle != 0 && rig.bed(Layer::Storm).gain < 0.1f) rainBeforeStorm = true;
        if (!rig.state.thunder.empty()) thunderHeard = true;
    }
    CHECK(rainBeforeStorm);   // the rain comes in first, the storm bed behind it
    CHECK(rig.bed(Layer::Rain).handle  != 0);
    CHECK(rig.bed(Layer::Wind).handle  != 0);
    CHECK(rig.bed(Layer::Storm).handle != 0);
    CHECK(rig.bed(Layer::Snow).handle  == 0);
    CHECK(rig.weather->strikeCount > 0u);
    CHECK(thunderHeard);

    // And back to a clear sky: it all fades away again.
    rig.weather->targetKind = WeatherKind::Clear;
    rig.run(30.0f);
    for (auto& b : rig.state.beds) CHECK(b.handle == 0);
}
