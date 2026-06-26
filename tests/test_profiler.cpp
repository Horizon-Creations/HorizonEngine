#include "doctest.h"
#include <Diagnostics/Profiler.h>
#include <Diagnostics/EngineProfiler.h>
#include <Diagnostics/GlobalState.h>
#include <Renderer/GpuPassAccumulator.h>

#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── Compile-time / no-op behaviour ────────────────────────────────────────────
// The macros must expand and run cleanly whether or not a capture is recording.

TEST_CASE("Profiler macros compile and execute as no-ops when not recording")
{
    { HE_PROFILE_SCOPE(); }
    { HE_PROFILE_SCOPE_N("TestZone"); }
    HE_PROFILE_FRAME();
    CHECK(true);
}

TEST_CASE("Profiler scope macro can nest")
{
    {
        HE_PROFILE_SCOPE_N("outer");
        {
            HE_PROFILE_SCOPE_N("inner");
        }
    }
    CHECK(true);
}

TEST_CASE("EngineProfiler scopes are zero-state when not recording")
{
    auto& prof = EngineProfiler::instance();
    REQUIRE_FALSE(prof.isRecording());
    // The singleton is shared across test cases and only cleared on capture start,
    // so assert relative to a baseline rather than an absolute 0 (keeps the test
    // independent of doctest's run order — --order-by=name/rand stays green).
    const size_t before = prof.recordedFrames();
    {
        HE_PROFILE_SCOPE_N("ShouldNotRecord");
    }
    // A scope opened while not recording must not record a frame or crash.
    CHECK(prof.recordedFrames() == before);
}

// ── Capture lifecycle + dump ──────────────────────────────────────────────────

static fs::path setupTempDeploy(const char* tag)
{
    fs::path dir = fs::temp_directory_path() / (std::string("he_prof_") + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    // setLogFile stores startupPath; getDumpsDir() derives <parent>/dumps from it.
    GlobalState::getInstance().setLogFile((dir / "fakeexe").string());
    return dir;
}

TEST_CASE("EngineProfiler records frames, scopes and writes a parseable dump")
{
    fs::path deploy = setupTempDeploy("lifecycle");
    auto& prof = EngineProfiler::instance();

    ProfSessionInfo info;
    info.backend = "TestBackend";
    info.os      = "TestOS";
    info.vsync   = false;
    prof.requestStart(info);

    constexpr int kFrames = 5;
    for (int i = 0; i < kFrames; ++i)
    {
        prof.beginFrame(/*deltaMs=*/16.6);    // applies the pending start on i==0
        CHECK(prof.isRecording());
        {
            HE_PROFILE_SCOPE_N("FrameWork");
            {
                HE_PROFILE_SCOPE_N("SubTask");
            }
        }
        ProfRenderStats rs;
        rs.drawCalls      = 42;
        rs.triangles      = 1000;
        rs.visibleObjects = 7;
        rs.totalObjects   = 12;
        prof.setRenderStats(rs);
        // A mix of an exact per-encoder pass and an approximate intra-Scene split.
        prof.setGpuTimes(2.5, { {"Scene", 1.8, false}, {"Sky+Clouds", 1.2, true} });
        prof.endFrame();
    }
    CHECK(prof.recordedFrames() == kFrames);

    // Stop is applied on the next frame boundary, which writes the dump.
    prof.requestStop();
    prof.beginFrame(16.6);
    CHECK_FALSE(prof.isRecording());

    std::string dumpPath;
    REQUIRE(prof.consumeJustDumped(dumpPath));
    REQUIRE_FALSE(dumpPath.empty());
    REQUIRE(fs::exists(dumpPath));
    // Lands in the deploy-adjacent dumps/ folder.
    CHECK(fs::path(dumpPath).parent_path().filename() == "dumps");

    std::ifstream in(dumpPath);
    REQUIRE(in.is_open());
    json j = json::parse(in);

    CHECK(j["session"]["backend"] == "TestBackend");
    CHECK(j["session"]["vsync"] == false);
    CHECK(j["session"]["frameCount"] == kFrames);
    CHECK(j["frames"].size() == kFrames);

    // Scope breakdown present, with the nested SubTask deeper than FrameWork.
    const auto& f0 = j["frames"][0];
    CHECK(f0["stats"]["draws"] == 42);
    CHECK(f0["stats"]["tris"] == 1000);
    CHECK(f0["stats"]["visible"] == 7);
    CHECK(f0["stats"]["total"] == 12);
    CHECK(f0.contains("gpu"));
    bool sawFrameWork = false, sawSubTask = false;
    for (const auto& s : f0["cpu"])
    {
        if (s["n"] == "FrameWork") sawFrameWork = true;
        if (s["n"] == "SubTask")   { sawSubTask = true; CHECK(s["d"].get<int>() >= 1); }
    }
    CHECK(sawFrameWork);
    CHECK(sawSubTask);

    // Per-frame GPU passes: the exact pass has no approx flag, the intra-Scene
    // split is flagged approx so a reader never treats it as ground truth.
    bool sawScene = false, sawSky = false;
    for (const auto& g : f0["gpu"])
    {
        if (g["n"] == "Scene")      { sawScene = true; CHECK_FALSE(g.contains("approx")); }
        if (g["n"] == "Sky+Clouds") { sawSky = true;  CHECK(g.value("approx", false)); }
    }
    CHECK(sawScene);
    CHECK(sawSky);

    // Per-scope + per-pass summaries exist; approx propagates into the summary.
    CHECK(j["summary"]["cpuScopes"].contains("FrameWork"));
    CHECK(j["summary"]["gpuPasses"].contains("Scene"));
    CHECK(j["summary"]["gpuPasses"].contains("Sky+Clouds"));
    CHECK_FALSE(j["summary"]["gpuPasses"]["Scene"].contains("approx"));
    CHECK(j["summary"]["gpuPasses"]["Sky+Clouds"].value("approx", false));

    fs::remove_all(deploy);
}

TEST_CASE("EngineProfiler ring buffer caps retained frames")
{
    setupTempDeploy("ring");
    auto& prof = EngineProfiler::instance();

    ProfSessionInfo info;
    prof.requestStart(info, /*maxFrames=*/3);
    for (int i = 0; i < 10; ++i)
    {
        prof.beginFrame(16.6);
        { HE_PROFILE_SCOPE_N("X"); }
        prof.endFrame();
    }
    CHECK(prof.recordedFrames() == 3);   // only the newest 3 retained

    prof.requestStop();
    prof.beginFrame(16.6);
    std::string p;
    prof.consumeJustDumped(p);
}

// ── GpuPassAccumulator (detailed-capture per-pass GPU timing bookkeeping) ──────

TEST_CASE("GpuPassAccumulator completes a frame once all expected passes report")
{
    GpuPassAccumulator acc;
    IRenderer::FrameGpuStats done;
    // 3 passes, in order; only the 3rd report completes the frame.
    CHECK_FALSE(acc.report(0, "Scene",   0.000, 0.010, 3, done));
    CHECK_FALSE(acc.report(0, "Tonemap", 0.010, 0.011, 3, done));
    CHECK(acc.report(0, "Present", 0.011, 0.012, 3, done));
    CHECK(done.passes.size() == 3);
    // gpuFrameMs = span (maxEnd - minStart) = 0.012 - 0.000 = 12 ms.
    CHECK(done.gpuFrameMs == doctest::Approx(12.0));
    CHECK(acc.inflightCount() == 0);
}

TEST_CASE("GpuPassAccumulator handles out-of-order and empty-pass completions")
{
    GpuPassAccumulator acc;
    IRenderer::FrameGpuStats done;
    // Reported out of submission order; one empty pass (end<=start) still counts.
    CHECK_FALSE(acc.report(7, "Present", 0.020, 0.021, 3, done));
    CHECK_FALSE(acc.report(7, "Shadow",  0.005, 0.005, 3, done)); // empty: 0 ms, counts
    CHECK(acc.report(7, "Scene", 0.006, 0.018, 3, done));
    CHECK(done.passes.size() == 3);
    // Detailed total = Σ exclusive pass times = 1 (Present) + 0 (empty Shadow) + 12 (Scene) = 13 ms.
    CHECK(done.gpuFrameMs == doctest::Approx(13.0));
    double shadowMs = -1.0;
    for (const auto& p : done.passes) if (std::string(p.name) == "Shadow") shadowMs = p.ms;
    CHECK(shadowMs == doctest::Approx(0.0));
}

TEST_CASE("GpuPassAccumulator: newer frame wins, late straggler never clobbers")
{
    GpuPassAccumulator acc;
    IRenderer::FrameGpuStats done;
    // Frame 10 completes fully.
    acc.report(10, "Scene",   0.0, 0.010, 2, done);
    REQUIRE(acc.report(10, "Present", 0.010, 0.012, 2, done));
    CHECK(acc.latest().gpuFrameMs == doctest::Approx(12.0));
    // Frame 11 completes fully and supersedes 10.
    acc.report(11, "Scene",   0.0, 0.020, 2, done);
    REQUIRE(acc.report(11, "Present", 0.020, 0.022, 2, done));
    CHECK(acc.latest().gpuFrameMs == doctest::Approx(22.0));
    // A *late* straggler that completes old frame 9 must NOT overwrite latest()=11.
    acc.report(9, "Scene",   0.0, 0.005, 2, done);
    CHECK(acc.report(9, "Present", 0.005, 0.006, 2, done)); // it does complete frame 9…
    CHECK(acc.latest().gpuFrameMs == doctest::Approx(22.0)); // …but latest stays frame 11
}

TEST_CASE("GpuPassAccumulator garbage-collects frames whose completions are lost")
{
    GpuPassAccumulator acc;
    IRenderer::FrameGpuStats done;
    // Frame 0 only ever reports 1 of its 3 passes (2 command buffers lost).
    acc.report(0, "Scene", 0.0, 0.010, 3, done);
    CHECK(acc.inflightCount() == 1);
    // Many later frames report; the stale frame 0 is dropped, map stays bounded.
    for (uint64_t f = 1; f <= 20; ++f)
    {
        acc.report(f, "Scene",   0.0, 0.010, 2, done);
        acc.report(f, "Present", 0.010, 0.012, 2, done);
    }
    CHECK(acc.inflightCount() == 0);   // frame 0 was GC'd, others completed
}

TEST_CASE("EngineProfiler ignores scopes opened on non-capture threads")
{
    // Regression: the JobSystem pool wraps every task in HE_PROFILE_SCOPE_N, so
    // during a capture many worker threads call beginScope on the shared scope
    // stack concurrently. Before the capture-thread restriction this raced and
    // corrupted the heap (malloc abort). The scope must record ONLY on the thread
    // that started the capture and silently drop scopes from any other thread.
    setupTempDeploy("threadsafe");
    auto& prof = EngineProfiler::instance();

    ProfSessionInfo info;
    prof.requestStart(info);
    prof.beginFrame(16.6);                 // capture starts on THIS (capture) thread
    REQUIRE(prof.isRecording());

    std::atomic<bool> go{ false };
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t)
        workers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) { /* spin until released */ }
            for (int i = 0; i < 5000; ++i) { HE_PROFILE_SCOPE_N("WorkerJob"); }
        });
    go.store(true, std::memory_order_release);
    // The capture thread records its own scopes at the same time.
    for (int i = 0; i < 100; ++i) { HE_PROFILE_SCOPE_N("MainScope"); }
    for (auto& w : workers) w.join();

    prof.endFrame();                       // no crash, balanced stack

    const ProfFrameRecord* f = prof.lastFrame();
    REQUIRE(f != nullptr);
    bool sawMain = false, sawWorker = false;
    for (const ProfScopeSample& s : f->scopes)
    {
        if (std::string(s.name) == "MainScope") sawMain = true;
        if (std::string(s.name) == "WorkerJob") sawWorker = true;
    }
    CHECK(sawMain);            // capture-thread scopes are recorded
    CHECK_FALSE(sawWorker);    // worker-thread scopes are dropped, not raced

    prof.requestStop();
    prof.beginFrame(16.6);
    std::string p;
    prof.consumeJustDumped(p);
}

TEST_CASE("EngineProfiler single-frame capture records exactly one frame, no dump")
{
    setupTempDeploy("single");
    auto& prof = EngineProfiler::instance();
    prof.setDetailedGpuCapture(false);
    REQUIRE_FALSE(prof.isRecording());

    prof.requestSingleFrameCapture();
    prof.beginFrame(16.6);                 // applies the pending single-frame start
    CHECK(prof.isRecording());             // recording for this one frame
    // Single-frame must NOT force detailed GPU (that serializes the GPU and makes the
    // captured frame slow); it respects the checkbox, which is off here.
    CHECK_FALSE(prof.detailedGpuCapture());
    { HE_PROFILE_SCOPE_N("SingleScope"); }
    prof.endFrame();

    CHECK_FALSE(prof.isRecording());       // auto-stopped after one frame
    CHECK_FALSE(prof.detailedGpuCapture()); // still off
    const ProfFrameRecord* sf = prof.singleFrame();
    REQUIRE(sf != nullptr);
    bool sawScope = false;
    for (const auto& s : sf->scopes) if (std::string(s.name) == "SingleScope") sawScope = true;
    CHECK(sawScope);

    // It must NOT have produced a dump (single-frame stays in memory only).
    std::string p;
    CHECK_FALSE(prof.consumeJustDumped(p));
}

TEST_CASE("EngineProfiler live ring captures lightweight frames when enabled")
{
    auto& prof = EngineProfiler::instance();
    prof.setLiveEnabled(true);
    REQUIRE(prof.liveEnabled());
    ProfLiveFrame lf;
    lf.deltaMs = 16.6; lf.cpuFrameMs = 9.0; lf.gpuFrameMs = 8.0; lf.draws = 5; lf.triangles = 999;
    prof.pushLive(lf);
    const std::vector<ProfLiveFrame> snap = prof.liveSnapshot();
    REQUIRE_FALSE(snap.empty());
    CHECK(snap.back().deltaMs == doctest::Approx(16.6));
    CHECK(snap.back().gpuFrameMs == doctest::Approx(8.0));
    CHECK(snap.back().draws == 5);
    prof.setLiveEnabled(false);
    CHECK_FALSE(prof.liveEnabled());
}

TEST_CASE("EngineProfiler toggle stops an active capture")
{
    setupTempDeploy("toggle");
    auto& prof = EngineProfiler::instance();
    REQUIRE_FALSE(prof.isRecording());

    ProfSessionInfo info;
    prof.requestToggle(info);            // -> start
    prof.beginFrame(16.6);
    CHECK(prof.isRecording());
    prof.endFrame();

    prof.requestToggle(info);            // -> stop
    prof.beginFrame(16.6);
    CHECK_FALSE(prof.isRecording());
    std::string p;
    prof.consumeJustDumped(p);
}
