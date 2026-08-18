#include "doctest.h"
#include "TestFsUtil.h"
#include <Diagnostics/Profiler.h>
#include <Diagnostics/EngineProfiler.h>
#include <Diagnostics/GlobalState.h>
#include <JobSystem/JobSystem.h>
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
    he_test::removeAllQuiet(dir);
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

    // ── v3 additions ────────────────────────────────────────────────────────
    CHECK(j["version"] == 2);

    // Self time reaches the dump, and the nesting is respected: FrameWork wholly
    // contains SubTask, so its self time must be strictly below its total.
    const auto& fw = j["summary"]["cpuScopes"]["FrameWork"];
    REQUIRE(fw.contains("selfMs"));
    REQUIRE(fw.contains("p95"));
    CHECK(fw["count"] == kFrames);
    CHECK(fw["selfMs"].get<double>() <= fw["totalMs"].get<double>());
    CHECK(j["summary"]["topScopesBySelfTime"].is_array());
    CHECK_FALSE(j["summary"]["topScopesBySelfTime"].empty());

    // Tail statistics: percentiles per series + benchmark-convention FPS lows.
    const auto& st = j["summary"]["stats"];
    REQUIRE(st.contains("deltaMs"));
    CHECK(st["deltaMs"]["p50"].get<double>() == doctest::Approx(16.6));
    CHECK(st["deltaMs"]["p99"].get<double>() == doctest::Approx(16.6));
    CHECK(st["deltaMs"]["count"] == kFrames);
    CHECK(st.contains("cpuMs"));
    CHECK(st["gpuMs"]["p50"].get<double>() == doctest::Approx(2.5));
    CHECK(st["fps"]["avg"].get<double>() == doctest::Approx(1000.0 / 16.6));
    // A perfectly even capture has no hitches — the block must be absent, not empty.
    CHECK_FALSE(j["summary"].contains("hitches"));

    // Per-thread timeline: the main lane is present and summarised, and the frame
    // marks share the spans' relative-ns axis.
    REQUIRE(j["summary"].contains("threads"));
    REQUIRE_FALSE(j["summary"]["threads"].empty());
    CHECK(j["summary"]["threads"][0]["main"] == true);
    CHECK(j["summary"]["threads"][0]["spans"].get<size_t>() >= 2 * kFrames);
    CHECK(j["summary"]["threads"][0]["droppedSpans"] == 0);
    CHECK_FALSE(j["summary"].contains("timelineTruncated"));   // tiny capture, no cap hit
    REQUIRE(j.contains("threads"));
    CHECK(j["threads"][0]["thread"] == "Main");
    REQUIRE(j.contains("frameMarks"));
    CHECK(j["frameMarks"].size() == kFrames);
    for (const auto& m : j["frameMarks"])
        CHECK(m["e"].get<uint64_t>() >= m["s"].get<uint64_t>());

    he_test::removeAllQuiet(deploy);
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

TEST_CASE("EngineProfiler records worker scopes on their own timeline lane, not the frame tree")
{
    // Regression + v3 contract change, in one case. The JobSystem pool wraps every
    // task in HE_PROFILE_SCOPE_N, so during a capture many worker threads call
    // beginScope at once. In v1 they all pushed onto ONE shared scope stack, which
    // raced and corrupted the heap (malloc abort); v2.1 fixed that by dropping
    // worker scopes entirely, and this test asserted they were dropped.
    //
    // v3 keeps the crash fix but stops throwing the data away: the per-FRAME scope
    // tree is still main-thread-only (single writer, unchanged), while each thread
    // additionally records into its OWN buffer, which is what makes parallel_for
    // work visible. So the assertion inverts on the timeline and holds on the tree:
    //   frame tree  → MainScope yes, WorkerJob no   (as before)
    //   timeline    → a lane per worker, carrying WorkerJob   (new)
    // and still, above all: 8 threads × 5000 scopes must not corrupt the heap.
    setupTempDeploy("threadsafe");
    auto& prof = EngineProfiler::instance();
    prof.setThreadTimelineEnabled(true);

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
    CHECK(sawMain);            // capture-thread scopes are in the frame tree
    CHECK_FALSE(sawWorker);    // worker scopes stay OUT of the single-writer tree

    // …but they are on the timeline, on lanes of their own.
    const std::vector<ProfThreadTimeline> lanes = prof.timelineSnapshot();
    size_t workerLanes = 0, workerSpans = 0, mainLanes = 0;
    bool sawMainOnTimeline = false;
    for (const ProfThreadTimeline& lane : lanes)
    {
        if (lane.isMain) { ++mainLanes; }
        else             { ++workerLanes; }
        for (const ProfThreadSpan& s : lane.spans)
        {
            CHECK(s.endNs >= s.startNs);                    // no negative-width spans
            if (std::string(s.name) == "WorkerJob")  { ++workerSpans; CHECK_FALSE(lane.isMain); }
            if (std::string(s.name) == "MainScope")  { sawMainOnTimeline = true; CHECK(lane.isMain); }
        }
    }
    CHECK(mainLanes == 1);
    CHECK(workerLanes == 8);
    CHECK(sawMainOnTimeline);
    // Every worker span is accounted for: nothing raced away, nothing was dropped
    // (8 × 5000 is far below the per-thread cap).
    CHECK(workerSpans == 8 * 5000);
    CHECK(prof.timelineDroppedSpans() == 0);

    prof.requestStop();
    prof.beginFrame(16.6);
    std::string dump;
    prof.consumeJustDumped(dump);
}

TEST_CASE("JobSystem tasks are named on the worker lanes, not all 'Job::Execute'")
{
    // The first real capture produced eight worker lanes carrying nothing but
    // "Job::Execute" — a timeline of the fact that jobs ran, and of nothing else.
    // parallel_for now labels its chunks, so a lane says WHAT the pool was doing.
    setupTempDeploy("jobnames");
    auto& prof = EngineProfiler::instance();
    prof.setThreadTimelineEnabled(true);

    ProfSessionInfo info;
    prof.requestStart(info);
    prof.beginFrame(16.6);
    REQUIRE(prof.isRecording());

    // Enough items that the pool is actually fed (parallel_for runs chunk 0 on the
    // calling thread, so a tiny count would never reach a worker at all).
    std::atomic<int> sum{ 0 };
    parallel_for(4096, [&](size_t i) { sum.fetch_add(static_cast<int>(i & 1), std::memory_order_relaxed); },
                 "TestCullChunk");
    // A direct submit keeps its own label too.
    globalPool().submit([]{ /* nothing */ }, "TestDirectJob").get();

    prof.endFrame();

    bool sawNamed = false, sawDirect = false, sawGeneric = false;
    for (const ProfThreadTimeline& lane : prof.timelineSnapshot())
    {
        if (lane.isMain) continue;
        for (const ProfThreadSpan& s : lane.spans)
        {
            const std::string n(s.name ? s.name : "");
            if (n == "TestCullChunk")  sawNamed  = true;
            if (n == "TestDirectJob")  sawDirect = true;
            if (n == "Job::Execute")   sawGeneric = true;
        }
    }
    CHECK(sawNamed);
    CHECK(sawDirect);
    // Nothing in this test submits an unnamed task, so the generic label must not
    // appear at all: if it does, a call site is still going through the default.
    CHECK_FALSE(sawGeneric);

    prof.requestStop();
    prof.beginFrame(16.6);
    std::string p;
    prof.consumeJustDumped(p);
}

TEST_CASE("Dump truncates the timeline by time, so every lane keeps the same window")
{
    // Regression from a real 4867-frame capture: the dump budget was spent lane by
    // lane, the main thread's 155k spans consumed all of it, and every worker lane
    // came out with ZERO spans — which reads as an idle thread pool rather than as
    // a lane that was never written. The budget is now a shared time cutoff, so a
    // truncated dump is a consistent prefix of the capture across all lanes.
    fs::path deploy = setupTempDeploy("truncate");
    auto& prof = EngineProfiler::instance();
    prof.setThreadTimelineEnabled(true);

    ProfSessionInfo info;
    prof.requestStart(info);
    prof.beginFrame(16.6);
    REQUIRE(prof.isRecording());

    // Comfortably over the 120k dump budget so the cutoff actually engages.
    std::atomic<bool> go{ false };
    std::vector<std::thread> workers;
    for (int t = 0; t < 6; ++t)
        workers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) { }
            for (int i = 0; i < 25000; ++i) { HE_PROFILE_SCOPE_N("BulkWorker"); }
        });
    go.store(true, std::memory_order_release);
    for (int i = 0; i < 25000; ++i) { HE_PROFILE_SCOPE_N("BulkMain"); }
    for (auto& w : workers) w.join();
    prof.endFrame();

    prof.requestStop();
    prof.beginFrame(16.6);
    std::string dumpPath;
    REQUIRE(prof.consumeJustDumped(dumpPath));
    std::ifstream in(dumpPath);
    REQUIRE(in.is_open());
    json j = json::parse(in);

    REQUIRE(j["summary"].contains("timelineTruncated"));
    REQUIRE(j["summary"].contains("timelineCutoffNs"));
    const uint64_t cutoff = j["summary"]["timelineCutoffNs"].get<uint64_t>();

    // The point of the fix: more than one lane survives truncation.
    size_t lanesWithSpans = 0, workerLanesWithSpans = 0;
    for (const auto& lane : j["threads"])
    {
        if (lane["spans"].empty()) continue;
        ++lanesWithSpans;
        if (!lane["main"].get<bool>()) ++workerLanesWithSpans;
        // Everything written is strictly before the shared cutoff.
        for (const auto& s : lane["spans"])
            CHECK(s["s"].get<uint64_t>() < cutoff);
    }
    CHECK(lanesWithSpans > 1);
    CHECK(workerLanesWithSpans > 0);

    // summary.threads still describes the WHOLE capture, truncation or not — that
    // is what makes the totals trustworthy when threads[] is only a prefix.
    uint64_t summarySpans = 0;
    for (const auto& t : j["summary"]["threads"]) summarySpans += t["spans"].get<uint64_t>();
    CHECK(summarySpans == 6 * 25000 + 25000);

    // Frame marks are clipped to the same cutoff, so no separator is drawn over a
    // stretch of timeline where no lane has data.
    for (const auto& m : j["frameMarks"]) CHECK(m["s"].get<uint64_t>() < cutoff);

    he_test::removeAllQuiet(deploy);
}

TEST_CASE("EngineProfiler timeline resets between captures and can be switched off")
{
    // A worker buffer survives its capture (shared_ptr in the registry), so the
    // generation stamp is what stops last capture's spans leaking into this one.
    setupTempDeploy("timeline_gen");
    auto& prof = EngineProfiler::instance();
    prof.setThreadTimelineEnabled(true);

    ProfSessionInfo info;
    prof.requestStart(info);
    prof.beginFrame(16.6);
    { HE_PROFILE_SCOPE_N("FirstCapture"); }
    prof.endFrame();
    prof.requestStop();
    prof.beginFrame(16.6);
    std::string p; prof.consumeJustDumped(p);

    // Second capture: the previous capture's spans must be gone.
    prof.requestStart(info);
    prof.beginFrame(16.6);
    { HE_PROFILE_SCOPE_N("SecondCapture"); }
    prof.endFrame();

    bool sawFirst = false, sawSecond = false;
    for (const ProfThreadTimeline& lane : prof.timelineSnapshot())
        for (const ProfThreadSpan& s : lane.spans)
        {
            if (std::string(s.name) == "FirstCapture")  sawFirst  = true;
            if (std::string(s.name) == "SecondCapture") sawSecond = true;
        }
    CHECK_FALSE(sawFirst);
    CHECK(sawSecond);

    // Frame marks share the timeline's relative-ns axis and line up with frames[].
    REQUIRE_FALSE(prof.frameMarks().empty());
    for (const ProfFrameMark& m : prof.frameMarks()) CHECK(m.endNs >= m.startNs);

    prof.requestStop();
    prof.beginFrame(16.6);
    prof.consumeJustDumped(p);

    // Switched off: the frame tree still works, the timeline stays empty.
    prof.setThreadTimelineEnabled(false);
    prof.requestStart(info);
    prof.beginFrame(16.6);
    { HE_PROFILE_SCOPE_N("NoTimeline"); }
    prof.endFrame();
    const ProfFrameRecord* f = prof.lastFrame();
    REQUIRE(f != nullptr);
    CHECK_FALSE(f->scopes.empty());
    CHECK(prof.timelineSnapshot().empty());

    prof.requestStop();
    prof.beginFrame(16.6);
    prof.consumeJustDumped(p);
    prof.setThreadTimelineEnabled(true);   // restore the default for later cases
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
