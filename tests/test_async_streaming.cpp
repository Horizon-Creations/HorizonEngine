#include "doctest.h"
#include "TestFsUtil.h"
#include <ContentManager/ContentManager.h>
#include <ContentManager/HAsset.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <future>
#include <JobSystem/JobSystem.h>

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<uint8_t> makeMaterialAssetBytes(const HE::UUID& id,
                                                    const std::string& name,
                                                    float r = 1.f, float g = 0.f, float b = 0.f)
{
    std::vector<uint8_t> meta;
    const uint16_t typeVal = static_cast<uint16_t>(HE::AssetType::Material);
    HAsset::Writer::appendPOD(meta, typeVal);
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, name);
    HAsset::Writer::appendString(meta, "async_test/" + name + ".hasset");

    std::vector<uint8_t> mtrl;
    HAsset::Writer::appendString(mtrl, "");        // shaderPath
    const uint64_t texCount = 0;
    HAsset::Writer::appendPOD(mtrl, texCount);     // texturePaths (0 entries)
    HAsset::Writer::appendPOD(mtrl, r);
    HAsset::Writer::appendPOD(mtrl, g);
    HAsset::Writer::appendPOD(mtrl, b);
    float metallic = 0.f, roughness = 0.5f, opacity = 1.f;
    HAsset::Writer::appendPOD(mtrl, metallic);
    HAsset::Writer::appendPOD(mtrl, roughness);
    HAsset::Writer::appendPOD(mtrl, opacity);

    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());
    return w.toBytes(typeVal);
}

// Write a material .hasset to a temp file; returns the full path.
static std::filesystem::path writeTempAsset(const std::string& filename,
                                             const HE::UUID& id,
                                             float r = 1.f, float g = 0.f, float b = 0.f)
{
    auto path = std::filesystem::temp_directory_path() / filename;
    const auto bytes = makeMaterialAssetBytes(id, filename, r, g, b);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return path;
}

// Poll repeatedly until no more pending jobs or timeout.
static void drainAsync(ContentManager& cm, int maxIterations = 100)
{
    for (int i = 0; i < maxIterations; ++i)
    {
        cm.pollAsyncResults();
        // Brief sleep to let background threads complete their I/O.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ─── Tests ───────────────────────────────────────────────────────────────────

TEST_CASE("loadAssetAsync: basic round-trip fires callback with valid UUID")
{
    const HE::UUID id{0xA5A5A5A5A5A5A5A5ULL, 0x5A5A5A5A5A5A5A5AULL};
    const auto full = writeTempAsset("async_basic.hasset", id, 0.1f, 0.2f, 0.3f);

    // Create a ContentManager rooted at the temp directory
    ContentManager cm(full.parent_path().string());

    HE::UUID gotId;
    bool fired = false;
    cm.loadAssetAsync("async_basic.hasset", [&](HE::UUID u) {
        gotId  = u;
        fired  = true;
    });

    CHECK(cm.isAsyncPending("async_basic.hasset"));

    // Poll until done (background I/O finishes)
    for (int i = 0; i < 200 && !fired; ++i)
    {
        cm.pollAsyncResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(fired);
    CHECK(gotId == id);
    CHECK(cm.isLoaded("async_basic.hasset"));
    CHECK(!cm.isAsyncPending("async_basic.hasset"));

    const MaterialAsset* mat = cm.getMaterial(gotId);
    REQUIRE(mat != nullptr);
    CHECK(mat->baseColor[0] == doctest::Approx(0.1f));
    CHECK(mat->baseColor[1] == doctest::Approx(0.2f));
    CHECK(mat->baseColor[2] == doctest::Approx(0.3f));

    he_test::removeQuiet(full);
}

TEST_CASE("loadAssetAsync: already-loaded asset fires callback immediately")
{
    const HE::UUID id{0xBEBEBEBEBEBEBEBEULL, 0x1212121212121212ULL};
    const auto full = writeTempAsset("async_preloaded.hasset", id);

    ContentManager cm(full.parent_path().string());
    // Load synchronously first
    const HE::UUID syncId = cm.loadAsset("async_preloaded.hasset");
    REQUIRE(syncId == id);

    bool fired = false;
    HE::UUID gotId;
    cm.loadAssetAsync("async_preloaded.hasset", [&](HE::UUID u) {
        gotId = u;
        fired = true;
    });

    // Should fire immediately — no poll needed
    CHECK(fired);
    CHECK(gotId == id);
    CHECK(!cm.isAsyncPending("async_preloaded.hasset"));

    he_test::removeQuiet(full);
}

TEST_CASE("loadAssetAsync: nonexistent path fires callback with empty UUID")
{
    ContentManager cm("/tmp");

    bool fired = false;
    HE::UUID gotId{1, 1}; // non-null sentinel
    cm.loadAssetAsync("nonexistent_xyz.hasset", [&](HE::UUID u) {
        gotId = u;
        fired = true;
    });

    for (int i = 0; i < 200 && !fired; ++i)
    {
        cm.pollAsyncResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(fired);
    CHECK(gotId == HE::UUID{});
}

TEST_CASE("loadAssetAsync: multiple concurrent requests all complete")
{
    const HE::UUID id1{0x1111111111111111ULL, 0xAAAAAAAAAAAAAAAAULL};
    const HE::UUID id2{0x2222222222222222ULL, 0xBBBBBBBBBBBBBBBBULL};
    const HE::UUID id3{0x3333333333333333ULL, 0xCCCCCCCCCCCCCCCCULL};

    auto tmp = std::filesystem::temp_directory_path();
    writeTempAsset("async_multi1.hasset", id1, 1.f, 0.f, 0.f);
    writeTempAsset("async_multi2.hasset", id2, 0.f, 1.f, 0.f);
    writeTempAsset("async_multi3.hasset", id3, 0.f, 0.f, 1.f);

    ContentManager cm(tmp.string());

    int firedCount = 0;
    cm.loadAssetAsync("async_multi1.hasset", [&](HE::UUID) { ++firedCount; });
    cm.loadAssetAsync("async_multi2.hasset", [&](HE::UUID) { ++firedCount; });
    cm.loadAssetAsync("async_multi3.hasset", [&](HE::UUID) { ++firedCount; });

    for (int i = 0; i < 400 && firedCount < 3; ++i)
    {
        cm.pollAsyncResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(firedCount == 3);
    CHECK(cm.getMaterial(id1) != nullptr);
    CHECK(cm.getMaterial(id2) != nullptr);
    CHECK(cm.getMaterial(id3) != nullptr);

    he_test::removeQuiet(tmp / "async_multi1.hasset");
    he_test::removeQuiet(tmp / "async_multi2.hasset");
    he_test::removeQuiet(tmp / "async_multi3.hasset");
}

TEST_CASE("loadAssetAsync: duplicate in-flight request is coalesced")
{
    const HE::UUID id{0xDDDDDDDDDDDDDDDDULL, 0xEEEEEEEEEEEEEEEEULL};
    const auto full = writeTempAsset("async_dup.hasset", id);

    ContentManager cm(full.parent_path().string());

    int firedCount = 0;
    cm.loadAssetAsync("async_dup.hasset", [&](HE::UUID) { ++firedCount; });
    cm.loadAssetAsync("async_dup.hasset", [&](HE::UUID) { ++firedCount; }); // duplicate

    for (int i = 0; i < 200; ++i)
    {
        cm.pollAsyncResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Only one job was submitted — only one callback fires
    CHECK(firedCount == 1);

    he_test::removeQuiet(full);
}

TEST_CASE("pollAsyncResults returns UUIDs of registered assets")
{
    const HE::UUID id{0xF0F0F0F0F0F0F0F0ULL, 0x0F0F0F0F0F0F0F0FULL};
    const auto full = writeTempAsset("async_return.hasset", id);

    ContentManager cm(full.parent_path().string());
    cm.loadAssetAsync("async_return.hasset");

    std::vector<HE::UUID> collected;
    for (int i = 0; i < 200; ++i)
    {
        auto batch = cm.pollAsyncResults();
        collected.insert(collected.end(), batch.begin(), batch.end());
        if (!collected.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(collected.size() == 1);
    CHECK(collected[0] == id);

    he_test::removeQuiet(full);
}

TEST_CASE("isAsyncPending tracks in-flight state correctly")
{
    const HE::UUID id{0xCAFECAFECAFECAFEULL, 0xDEADDEADDEADDEADULL};
    const auto full = writeTempAsset("async_pending.hasset", id);

    ContentManager cm(full.parent_path().string());

    CHECK(!cm.isAsyncPending("async_pending.hasset"));
    cm.loadAssetAsync("async_pending.hasset");
    CHECK(cm.isAsyncPending("async_pending.hasset"));

    for (int i = 0; i < 200; ++i)
    {
        cm.pollAsyncResults();
        if (!cm.isAsyncPending("async_pending.hasset")) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(!cm.isAsyncPending("async_pending.hasset"));
    CHECK(cm.isLoaded("async_pending.hasset"));

    he_test::removeQuiet(full);
}

// Regression: a job submitted fire-and-forget outlives its ContentManager whenever
// the owner is destroyed before the worker runs — play-mode stop, a closed project,
// or a test that submits and never drains (test_project_exporter.cpp:481 does exactly
// that). The worker used to lock this->m_resultsMutex and push into
// this->m_asyncResults after the owner was gone, writing a string, a vector and a
// std::function into freed memory. On glibc that corrupts the allocator's tcache and
// aborts an unrelated malloc much later, which is how it showed up: a sporadic
// SIGABRT ("malloc(): unaligned tcache chunk detected") in a Python scripting test
// ~20 test cases downstream.
//
// The pool is saturated first on purpose. Without that the worker almost always wins
// the race on a fast idle machine (verified: an in-flight counter in ~ContentManager
// stayed at 0 over 70 macOS runs, 30 of them under full CPU load) and the test would
// pass whether or not the bug is present. Blocking every worker makes the job sit in
// the queue, so the owner is guaranteed to die first.
TEST_CASE("ContentManager destroyed with an async job still queued is safe")
{
    const HE::UUID id{0x1111111111111111ULL, 0x2222222222222222ULL};
    const auto full = writeTempAsset("async_owner_outlived.hasset", id);

    ThreadPool& pool = globalPool();
    std::atomic<bool> release{false};
    std::vector<std::future<void>> blockers;
    for (size_t i = 0; i < pool.threadCount(); ++i)
        blockers.push_back(pool.submit([&release]{
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }));

    {
        ContentManager cm(full.parent_path().string());
        cm.loadAssetAsync("async_owner_outlived.hasset", [](HE::UUID) {});
        CHECK(cm.isAsyncPending("async_owner_outlived.hasset"));
    }   // owner destroyed with the job still queued

    release.store(true);
    for (auto& f : blockers) f.get();
    // Give the freed job its chance to run against a dead owner. Clean under ASan/TSan;
    // before the shared-sink fix this wrote into freed memory.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    he_test::removeQuiet(full);
    CHECK(true);
}

// ─── Block-wise read + live progress ─────────────────────────────────────────
// The worker used to slurp the file with a single istreambuf_iterator, which gave
// the caller nothing to show while a several-hundred-megabyte asset streamed in.
// It now reads in 4 MiB blocks and publishes the byte counts (asyncProgress), which
// is what the Material Editor's preview-mesh picker draws its progress bar from.
//
// The interesting risk in that rewrite is the reassembly: a file spanning several
// blocks must come back byte-identical. This asset carries a ~9 MiB payload (three
// blocks, last one partial) and is checked across both block boundaries.
TEST_CASE("loadAssetAsync: multi-block read is byte-exact and reports progress")
{
    const HE::UUID id{0x0B10CB10CB10CB10ULL, 0x0123456789ABCDEFULL};
    const std::string name = "async_bigpayload";

    // A material whose custom-shader string is the bulk payload — a pattern, so a
    // mis-stitched block boundary shows up as a mismatch rather than as zeroes.
    std::string payload(9u * 1024u * 1024u + 12345u, '\0');
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>('a' + (i % 26));

    std::vector<uint8_t> meta;
    const uint16_t typeVal = static_cast<uint16_t>(HE::AssetType::Material);
    HAsset::Writer::appendPOD(meta, typeVal);
    HAsset::Writer::appendPOD(meta, id.hi);
    HAsset::Writer::appendPOD(meta, id.lo);
    HAsset::Writer::appendString(meta, name);
    HAsset::Writer::appendString(meta, name + ".hasset");

    std::vector<uint8_t> mtrl;
    HAsset::Writer::appendString(mtrl, "");    // shaderPath
    const uint64_t texCount = 0;
    HAsset::Writer::appendPOD(mtrl, texCount); // texturePaths
    float c = 0.5f;
    HAsset::Writer::appendPOD(mtrl, c); HAsset::Writer::appendPOD(mtrl, c); HAsset::Writer::appendPOD(mtrl, c);
    float metallic = 0.f, roughness = 0.5f, opacity = 1.f;
    HAsset::Writer::appendPOD(mtrl, metallic);
    HAsset::Writer::appendPOD(mtrl, roughness);
    HAsset::Writer::appendPOD(mtrl, opacity);
    HAsset::Writer::appendString(mtrl, payload); // customShaderFragGlsl — the bulk

    HAsset::Writer w;
    w.addChunk(HAsset::CHUNK_META, meta.data(), meta.size());
    w.addChunk(HAsset::CHUNK_MTRL, mtrl.data(), mtrl.size());
    const auto bytes = w.toBytes(typeVal);

    const auto full = std::filesystem::temp_directory_path() / (name + ".hasset");
    {
        std::ofstream f(full, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }

    ContentManager cm(full.parent_path().string());
    cm.loadAssetAsync(name + ".hasset");

    // Sample the live counters. Whatever we catch must be self-consistent: the total
    // is either "not opened yet" or the real file size, and the read count only grows.
    std::uint64_t lastRead = 0;
    for (int i = 0; i < 200 && cm.isAsyncPending(name + ".hasset"); ++i)
    {
        std::uint64_t rd = 0, total = 0;
        if (cm.asyncProgress(name + ".hasset", rd, total))
        {
            CHECK(rd >= lastRead);
            CHECK(rd <= (total ? total : rd));
            CHECK((total == 0 || total == bytes.size()));
            lastRead = rd;
        }
        cm.pollAsyncResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    drainAsync(cm, 100);

    // Landed, and the progress cell is gone with it (no leak per finished load).
    std::uint64_t rd = 0, total = 0;
    CHECK_FALSE(cm.asyncProgress(name + ".hasset", rd, total));
    REQUIRE(cm.isLoaded(name + ".hasset"));

    const MaterialAsset* mat = cm.getMaterial(cm.loadAsset(name + ".hasset"));
    REQUIRE(mat != nullptr);
    REQUIRE(mat->customShaderFragGlsl.size() == payload.size());
    CHECK(mat->customShaderFragGlsl == payload);   // every block, stitched in order

    he_test::removeQuiet(full);
}

// ─── A remote-only EngineContent asset addressed by PATH ─────────────────────
// A loose material names its textures by path only (graphTexturePaths, no baked
// UUID), so nothing but the path ever asks for an EngineContent texture that is not
// on this machine yet. The path loaders have to start the server download themselves:
// registerRemoteAsset() is otherwise only reached through a UUID.

namespace
{
    // Stands in for the SFTP download: the file lands where resolveAbsolutePath looks
    // for the shipped default, then the test completes the materialize callback.
    void landRemoteAsset(const std::filesystem::path& file, const HE::UUID& id)
    {
        const auto bytes = makeMaterialAssetBytes(id, "T_Remote");
        std::filesystem::create_directories(file.parent_path());
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
}

TEST_CASE("remote EngineContent asset: loadAsset(path) starts the download once and loads it once it landed")
{
    const auto base       = std::filesystem::temp_directory_path() / "he_test_remote_path_a";
    he_test::removeAllQuiet(base);
    const auto contentDir = base / "Content";
    const auto engineDir  = base / "EngineContent";
    std::filesystem::create_directories(contentDir);
    std::filesystem::create_directories(engineDir);

    const HE::UUID    id{0xC0DE0001C0DE0001ULL, 0x1ULL};
    const std::string rel = "Engine/Textures/Landscape/T_Remote.hasset";

    ContentManager cm(contentDir.string());
    cm.setEngineContentRoot(engineDir.string());

    int                       started = 0;
    std::function<void(bool)> finish;
    cm.registerRemoteAsset(id, rel, [&](std::function<void(bool)> done) {
        ++started;
        finish = std::move(done);
    });
    CHECK(cm.isRemoteAssetPending(rel));
    CHECK_FALSE(cm.isRemoteAssetPending("Engine/Textures/Landscape/Other.hasset"));
    CHECK(started == 0);                         // registering starts nothing

    // The file is not there: no asset yet, but the download is under way.
    CHECK(cm.loadAsset(rel) == HE::UUID{});
    CHECK(started == 1);
    CHECK(cm.loadAsset(rel) == HE::UUID{});      // asking again while it is in flight ...
    CHECK(started == 1);                         // ... does not start a second download

    // A path the server does not offer is still just a miss, nothing is started.
    CHECK(cm.loadAsset("Engine/Textures/Landscape/Nope.hasset") == HE::UUID{});
    CHECK(started == 1);

    // The download lands.
    landRemoteAsset(engineDir / "Textures" / "Landscape" / "T_Remote.hasset", id);
    const uint64_t epoch = cm.contentEpoch();
    REQUIRE(finish);
    finish(true);
    for (int i = 0; i < 200 && !cm.isLoaded(rel); ++i)
    {
        cm.pollAsyncResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(cm.contentEpoch() > epoch);            // whoever remembered "not there" looks again
    CHECK_FALSE(cm.isRemoteAssetPending(rel));
    CHECK(cm.isLoaded(rel));
    CHECK(cm.loadAsset(rel) == id);
    CHECK(started == 1);

    he_test::removeAllQuiet(base);
}

TEST_CASE("remote EngineContent asset: loadAssetAsync(path) downloads first and calls back with the loaded asset")
{
    const auto base       = std::filesystem::temp_directory_path() / "he_test_remote_path_b";
    he_test::removeAllQuiet(base);
    const auto contentDir = base / "Content";
    const auto engineDir  = base / "EngineContent";
    std::filesystem::create_directories(contentDir);
    std::filesystem::create_directories(engineDir);

    const HE::UUID    id{0xC0DE0002C0DE0002ULL, 0x1ULL};
    const std::string rel = "Engine/Textures/Landscape/T_Remote.hasset";

    ContentManager cm(contentDir.string());
    cm.setEngineContentRoot(engineDir.string());

    int                       started = 0;
    std::function<void(bool)> finish;
    cm.registerRemoteAsset(id, rel, [&](std::function<void(bool)> done) {
        ++started;
        finish = std::move(done);
    });

    HE::UUID got;
    bool     fired = false;
    cm.loadAssetAsync(rel, [&](HE::UUID u) { got = u; fired = true; });
    CHECK(started == 1);
    CHECK_FALSE(fired);                          // nothing to read yet

    landRemoteAsset(engineDir / "Textures" / "Landscape" / "T_Remote.hasset", id);
    REQUIRE(finish);
    finish(true);
    for (int i = 0; i < 200 && !fired; ++i)
    {
        cm.pollAsyncResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(fired);
    CHECK(got == id);
    CHECK(cm.isLoaded(rel));

    he_test::removeAllQuiet(base);
}

TEST_CASE("remote EngineContent asset: a file that is on disk wins, the download is not started")
{
    const auto base       = std::filesystem::temp_directory_path() / "he_test_remote_path_c";
    he_test::removeAllQuiet(base);
    const auto contentDir = base / "Content";
    const auto engineDir  = base / "EngineContent";
    std::filesystem::create_directories(contentDir);

    const HE::UUID    diskId{0xC0DE0003C0DE0003ULL, 0x1ULL};
    const HE::UUID    remoteId{0xC0DE0004C0DE0004ULL, 0x1ULL};
    const std::string rel = "Engine/Textures/Landscape/T_Remote.hasset";
    landRemoteAsset(engineDir / "Textures" / "Landscape" / "T_Remote.hasset", diskId);

    ContentManager cm(contentDir.string());
    cm.setEngineContentRoot(engineDir.string());

    int started = 0;
    cm.registerRemoteAsset(remoteId, rel, [&](std::function<void(bool)>) { ++started; });

    CHECK(cm.loadAsset(rel) == diskId);          // the shipped default, as before
    CHECK(started == 0);

    he_test::removeAllQuiet(base);
}

// ─── Cancellation (Thema 153, Schritt 2) ─────────────────────────────────────
// Streaming loads run as Low jobs with a stale() check: a load is dropped once
// every requester's CancelToken is cancelled. The trap these tests guard is not
// in the job system but in the coalesce key: a dropped job that reported nothing
// would leave its key in the pending set forever, and every later request for
// the same asset would be swallowed as "already in flight".

namespace {

// Holds every worker of the global pool inside a Normal task until release(), so
// loads requested meanwhile stay queued — the only way to cancel "before start"
// deterministically. Released on destruction too, so a failed REQUIRE cannot
// wedge the process-wide pool.
struct HeldPool
{
    std::atomic<bool> go{ false };
    std::atomic<size_t> parked{ 0 };
    std::vector<std::future<void>> blockers;
    HeldPool()
    {
        ThreadPool& pool = globalPool();
        for (size_t i = 0; i < pool.threadCount(); ++i)
            blockers.push_back(pool.submit([this] {
                parked.fetch_add(1);
                while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }, "TestHeldPool"));
        while (parked.load() < pool.threadCount()) std::this_thread::yield();
    }
    void release()
    {
        go.store(true);
        for (auto& b : blockers) if (b.valid()) b.get();
    }
    ~HeldPool() { release(); }
};

HE::AsyncLoadOptions withToken(const HE::CancelToken& t)
{
    HE::AsyncLoadOptions o;
    o.cancel = t;
    return o;
}

// drainAsync pumps a fixed 100 rounds; these stop as soon as `done` holds. That
// matters on a Mac in Low Power Mode, where a 5 ms sleep was measured at ~150 ms
// (timer coalescing) and a fixed drain alone costs 15 s.
template<typename P>
void drainUntil(ContentManager& cm, P done, int maxRounds = 2000)
{
    for (int i = 0; i < maxRounds; ++i)
    {
        cm.pollAsyncResults();
        if (done()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace

TEST_CASE("loadAssetAsync: cancelled before it starts → empty callback, key released, reload works")
{
    const HE::UUID id{0xCA9CE1ED00000001ULL, 0x0000000000000001ULL};
    const auto full = writeTempAsset("async_cancel_before.hasset", id);
    ContentManager cm(full.parent_path().string());
    const std::string rel = "async_cancel_before.hasset";

    bool called = false;
    HE::UUID got{ 1, 1 };
    {
        HeldPool held;
        HE::CancelToken zone = HE::CancelToken::create();
        cm.loadAssetAsync(rel, [&](HE::UUID u) { called = true; got = u; }, withToken(zone));
        CHECK(cm.isAsyncPending(rel));
        zone.cancel();                       // the zone is gone before its load ran
    }
    drainUntil(cm, [&] { return called; });
    CHECK(called);
    CHECK(got == HE::UUID{});                // reported like a failed load
    CHECK_FALSE(cm.isLoaded(rel));
    CHECK_FALSE(cm.isAsyncPending(rel));     // the key did not leak
    CHECK(cm.asyncInFlightCount() == 0);

    // The leaked-key failure mode: this request would be swallowed forever.
    HE::UUID again;
    cm.loadAssetAsync(rel, [&](HE::UUID u) { again = u; });
    drainUntil(cm, [&] { return again != HE::UUID{}; });
    CHECK(again == id);
    CHECK(cm.isLoaded(rel));

    he_test::removeQuiet(full);
}

// The editor's streaming view (Thema 153, Schritt 6): which loads are in
// flight, and what pollAsyncResults did with the ones that came back.
TEST_CASE("ContentManager: in-flight paths and poll counters for the streaming view")
{
    const HE::UUID id{0x5EE0000000000001ULL, 0x0000000000000001ULL};
    const auto good = writeTempAsset("sv_good.hasset", id);
    const auto dir  = good.parent_path();
    writeTempAsset("sv_dropped.hasset", HE::UUID{0x5EE0000000000002ULL, 2});
    {
        std::ofstream junk(dir / "sv_junk.hasset", std::ios::binary);
        junk << "not an asset at all";
    }
    ContentManager cm(dir.string());
    const ContentManager::AsyncPollStats before = cm.asyncPollStats();
    {
        HeldPool held;
        HE::CancelToken zone = HE::CancelToken::create();
        cm.loadAssetAsync("sv_good.hasset");
        cm.loadAssetAsync("sv_junk.hasset");
        cm.loadAssetAsync("sv_dropped.hasset", {}, withToken(zone));
        // Sorted, and cut at the count asked for.
        CHECK(cm.asyncInFlightPaths(10) ==
              std::vector<std::string>{ "sv_dropped.hasset", "sv_good.hasset", "sv_junk.hasset" });
        CHECK(cm.asyncInFlightPaths(1) == std::vector<std::string>{ "sv_dropped.hasset" });
        zone.cancel();
    }
    drainUntil(cm, [&] { return cm.asyncInFlightCount() == 0; });
    CHECK(cm.asyncInFlightPaths(10).empty());
    const ContentManager::AsyncPollStats& after = cm.asyncPollStats();
    CHECK(after.registered - before.registered == 1);
    CHECK(after.failed - before.failed == 1);
    CHECK(after.dropped - before.dropped == 1);
    CHECK(after.restarted == before.restarted);
    CHECK(after.lastPollLeft == 0);

    he_test::removeQuiet(good);
    he_test::removeQuiet(dir / "sv_dropped.hasset");
    he_test::removeQuiet(dir / "sv_junk.hasset");
}

TEST_CASE("loadAssetAsync: a shared load is dropped only when EVERY requester cancelled")
{
    const HE::UUID idA{0xCA9CE1ED00000002ULL, 0x2ULL};
    const HE::UUID idB{0xCA9CE1ED00000003ULL, 0x3ULL};
    const auto fullA = writeTempAsset("async_cancel_shared_a.hasset", idA);
    const auto fullB = writeTempAsset("async_cancel_shared_b.hasset", idB);
    ContentManager cm(fullA.parent_path().string());

    {
        HeldPool held;
        // A: a zone and the main scene both want it; only the zone goes away.
        HE::CancelToken zone  = HE::CancelToken::create();
        HE::CancelToken scene = HE::CancelToken::create();
        cm.loadAssetAsync("async_cancel_shared_a.hasset", {}, withToken(zone));
        cm.loadAssetAsync("async_cancel_shared_a.hasset", {}, withToken(scene));
        // B: a cancellable request plus one without a token, which pins it.
        HE::CancelToken other = HE::CancelToken::create();
        cm.loadAssetAsync("async_cancel_shared_b.hasset", {}, withToken(other));
        cm.loadAssetAsync("async_cancel_shared_b.hasset");
        zone.cancel();
        other.cancel();
    }
    drainUntil(cm, [&] { return cm.asyncInFlightCount() == 0; });
    CHECK(cm.isLoaded("async_cancel_shared_a.hasset"));
    CHECK(cm.isLoaded("async_cancel_shared_b.hasset"));

    he_test::removeQuiet(fullA);
    he_test::removeQuiet(fullB);
}

TEST_CASE("loadAssetAsync: asked again after it was dropped → loaded after all")
{
    // A zone streamed out and straight back in. Depending on timing the second
    // request either joins the queued job before it is dropped, or arrives after
    // the job already gave up and its 'cancelled' result is waiting to be drained
    // — then pollAsyncResults must start it over instead of failing it. Either
    // way the asset has to arrive.
    const HE::UUID id{0xCA9CE1ED00000004ULL, 0x4ULL};
    const auto full = writeTempAsset("async_cancel_revive.hasset", id);
    ContentManager cm(full.parent_path().string());
    const std::string rel = "async_cancel_revive.hasset";

    HE::UUID first{ 1, 1 };
    {
        HeldPool held;
        HE::CancelToken out = HE::CancelToken::create();
        cm.loadAssetAsync(rel, [&](HE::UUID u) { first = u; }, withToken(out));
        out.cancel();
    }
    // Most likely the job has been dropped by now, its result not yet drained.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    HE::CancelToken backIn = HE::CancelToken::create();
    cm.loadAssetAsync(rel, {}, withToken(backIn));   // coalesces onto the same key
    drainUntil(cm, [&] { return cm.asyncInFlightCount() == 0; });
    CHECK(cm.isLoaded(rel));
    CHECK(first == id);                     // the one callback the key carries
    CHECK_FALSE(cm.isAsyncPending(rel));

    he_test::removeQuiet(full);
}
