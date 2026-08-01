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
