#include "doctest.h"
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/AudioSystem.h>
#include <ContentManager/ContentManager.h>
#include <Types/UUID.h>
#include <algorithm>
#include <cstdlib>
#include <cmath>

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Generate N frames of silence as int16 PCM
static std::vector<uint8_t> makeSilence(int frames, int channels)
{
    return std::vector<uint8_t>(static_cast<size_t>(frames * channels) * 2, 0);
}

// ─── AudioSourceComponent ──────────────────────────────────────────────────────

TEST_CASE("AudioSourceComponent has sane defaults")
{
    AudioSourceComponent a;
    CHECK(a.volume == doctest::Approx(1.0f));
    CHECK(a.pitch  == doctest::Approx(1.0f));
    CHECK(a.range  == doctest::Approx(20.0f));
    CHECK(!a.loop);
    CHECK(!a.playOnStart);
    CHECK(!a.spatial);
    CHECK(a.assetId == HE::UUID{}); // default-constructed = null
}

TEST_CASE("AudioSourceComponent can be attached to an entity")
{
    HorizonWorld world;
    auto e = world.createEntity("Speaker");
    auto& reg = world.registry();

    HE::UUID assetId = HE::UUID::generate();
    AudioSourceComponent src;
    src.assetId     = assetId;
    src.volume      = 0.75f;
    src.loop        = true;
    src.playOnStart = true;
    src.spatial     = true;
    src.range       = 50.0f;
    reg.emplace<AudioSourceComponent>(e, src);

    const auto& stored = reg.get<AudioSourceComponent>(e);
    CHECK(stored.assetId     == assetId);
    CHECK(stored.volume      == doctest::Approx(0.75f));
    CHECK(stored.loop        == true);
    CHECK(stored.playOnStart == true);
    CHECK(stored.spatial     == true);
    CHECK(stored.range       == doctest::Approx(50.0f));
}

// ─── AudioListenerComponent ───────────────────────────────────────────────────

TEST_CASE("AudioListenerComponent has sane defaults")
{
    AudioListenerComponent l;
    CHECK(l.masterVolume == doctest::Approx(1.0f));
}

TEST_CASE("AudioListenerComponent can be attached to an entity")
{
    HorizonWorld world;
    auto e = world.createEntity("Player");
    auto& reg = world.registry();

    AudioListenerComponent l;
    l.masterVolume = 0.8f;
    reg.emplace<AudioListenerComponent>(e, l);

    CHECK(reg.get<AudioListenerComponent>(e).masterVolume == doctest::Approx(0.8f));
}

// ─── SceneSerializer round-trip ───────────────────────────────────────────────

TEST_CASE("AudioSourceComponent serializes and deserializes via memory snapshot")
{
    HorizonWorld world;
    auto e = world.createEntity("Speaker");
    auto& reg = world.registry();

    HE::UUID assetId = HE::UUID::generate();
    AudioSourceComponent src;
    src.assetId     = assetId;
    src.volume      = 0.6f;
    src.pitch       = 1.2f;
    src.range       = 30.0f;
    src.loop        = true;
    src.playOnStart = false;
    src.spatial     = true;
    reg.emplace<AudioSourceComponent>(e, src);

    // Round-trip through the binary memory snapshot (same path as play-in-editor)
    SceneSerializer serializer;
    std::vector<uint8_t> snapshot;
    REQUIRE(serializer.saveToMemory(world, snapshot));
    CHECK(!snapshot.empty());

    HorizonWorld world2;
    REQUIRE(serializer.loadFromMemory(world2, snapshot));

    bool found = false;
    for (auto [ent, name] : world2.registry().view<NameComponent>().each())
    {
        if (name.name != "Speaker") continue;
        found = true;
        const auto* a = world2.registry().try_get<AudioSourceComponent>(ent);
        REQUIRE(a != nullptr);
        CHECK(a->assetId     == assetId);
        CHECK(a->volume      == doctest::Approx(0.6f));
        CHECK(a->pitch       == doctest::Approx(1.2f));
        CHECK(a->range       == doctest::Approx(30.0f));
        CHECK(a->loop        == true);
        CHECK(a->playOnStart == false);
        CHECK(a->spatial     == true);
        break;
    }
    CHECK(found);
}

TEST_CASE("AudioListenerComponent serializes and deserializes via memory snapshot")
{
    HorizonWorld world;
    auto e = world.createEntity("MainCamera");
    auto& reg = world.registry();

    AudioListenerComponent l;
    l.masterVolume = 0.5f;
    reg.emplace<AudioListenerComponent>(e, l);

    SceneSerializer serializer;
    std::vector<uint8_t> snapshot;
    REQUIRE(serializer.saveToMemory(world, snapshot));

    HorizonWorld world2;
    REQUIRE(serializer.loadFromMemory(world2, snapshot));

    bool found = false;
    for (auto [ent, name] : world2.registry().view<NameComponent>().each())
    {
        if (name.name != "MainCamera") continue;
        found = true;
        const auto* lp = world2.registry().try_get<AudioListenerComponent>(ent);
        REQUIRE(lp != nullptr);
        CHECK(lp->masterVolume == doctest::Approx(0.5f));
        break;
    }
    CHECK(found);
}

TEST_CASE("AudioSourceComponent round-trip preserves null assetId")
{
    HorizonWorld world;
    auto e = world.createEntity("SilentSource");
    world.registry().emplace<AudioSourceComponent>(e); // all defaults, null UUID

    SceneSerializer serializer;
    std::vector<uint8_t> snapshot;
    REQUIRE(serializer.saveToMemory(world, snapshot));

    HorizonWorld world2;
    REQUIRE(serializer.loadFromMemory(world2, snapshot));

    for (auto [ent, name] : world2.registry().view<NameComponent>().each())
    {
        if (name.name != "SilentSource") continue;
        const auto* a = world2.registry().try_get<AudioSourceComponent>(ent);
        REQUIRE(a != nullptr);
        CHECK(a->assetId == HE::UUID{}); // should still be null
        break;
    }
}

// ─── AudioEngine (noDevice / headless) ───────────────────────────────────────

TEST_CASE("AudioEngine: init/shutdown in noDevice mode")
{
    AudioEngine engine;
    CHECK(engine.init(true));   // noDevice=true
    CHECK(engine.isInitialized());
    engine.shutdown();
    CHECK(!engine.isInitialized());
}

TEST_CASE("AudioEngine: double-init is safe")
{
    AudioEngine engine;
    CHECK(engine.init(true));
    CHECK(engine.init(true)); // second call is a no-op
    engine.shutdown();
}

TEST_CASE("AudioEngine: play silence returns valid handle")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(1024, 2);
    uint64_t h = engine.play(pcm, 48000, 2);
    CHECK(h != 0);
    engine.shutdown();
}

TEST_CASE("AudioEngine: play empty data returns 0")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    std::vector<uint8_t> empty;
    CHECK(engine.play(empty, 48000, 2) == 0);
    engine.shutdown();
}

TEST_CASE("AudioEngine: stop handle is safe")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(1024, 1);
    uint64_t h = engine.play(pcm, 44100, 1);
    REQUIRE(h != 0);
    engine.stop(h);
    engine.stop(h);   // double-stop is safe
    engine.stop(9999); // unknown handle is safe
    engine.shutdown();
}

TEST_CASE("AudioEngine: stopAll clears all sounds")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(512, 2);
    uint64_t h1 = engine.play(pcm, 48000, 2);
    uint64_t h2 = engine.play(pcm, 48000, 2);
    CHECK(h1 != 0);
    CHECK(h2 != 0);
    engine.stopAll();
    CHECK(!engine.isPlaying(h1));
    CHECK(!engine.isPlaying(h2));
    engine.shutdown();
}

TEST_CASE("AudioEngine: play with volume and pitch")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(2048, 2);
    uint64_t h = engine.play(pcm, 48000, 2, 0.5f, 1.5f, false);
    CHECK(h != 0);
    engine.shutdown();
}

// ─── AudioSystem::playOnStart ─────────────────────────────────────────────────

TEST_CASE("AudioSystem: playOnStart skips entities without flag")
{
    HorizonWorld world;
    AudioEngine  engine;
    REQUIRE(engine.init(true));

    auto e = world.createEntity("Speaker");
    AudioSourceComponent src;
    src.playOnStart = false;
    world.registry().emplace<AudioSourceComponent>(e, src);

    // No ContentManager and playOnStart=false — should not crash
    AudioSystem::playOnStart(world, engine, nullptr);
    engine.shutdown();
}

TEST_CASE("AudioSystem: playOnStart calls engine when asset is present")
{
    HorizonWorld    world;
    ContentManager  content;
    AudioEngine     engine;
    REQUIRE(engine.init(true));

    // Register an audio asset with silence PCM
    AudioAsset asset;
    asset.name       = "test_tone";
    asset.sampleRate = 44100;
    asset.channels   = 1;
    asset.audioData  = makeSilence(4410, 1); // 0.1 s mono
    HE::UUID assetId = content.registerAudio(std::move(asset));

    // Entity with playOnStart=true
    auto e = world.createEntity("SpeakerEntity");
    AudioSourceComponent src;
    src.assetId     = assetId;
    src.playOnStart = true;
    src.volume      = 0.8f;
    world.registry().emplace<AudioSourceComponent>(e, src);

    // Should not crash, engine.play() should succeed
    AudioSystem::playOnStart(world, engine, &content);
    engine.shutdown();
}

// ─── 4c.2 Spatialization ─────────────────────────────────────────────────────

TEST_CASE("AudioSourceComponent: new spatial fields have sane defaults")
{
    AudioSourceComponent a;
    CHECK(a.innerRange    == doctest::Approx(1.0f));
    CHECK(a.rolloffFactor == doctest::Approx(1.0f));
    CHECK(a.handle        == 0u);
}

TEST_CASE("AudioEngine: playSpatial returns valid handle")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(4410, 1);
    uint64_t h = engine.playSpatial(pcm, 44100, 1,
                                     1.0f, 1.0f, false,
                                     0.0f, 0.0f, 0.0f,
                                     1.0f, 20.0f);
    CHECK(h != 0);
    engine.shutdown();
}

TEST_CASE("AudioEngine: playSpatial empty data returns 0")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    uint64_t h = engine.playSpatial({}, 44100, 1, 1.0f, 1.0f, false, 0, 0, 0, 1, 20);
    CHECK(h == 0);
    engine.shutdown();
}

TEST_CASE("AudioEngine: setSoundPosition does not crash")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(4410, 1);
    uint64_t h = engine.playSpatial(pcm, 44100, 1, 1.0f, 1.0f, false, 0, 0, 0, 1, 20);
    REQUIRE(h != 0);

    CHECK_NOTHROW(engine.setSoundPosition(h, 5.0f, 0.0f, 3.0f));
    CHECK_NOTHROW(engine.setSoundPosition(99999, 1.0f, 0.0f, 0.0f)); // unknown handle
    engine.shutdown();
}

TEST_CASE("AudioEngine: setListenerTransform does not crash")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    // Default forward=-Z, up=+Y
    CHECK_NOTHROW(engine.setListenerTransform(
        1.0f, 2.0f, 3.0f,
        0.0f, 0.0f, -1.0f,
        0.0f, 1.0f,  0.0f));
    engine.shutdown();
}

TEST_CASE("AudioSystem: updateSpatial with no listener or sources does not crash")
{
    HorizonWorld world;
    AudioEngine  engine;
    REQUIRE(engine.init(true));
    CHECK_NOTHROW(AudioSystem::updateSpatial(world, engine));
    engine.shutdown();
}

TEST_CASE("AudioSystem: playOnStart plays spatial source and stores handle")
{
    HorizonWorld    world;
    ContentManager  content;
    AudioEngine     engine;
    REQUIRE(engine.init(true));

    AudioAsset asset;
    asset.name       = "boom";
    asset.sampleRate = 44100;
    asset.channels   = 1;
    asset.audioData  = makeSilence(4410, 1);
    HE::UUID assetId = content.registerAudio(std::move(asset));

    auto e = world.createEntity("SpatialSource");
    TransformComponent t; t.position = { 5.0f, 0.0f, 0.0f };
    world.registry().emplace<TransformComponent>(e, t);
    AudioSourceComponent src;
    src.assetId     = assetId;
    src.playOnStart = true;
    src.spatial     = true;
    src.range       = 30.0f;
    world.registry().emplace<AudioSourceComponent>(e, src);

    AudioSystem::playOnStart(world, engine, &content);

    // handle should have been written back into the component
    const auto& stored = world.registry().get<AudioSourceComponent>(e);
    CHECK(stored.handle != 0);
    engine.shutdown();
}

TEST_CASE("AudioSystem: updateSpatial updates listener and source positions")
{
    HorizonWorld world;
    AudioEngine  engine;
    REQUIRE(engine.init(true));

    // Listener entity
    auto listener = world.createEntity("Listener");
    TransformComponent lt; lt.position = { 0, 0, 0 }; lt.rotation = {};
    world.registry().emplace<TransformComponent>(listener, lt);
    world.registry().emplace<AudioListenerComponent>(listener, AudioListenerComponent{});

    // Spatial source (already playing — simulate by storing a handle)
    auto speaker = world.createEntity("Speaker");
    TransformComponent st; st.position = { 5, 0, 0 };
    world.registry().emplace<TransformComponent>(speaker, st);
    AudioSourceComponent src;
    src.spatial = true;
    src.handle  = engine.playSpatial(makeSilence(4410, 1), 44100, 1,
                                      1.0f, 1.0f, true, 5.0f, 0.0f, 0.0f, 1.0f, 30.0f);
    world.registry().emplace<AudioSourceComponent>(speaker, src);

    CHECK_NOTHROW(AudioSystem::updateSpatial(world, engine));
    engine.shutdown();
}

TEST_CASE("AudioSourceComponent: new fields round-trip through serializer")
{
    HorizonWorld src_world;
    auto e = src_world.createEntity("Speaker");
    AudioSourceComponent src;
    src.innerRange    = 2.5f;
    src.rolloffFactor = 3.0f;
    src.spatial       = true;
    src_world.registry().emplace<AudioSourceComponent>(e, src);

    // Save / load via memory snapshot
    const auto snapshotPath = std::filesystem::temp_directory_path() / "he_audio_spatial_test.hescene";
    SceneSerializer serializer;
    REQUIRE(serializer.save(src_world, snapshotPath, SerializeFormat::JSON));

    HorizonWorld dst_world;
    REQUIRE(serializer.load(dst_world, snapshotPath, SerializeFormat::JSON));

    auto view = dst_world.registry().view<AudioSourceComponent>();
    REQUIRE(!view.empty());
    const auto& loaded = dst_world.registry().get<AudioSourceComponent>(*view.begin());
    CHECK(loaded.innerRange    == doctest::Approx(2.5f));
    CHECK(loaded.rolloffFactor == doctest::Approx(3.0f));
    CHECK(loaded.spatial       == true);
    CHECK(loaded.handle        == 0u); // runtime field, not serialized
}

// ─── 4c.3 Mixer/Bus ──────────────────────────────────────────────────────────

TEST_CASE("AudioEngine: createBus returns true and bus exists")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    CHECK(engine.createBus("SFX", 0.8f));
    CHECK(engine.hasBus("SFX"));
    CHECK_FALSE(engine.hasBus("Music")); // not created
    engine.shutdown();
}

TEST_CASE("AudioEngine: createBus is idempotent")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    CHECK(engine.createBus("SFX"));
    CHECK(engine.createBus("SFX")); // second call should not crash
    engine.shutdown();
}

TEST_CASE("AudioEngine: getBusVolume returns set value")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    engine.createBus("Music", 0.5f);
    CHECK(engine.getBusVolume("Music") == doctest::Approx(0.5f).epsilon(0.01f));
    engine.shutdown();
}

TEST_CASE("AudioEngine: setBusVolume changes volume")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    engine.createBus("SFX", 1.0f);
    engine.setBusVolume("SFX", 0.25f);
    CHECK(engine.getBusVolume("SFX") == doctest::Approx(0.25f).epsilon(0.01f));
    engine.shutdown();
}

TEST_CASE("AudioEngine: getBusVolume returns 1.0 for unknown bus")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));
    CHECK(engine.getBusVolume("NonExistent") == doctest::Approx(1.0f));
    engine.shutdown();
}

TEST_CASE("AudioEngine: play through named bus routes correctly")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    engine.createBus("SFX", 1.0f);
    auto pcm = makeSilence(4410, 1);
    uint64_t h = engine.play(pcm, 44100, 1, 1.0f, 1.0f, false, "SFX");
    CHECK(h != 0);
    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: play through non-existent bus still plays on master")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(4410, 1);
    uint64_t h = engine.play(pcm, 44100, 1, 1.0f, 1.0f, false, "NonExistentBus");
    CHECK(h != 0); // should fall back to master, not fail
    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: bus volume 0 mutes sounds on that bus")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    engine.createBus("Quiet", 0.0f);
    CHECK(engine.getBusVolume("Quiet") == doctest::Approx(0.0f));
    engine.shutdown();
}

TEST_CASE("AudioSourceComponent: busName field defaults to empty")
{
    AudioSourceComponent a;
    CHECK(a.busName.empty());
}

// ─── Sample-rate handling ────────────────────────────────────────────────────
// Regression: play()/playSpatial() validated the sampleRate argument but never
// wrote it into the ma_audio_buffer_config, which leaves miniaudio's default of 0.
// A zero rate makes the sound inherit the engine rate (48 kHz here) and skip the
// resampler, so 44.1 kHz assets — what the WAV importer produces verbatim, it does
// not resample — played back ~9% too fast and a pitch too high.

TEST_CASE("AudioEngine: play keeps the clip's sample rate instead of the engine's")
{
    AudioEngine engine;
    REQUIRE(engine.init(true)); // noDevice mode runs the engine at 48000 Hz

    auto pcm = makeSilence(4410, 1);
    uint64_t h = engine.play(pcm, 44100, 1);
    REQUIRE(h != 0);
    CHECK(engine.getSoundSampleRate(h) == 44100); // was 0 (-> silently 48000) before the fix

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: playSpatial keeps the clip's sample rate")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(2205, 2);
    uint64_t h = engine.playSpatial(pcm, 22050, 2,
                                     1.0f, 1.0f, false,
                                     0.0f, 0.0f, 0.0f,
                                     1.0f, 20.0f);
    REQUIRE(h != 0);
    CHECK(engine.getSoundSampleRate(h) == 22050);

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: sample rate matching the engine still reports correctly")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(4800, 2);
    uint64_t h = engine.play(pcm, 48000, 2);
    REQUIRE(h != 0);
    CHECK(engine.getSoundSampleRate(h) == 48000);

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: getSoundSampleRate returns 0 for unknown handle")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));
    CHECK(engine.getSoundSampleRate(0)     == 0);
    CHECK(engine.getSoundSampleRate(99999) == 0);
    engine.shutdown();
}

TEST_CASE("AudioSystem: playOnStart forwards the asset's sample rate to the engine")
{
    HorizonWorld    world;
    ContentManager  content;
    AudioEngine     engine;
    REQUIRE(engine.init(true));

    AudioAsset asset;
    asset.name       = "cd_quality";
    asset.sampleRate = 44100; // what AudioImporter writes for a stock 44.1 kHz WAV
    asset.channels   = 1;
    asset.audioData  = makeSilence(4410, 1);
    HE::UUID assetId = content.registerAudio(std::move(asset));

    auto e = world.createEntity("SpeakerEntity");
    AudioSourceComponent src;
    src.assetId     = assetId;
    src.playOnStart = true;
    world.registry().emplace<AudioSourceComponent>(e, src);

    AudioSystem::playOnStart(world, engine, &content);

    const auto& stored = world.registry().get<AudioSourceComponent>(e);
    REQUIRE(stored.handle != 0);
    CHECK(engine.getSoundSampleRate(stored.handle) == 44100);
    engine.shutdown();
}

TEST_CASE("AudioSourceComponent: busName serializes and deserializes")
{
    HorizonWorld src_world;
    auto e = src_world.createEntity("Speaker");
    AudioSourceComponent src;
    src.busName = "Music";
    src_world.registry().emplace<AudioSourceComponent>(e, src);

    const auto snapshotPath = std::filesystem::temp_directory_path() / "he_audio_bus_test.hescene";
    SceneSerializer serializer;
    REQUIRE(serializer.save(src_world, snapshotPath, SerializeFormat::JSON));

    HorizonWorld dst_world;
    REQUIRE(serializer.load(dst_world, snapshotPath, SerializeFormat::JSON));

    auto view = dst_world.registry().view<AudioSourceComponent>();
    REQUIRE(!view.empty());
    const auto& loaded = dst_world.registry().get<AudioSourceComponent>(*view.begin());
    CHECK(loaded.busName == "Music");
}

// ─── Transport (editor preview / audio tab) ──────────────────────────────────
// All of these run in noDevice mode, where nothing pulls frames through the
// mixer — so a sound never ADVANCES on its own here. That is exactly why the
// tests below drive the cursor with seekSound() and read it back rather than
// waiting for playback to move it.

TEST_CASE("AudioEngine: getSoundLengthFrames reports the buffer length")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(4800, 2);
    uint64_t h = engine.play(pcm, 48000, 2);
    REQUIRE(h != 0);
    CHECK(engine.getSoundLengthFrames(h) == 4800);

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: seekSound moves the cursor and it reads back")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(48000, 1);
    uint64_t h = engine.play(pcm, 48000, 1);
    REQUIRE(h != 0);
    CHECK(engine.getSoundCursorFrames(h) == 0);

    engine.seekSound(h, 12000);
    CHECK(engine.getSoundCursorFrames(h) == 12000);

    engine.seekSound(h, 0);
    CHECK(engine.getSoundCursorFrames(h) == 0);

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: cursor is in SOURCE frames, unaffected by pitch")
{
    // The audio tab maps cursor/sampleRate straight to seconds. That only holds
    // if the cursor counts frames of the clip rather than of the mixer's output,
    // which resampling at a non-1.0 pitch would otherwise skew.
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(48000, 1);
    uint64_t h = engine.play(pcm, 48000, 1, 1.0f, 2.0f);
    REQUIRE(h != 0);
    CHECK(engine.getSoundLengthFrames(h) == 48000);

    engine.seekSound(h, 24000);
    CHECK(engine.getSoundCursorFrames(h) == 24000);

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: pause keeps the voice and the cursor, resume restarts it")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(48000, 2);
    uint64_t h = engine.play(pcm, 48000, 2);
    REQUIRE(h != 0);
    engine.seekSound(h, 9000);

    engine.pauseSound(h);
    CHECK(!engine.isPlaying(h));           // paused reads as not-playing …
    CHECK(engine.getSoundCursorFrames(h) == 9000); // … but the voice is still there

    engine.resumeSound(h);
    CHECK(engine.isPlaying(h));
    CHECK(engine.getSoundCursorFrames(h) == 9000);

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: isPaused tells a paused voice from a finished or unknown one")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(48000, 2);
    uint64_t h = engine.play(pcm, 48000, 2);
    REQUIRE(h != 0);
    CHECK(!engine.isPaused(h));            // playing

    engine.pauseSound(h);
    CHECK(engine.isPaused(h));
    CHECK(!engine.isPlaying(h));           // both false is the ambiguity isPaused resolves

    engine.resumeSound(h);
    CHECK(!engine.isPaused(h));
    CHECK(engine.isPlaying(h));

    engine.pauseSound(h);
    engine.stop(h);                        // stopped voice is gone: not paused, not playing
    CHECK(!engine.isPaused(h));
    CHECK(!engine.isPaused(99999));        // unknown handle
    engine.shutdown();
}

TEST_CASE("AudioEngine: getSoundVolume/getSoundPitch read back what play and the setters left")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(4800, 2);
    uint64_t h = engine.play(pcm, 48000, 2, 0.6f, 1.25f);
    REQUIRE(h != 0);
    CHECK(engine.getSoundVolume(h) == doctest::Approx(0.6f));
    CHECK(engine.getSoundPitch(h)  == doctest::Approx(1.25f));

    engine.setSoundVolume(h, 0.3f);
    engine.setSoundPitch(h, 0.5f);
    CHECK(engine.getSoundVolume(h) == doctest::Approx(0.3f));
    CHECK(engine.getSoundPitch(h)  == doctest::Approx(0.5f));

    // Unknown handle: the neutral value of each, so a UI bound to a dead
    // handle shows "silent" and "unchanged" rather than garbage.
    CHECK(engine.getSoundVolume(99999) == doctest::Approx(0.0f));
    CHECK(engine.getSoundPitch(99999)  == doctest::Approx(1.0f));

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: live looping/volume/pitch changes are safe on a playing voice")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(4800, 2);
    uint64_t h = engine.play(pcm, 48000, 2);
    REQUIRE(h != 0);

    engine.setSoundLooping(h, true);
    engine.setSoundVolume(h, 0.25f);
    engine.setSoundPitch(h, 1.5f);
    CHECK(engine.isPlaying(h));

    engine.stop(h);
    engine.shutdown();
}

TEST_CASE("AudioEngine: transport calls on an unknown handle are no-ops")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    CHECK(engine.getSoundCursorFrames(0)     == 0);
    CHECK(engine.getSoundLengthFrames(0)     == 0);
    CHECK(engine.getSoundCursorFrames(99999) == 0);
    CHECK(engine.getSoundLengthFrames(99999) == 0);

    // None of these may touch anything for a handle that does not exist.
    engine.seekSound(99999, 100);
    engine.pauseSound(99999);
    engine.resumeSound(99999);
    engine.setSoundLooping(99999, true);
    engine.setSoundVolume(99999, 0.5f);
    engine.setSoundPitch(99999, 2.0f);

    engine.shutdown();
}

TEST_CASE("AudioEngine: stop after pause releases the voice")
{
    // The audio tab reaps finished voices to give multi-megabyte PCM copies back;
    // a paused voice has to survive that path and still be stoppable.
    AudioEngine engine;
    REQUIRE(engine.init(true));

    auto pcm = makeSilence(4800, 1);
    uint64_t h = engine.play(pcm, 48000, 1);
    REQUIRE(h != 0);

    engine.pauseSound(h);
    engine.stop(h);
    CHECK(!engine.isPlaying(h));
    CHECK(engine.getSoundCursorFrames(h) == 0); // handle is gone entirely

    engine.shutdown();
}

// ─── Ogg Vorbis: compressed clips, decoded as they play ──────────────────────
// The fixture is the only Vorbis stream the tests own (no encoder is linked in):
// 0.25 s of a 440 Hz sine, 22050 Hz mono, 5512 frames. Everything below runs in
// noDevice mode, like the transport tests above — the decoder is exercised by
// init/seek/length and by the offline decode, not by the mixer pulling frames.

#include "fixtures/tone_vorbis.h"

static AudioAsset makeVorbisClip()
{
    AudioAsset a;
    a.type       = HE::AssetType::Audio;
    a.name       = "tone";
    a.encoding   = AudioEncoding::Vorbis;
    a.sampleRate = he_test::kToneVorbisSampleRate;
    a.channels   = he_test::kToneVorbisChannels;
    a.audioData.assign(he_test::kToneVorbisOgg,
                       he_test::kToneVorbisOgg + he_test::kToneVorbisOggSize);
    return a;
}

TEST_CASE("AudioEngine::decodeToPcm16 turns a Vorbis clip into the right amount of int16 PCM")
{
    const AudioAsset clip = makeVorbisClip();
    std::vector<uint8_t> pcm;
    REQUIRE(AudioEngine::decodeToPcm16(clip, pcm));

    const size_t frames = pcm.size() / (sizeof(int16_t) * he_test::kToneVorbisChannels);
    CHECK(frames == he_test::kToneVorbisFrames);

    // A -6 dBFS sine has to come back as one: the peak lands in a narrow band,
    // which is what tells silence, garbage and a wrong sample format apart.
    const auto* s = reinterpret_cast<const int16_t*>(pcm.data());
    int peak = 0;
    for (size_t i = 0; i < frames; ++i) peak = std::max(peak, std::abs(static_cast<int>(s[i])));
    CHECK(peak >= he_test::kToneVorbisPeakMin);
    CHECK(peak <= he_test::kToneVorbisPeakMax);
}

TEST_CASE("AudioEngine::decodeToPcm16 copies a PCM clip and refuses garbage")
{
    AudioAsset pcmClip;
    pcmClip.sampleRate = 8000; pcmClip.channels = 1;
    pcmClip.audioData  = makeSilence(10, 1);
    std::vector<uint8_t> out;
    REQUIRE(AudioEngine::decodeToPcm16(pcmClip, out));
    CHECK(out == pcmClip.audioData);

    AudioAsset junk = makeVorbisClip();
    junk.audioData.assign(512, 0x5A);   // not an Ogg page in sight
    CHECK_FALSE(AudioEngine::decodeToPcm16(junk, out));
    CHECK(out.empty());

    AudioAsset empty = makeVorbisClip();
    empty.audioData.clear();
    CHECK_FALSE(AudioEngine::decodeToPcm16(empty, out));
}

TEST_CASE("AudioEngine: a Vorbis clip plays as a streamed voice with the stream's own length and rate")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    const AudioAsset clip = makeVorbisClip();
    const uint64_t h = engine.play(clip);
    REQUIRE(h != 0);
    // Length and rate come from the decoder (pull mode over memory), in SOURCE
    // frames — the same contract the PCM path has.
    CHECK(engine.getSoundLengthFrames(h) == he_test::kToneVorbisFrames);
    CHECK(engine.getSoundSampleRate(h)   == he_test::kToneVorbisSampleRate);
    CHECK(engine.getSoundCursorFrames(h) == 0);

    engine.seekSound(h, 3000);
    CHECK(engine.getSoundCursorFrames(h) == 3000);

    engine.pauseSound(h);
    CHECK(!engine.isPlaying(h));
    engine.resumeSound(h);
    CHECK(engine.isPlaying(h));
    engine.setSoundLooping(h, true);
    engine.setSoundVolume(h, 0.5f);
    engine.setSoundPitch(h, 1.5f);

    engine.stop(h);
    CHECK(!engine.isPlaying(h));
    engine.shutdown();
}

TEST_CASE("AudioEngine: a Vorbis clip plays spatially and through a bus")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));
    REQUIRE(engine.createBus("Music", 0.8f));

    const AudioAsset clip = makeVorbisClip();
    const uint64_t h = engine.playSpatial(clip, 1.0f, 1.0f, true, 1.0f, 2.0f, 3.0f, 1.0f, 10.0f, "Music");
    REQUIRE(h != 0);
    engine.setSoundPosition(h, 4.0f, 5.0f, 6.0f);
    CHECK(engine.getSoundLengthFrames(h) == he_test::kToneVorbisFrames);

    engine.stopAll();
    CHECK(!engine.isPlaying(h));
    engine.shutdown();
}

TEST_CASE("AudioEngine: a Vorbis clip the decoder rejects returns 0 instead of a voice")
{
    AudioEngine engine;
    REQUIRE(engine.init(true));

    AudioAsset junk = makeVorbisClip();
    junk.audioData.assign(512, 0x5A);
    CHECK(engine.play(junk) == 0);

    // A stream cut off in the middle of its headers must not get through either.
    AudioAsset cut = makeVorbisClip();
    cut.audioData.resize(40);
    CHECK(engine.play(cut) == 0);

    engine.shutdown();
}

TEST_CASE("AudioSystem: playOnStart plays a Vorbis asset from the ContentManager")
{
    // A game reaches a clip through ContentManager and AudioSystem — the whole
    // chain, not just the engine, has to hand a compressed asset through intact.
    HorizonWorld   world;
    ContentManager content;
    const HE::UUID assetId = content.registerAudio(makeVorbisClip());

    auto e = world.createEntity("Speaker");
    AudioSourceComponent src;
    src.assetId     = assetId;
    src.playOnStart = true;
    world.registry().emplace<AudioSourceComponent>(e, src);

    AudioEngine engine;
    REQUIRE(engine.init(true));
    AudioSystem::playOnStart(world, engine, &content);

    const auto& stored = world.registry().get<AudioSourceComponent>(e);
    REQUIRE(stored.handle != 0);
    CHECK(engine.getSoundSampleRate(stored.handle)   == he_test::kToneVorbisSampleRate);
    CHECK(engine.getSoundLengthFrames(stored.handle) == he_test::kToneVorbisFrames);

    engine.shutdown();
}

TEST_CASE("AudioEngine: the mixer decodes a Vorbis voice as it pulls frames")
{
    // Everything above only sets a voice up. This is the streaming path itself:
    // no PCM exists anywhere until the mixer asks, and what it asks for has to
    // come out as the tone — resampled 22050 → 48000 and spread to stereo — not
    // as silence, and not as a burst of garbage.
    AudioEngine engine;
    REQUIRE(engine.init(true));
    const int ch = engine.outputChannels();
    REQUIRE(ch >= 1);

    const AudioAsset clip = makeVorbisClip();
    const uint64_t h = engine.play(clip);
    REQUIRE(h != 0);

    // 0.1 s at 48 kHz, well inside the 0.25 s clip.
    const uint64_t frames = 4800;
    std::vector<float> mix(static_cast<size_t>(frames) * ch, 0.0f);
    const uint64_t got = engine.readMixedFrames(mix.data(), frames);
    CHECK(got == frames);

    float peak = 0.0f; size_t nonFinite = 0;
    for (float v : mix)
    {
        if (!std::isfinite(v)) { ++nonFinite; continue; }
        peak = std::max(peak, std::fabs(v));
    }
    CHECK(nonFinite == 0);
    // -6 dBFS sine through a unity chain: the peak sits near 0.5 (the resampler
    // and the mixer's own headroom move it by a few percent at most).
    CHECK(peak > 0.35f);
    CHECK(peak < 0.70f);

    // And the cursor moved by what was pulled, in SOURCE frames: 4800 engine
    // frames at 48 kHz are 2205 clip frames at 22050 Hz, plus the few hundred
    // the resampler reads ahead (it pulls input in whole chunks) — but nowhere
    // near 4800, which is what a cursor counting engine frames would show.
    const uint64_t cursor = engine.getSoundCursorFrames(h);
    CHECK(cursor >= 2100);
    CHECK(cursor <= 3200);

    engine.stop(h);
    engine.shutdown();
}
