#include "doctest.h"
#include <HorizonScene/HorizonScene.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/AudioSystem.h>
#include <ContentManager/ContentManager.h>
#include <Types/UUID.h>

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
