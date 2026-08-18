#include "doctest.h"
#include <Diagnostics/ProfilerStats.h>

#include <cmath>
#include <string>
#include <vector>

// ── ProfilerStats ─────────────────────────────────────────────────────────────
// Pure value-in/value-out analysis, so it is fully testable on a machine with no
// display — which is the point: the editor draws these numbers, this file proves
// them. Every case here is hand-computable, no golden files.

using namespace HE::Prof;

TEST_CASE("Percentiles use nearest rank and report spread")
{
    // 1..100: nearest rank puts p50 at index ceil(0.50*100)-1 = 49 → 50.
    std::vector<double> v;
    for (int i = 1; i <= 100; ++i) v.push_back(static_cast<double>(i));

    const Percentiles p = computePercentiles(v);
    CHECK(p.count == 100);
    CHECK(p.min == doctest::Approx(1.0));
    CHECK(p.max == doctest::Approx(100.0));
    CHECK(p.mean == doctest::Approx(50.5));
    CHECK(p.p50 == doctest::Approx(50.0));
    CHECK(p.p95 == doctest::Approx(95.0));
    CHECK(p.p99 == doctest::Approx(99.0));
    CHECK(p.stddev > 28.0);   // uniform 1..100 → ~28.87
    CHECK(p.stddev < 29.5);

    // Empty input must not divide by zero or read past the end.
    const Percentiles e = computePercentiles({});
    CHECK(e.count == 0);
    CHECK(e.p99 == doctest::Approx(0.0));

    // Single sample: every percentile is that sample.
    const Percentiles one = computePercentiles({ 7.0 });
    CHECK(one.p50 == doctest::Approx(7.0));
    CHECK(one.p99 == doctest::Approx(7.0));
    CHECK(one.stddev == doctest::Approx(0.0));
}

TEST_CASE("Low-percentile FPS averages the worst bucket, not the p99 frame")
{
    // 99 frames at 10 ms + 1 frame at 100 ms. The slowest 1% is exactly that one
    // frame → 1% low = 1000/100 = 10 FPS. Mean frame time = 10.9 ms → ~91.7 FPS.
    std::vector<double> frames(99, 10.0);
    frames.push_back(100.0);

    const FpsLows lows = computeFpsLows(frames);
    CHECK(lows.frames == 100);
    CHECK(lows.avgFps == doctest::Approx(1000.0 / 10.9).epsilon(0.001));
    CHECK(lows.low1Fps == doctest::Approx(10.0));
    // 0.1% of 100 frames rounds to 0 → clamped to 1 frame, so it must report the
    // worst frame rather than 0 FPS from an empty bucket.
    CHECK(lows.low01Fps == doctest::Approx(10.0));

    // Non-positive samples are dropped, not turned into infinite FPS.
    const FpsLows z = computeFpsLows({ 0.0, -1.0, 20.0 });
    CHECK(z.frames == 1);
    CHECK(z.avgFps == doctest::Approx(50.0));

    CHECK(computeFpsLows({}).frames == 0);
}

TEST_CASE("Hitch detection finds spikes against the median and names the worst scope")
{
    std::vector<ProfFrameRecord> frames;
    for (int i = 0; i < 20; ++i)
    {
        ProfFrameRecord f;
        f.index   = static_cast<uint64_t>(i);
        f.deltaMs = 10.0;
        f.scopes  = { { "Render", 6.0, 0 }, { "Logic", 2.0, 0 } };
        frames.push_back(f);
    }
    // One 50 ms stall, and the cost is in Logic — not in the usually-dominant Render.
    frames[7].deltaMs = 50.0;
    frames[7].scopes  = { { "Render", 6.0, 0 }, { "Logic", 41.0, 0 } };
    frames[7].gpuFrameMs = 9.0;

    const std::vector<Hitch> h = findHitches(frames, 2.0);
    REQUIRE(h.size() == 1);
    CHECK(h[0].frameIndex == 7);
    CHECK(h[0].deltaMs == doctest::Approx(50.0));
    CHECK(h[0].ratio == doctest::Approx(5.0));
    CHECK(std::string(h[0].worstScope) == "Logic");
    CHECK(h[0].worstScopeMs == doctest::Approx(41.0));
    CHECK(h[0].gpuMs == doctest::Approx(9.0));   // GPU was fine → a CPU stall

    // A perfectly even capture has no hitches at all.
    for (auto& f : frames) { f.deltaMs = 10.0; }
    CHECK(findHitches(frames, 2.0).empty());

    // Pathological capture: everything is a hitch → worst-first, capped, not 20k rows.
    std::vector<ProfFrameRecord> spiky;
    for (int i = 0; i < 300; ++i)
    {
        ProfFrameRecord f;
        f.index   = static_cast<uint64_t>(i);
        f.deltaMs = (i % 2 == 0) ? 5.0 : 100.0 + i;   // median 5-ish, half are spikes
        spiky.push_back(f);
    }
    const std::vector<Hitch> capped = findHitches(spiky, 2.0, 10);
    CHECK(capped.size() == 10);
    for (size_t i = 1; i < capped.size(); ++i)
        CHECK(capped[i - 1].deltaMs >= capped[i].deltaMs);   // worst first
}

TEST_CASE("Self time subtracts direct children only, never grandchildren twice")
{
    // Tree:  A(10) { B(6) { C(4) }, D(2) }
    // Recorded in CLOSE order with depths, exactly as EngineProfiler stores it.
    //   self(A) = 10 - (6 + 2) = 2
    //   self(B) =  6 - 4       = 2      ← C must NOT also be subtracted from A
    //   self(C) =  4
    //   self(D) =  2
    ProfFrameRecord f;
    f.deltaMs    = 16.0;
    f.cpuFrameMs = 10.0;
    f.scopes = {
        { "C", 4.0, 2 },
        { "B", 6.0, 1 },
        { "D", 2.0, 1 },
        { "A", 10.0, 0 },
    };

    const std::vector<ScopeAggregate> agg = aggregateScopes({ f });
    REQUIRE(agg.size() == 4);

    auto find = [&](const std::string& n) {
        for (const ScopeAggregate& a : agg) if (a.name == n) return a;
        FAIL("scope not found: ", n);
        return ScopeAggregate{};
    };
    CHECK(find("A").selfMs == doctest::Approx(2.0));
    CHECK(find("B").selfMs == doctest::Approx(2.0));
    CHECK(find("C").selfMs == doctest::Approx(4.0));
    CHECK(find("D").selfMs == doctest::Approx(2.0));
    CHECK(find("A").totalMs == doctest::Approx(10.0));
    CHECK(find("A").minDepth == 0);
    CHECK(find("C").minDepth == 2);

    // Sorted by self time descending → C (4 ms) leads, which is the row that
    // actually points at where the frame went.
    CHECK(agg.front().name == "C");
    for (size_t i = 1; i < agg.size(); ++i)
        CHECK(agg[i - 1].selfMs >= agg[i].selfMs);
}

TEST_CASE("Self time handles siblings, repeats and timer noise")
{
    // Two independent top-level subtrees in one frame; the second scope repeats,
    // so count/avg must accumulate across BOTH occurrences and both frames.
    ProfFrameRecord f;
    f.scopes = {
        { "Child", 1.0, 1 },
        { "Parent", 3.0, 0 },
        { "Child", 2.0, 1 },
        { "Other", 5.0, 0 },
    };
    const std::vector<ScopeAggregate> agg = aggregateScopes({ f, f });
    auto find = [&](const std::string& n) {
        for (const ScopeAggregate& a : agg) if (a.name == n) return a;
        FAIL("scope not found: ", n);
        return ScopeAggregate{};
    };
    // "Child" 1.0 belongs to Parent; "Child" 2.0 belongs to Other. Neither parent
    // may claim the other's child.
    CHECK(find("Parent").selfMs == doctest::Approx(2.0 * (3.0 - 1.0)));
    CHECK(find("Other").selfMs  == doctest::Approx(2.0 * (5.0 - 2.0)));
    CHECK(find("Child").count == 4);
    CHECK(find("Child").minMs == doctest::Approx(1.0));
    CHECK(find("Child").maxMs == doctest::Approx(2.0));
    CHECK(find("Child").avgMs == doctest::Approx(1.5));

    // Timer noise: a child measured a hair longer than its parent must not produce
    // a negative self time (that reads as a profiler bug, not a frame problem).
    ProfFrameRecord noisy;
    noisy.scopes = { { "Inner", 1.0001, 1 }, { "Outer", 1.0, 0 } };
    const std::vector<ScopeAggregate> n = aggregateScopes({ noisy });
    for (const ScopeAggregate& a : n) CHECK(a.selfMs >= 0.0);
}

TEST_CASE("Histogram bins equal width and puts the max value in the last bin")
{
    // 0..10 in 10 bins of width 1. The value 10 lands exactly on the top edge and
    // must fall into the LAST bin, not one past the end.
    std::vector<double> v;
    for (int i = 0; i <= 10; ++i) v.push_back(static_cast<double>(i));

    const Histogram h = computeHistogram(v, 10);
    REQUIRE(h.bins.size() == 10);
    CHECK(h.lo == doctest::Approx(0.0));
    CHECK(h.hi == doctest::Approx(10.0));
    CHECK(h.binWidth == doctest::Approx(1.0));
    CHECK(h.binStart(0) == doctest::Approx(0.0));
    CHECK(h.binStart(9) == doctest::Approx(9.0));
    CHECK(h.bins[9] == 2);          // 9 and 10 both land here
    CHECK(h.maxCount == 2);
    uint32_t total = 0;
    for (uint32_t c : h.bins) total += c;
    CHECK(total == 11);             // nothing lost off either end

    // Degenerate range: every value identical → one populated bin, no divide by zero.
    const Histogram flat = computeHistogram({ 5.0, 5.0, 5.0 }, 8);
    CHECK(flat.bins.size() == 8);
    CHECK(flat.bins[0] == 3);
    CHECK(flat.maxCount == 3);
    CHECK(flat.binWidth == doctest::Approx(0.0));

    CHECK(computeHistogram({}, 8).bins.empty());
    CHECK(computeHistogram({ 1.0, 2.0 }, 0).bins.empty());
}

TEST_CASE("Lane occupancy counts depth-0 spans only")
{
    // A worker busy 4 ms out of a 20 ms capture is at 20% — but its spans are
    // nested three deep, and summing every depth would claim well over 100%.
    ProfThreadTimeline main;
    main.label  = "Main";
    main.isMain = true;
    main.spans  = {
        { "Frame", 0, 8'000'000, 0 },          // 8 ms, depth 0
        { "Inner", 1'000'000, 5'000'000, 1 },  // 4 ms, nested → must NOT add
        { "Deep",  2'000'000, 3'000'000, 2 },
    };
    ProfThreadTimeline worker;
    worker.label = "Worker 0";
    worker.spans = {
        { "Job", 1'000'000, 3'000'000, 0 },    // 2 ms
        { "Job", 5'000'000, 7'000'000, 0 },    // 2 ms
    };

    const std::vector<LaneOccupancy> occ = laneOccupancy({ main, worker }, 20.0);
    REQUIRE(occ.size() == 2);

    CHECK(occ[0].isMain);
    CHECK(occ[0].busyMs == doctest::Approx(8.0));        // not 8+4+1
    CHECK(occ[0].utilisation == doctest::Approx(0.4));
    CHECK(occ[0].maxDepth == 2);
    CHECK(occ[0].spanMs == doctest::Approx(8.0));        // first start → last end

    CHECK(occ[1].busyMs == doctest::Approx(4.0));
    CHECK(occ[1].utilisation == doctest::Approx(0.2));   // the idle-worker verdict
    CHECK(occ[1].spanCount == 2);
    CHECK(occ[1].spanMs == doctest::Approx(6.0));        // 1 ms → 7 ms, gaps included

    // A zero-length capture must not divide by zero.
    const std::vector<LaneOccupancy> zero = laneOccupancy({ main }, 0.0);
    CHECK(zero[0].utilisation == doctest::Approx(0.0));
}
