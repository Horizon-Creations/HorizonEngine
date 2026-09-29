#include "doctest.h"
#include <JobSystem/JobSystem.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("parallel_for runs the task for every index exactly once")
{
    const size_t N = 64;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0);

    parallel_for(N, [&](size_t i) { hits[i].fetch_add(1); });

    for (size_t i = 0; i < N; ++i)
        CHECK(hits[i].load() == 1);
}

TEST_CASE("parallel_for handles count == 0 without invoking f")
{
    bool ran = false;
    parallel_for(0, [&](size_t) { ran = true; });
    CHECK_FALSE(ran);
}

TEST_CASE("parallel_for handles count == 1 inline (no pool submission)")
{
    size_t seen = SIZE_MAX;
    parallel_for(1, [&](size_t i) { seen = i; });
    CHECK(seen == 0);
}

TEST_CASE("parallel_for produces the correct sum")
{
    const size_t N = 100;
    std::atomic<long long> sum{0};
    parallel_for(N, [&](size_t i) { sum.fetch_add(static_cast<long long>(i)); });
    // 0 + 1 + … + 99 = 4950
    CHECK(sum.load() == 4950LL);
}

TEST_CASE("ThreadPool executes submitted tasks")
{
    ThreadPool pool(2);
    std::atomic<int> total{0};
    std::vector<std::future<void>> futs;
    for (int i = 0; i < 10; ++i)
        futs.push_back(pool.submit([&total, i]{ total.fetch_add(i); }));
    for (auto& f : futs) f.get();
    CHECK(total.load() == 45); // 0+1+…+9
}

TEST_CASE("globalPool returns the same instance on every call")
{
    ThreadPool& a = globalPool();
    ThreadPool& b = globalPool();
    CHECK(&a == &b);
}

TEST_CASE("globalPool has at least one thread")
{
    CHECK(globalPool().threadCount() >= 1);
}

// ─── Minimum grain + caller helping (perf audit step 3, B2) ──────────────────

namespace {

// Occupies every worker of globalPool() with a task that waits on a gate, and
// releases them on destruction — also when a CHECK fails or something throws, so
// a broken test cannot leave the process-wide pool the rest of the suite uses
// wedged forever.
struct SaturatedPool
{
    std::mutex              m;
    std::condition_variable cv;
    bool                    open = false;
    std::atomic<size_t>     parked{ 0 };
    std::vector<std::future<void>> blockers;

    SaturatedPool()
    {
        ThreadPool& pool = globalPool();
        for (size_t i = 0; i < pool.threadCount(); ++i)
            blockers.push_back(pool.submit([this] {
                parked.fetch_add(1);
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [this] { return open; });
            }, "TestBlocker"));
        // Every worker is inside a blocker only once all of them have checked in.
        while (parked.load() < pool.threadCount())
            std::this_thread::yield();
    }
    ~SaturatedPool()
    {
        { std::lock_guard<std::mutex> lock(m); open = true; }
        cv.notify_all();
        for (auto& b : blockers) b.get();
    }
};

} // namespace

TEST_CASE("parallel_for_chunks: every chunk holds at least minGrain indices")
{
    // Below two grains' worth there is nothing to split: run inline.
    CHECK(parallel_for_chunks(0,    256, 10) == 1);
    CHECK(parallel_for_chunks(4,    256, 10) == 1);   // the audit's 4 objects: no fan-out
    CHECK(parallel_for_chunks(511,  256, 10) == 1);
    CHECK(parallel_for_chunks(512,  256, 10) == 2);
    CHECK(parallel_for_chunks(1000, 256, 10) == 3);   // floor, never a short chunk
    // Capped at 4 × (workers + 1) however small the grain.
    CHECK(parallel_for_chunks(768,  1,   10) == 44);
    CHECK(parallel_for_chunks(1u << 20, 256, 10) == 44);
    CHECK(parallel_for_chunks(1u << 20, 256, 1) == 8);
    // A grain of 0 means 1, and a 0-worker report still means one worker.
    CHECK(parallel_for_chunks(3, 0, 10) == 3);
    CHECK(parallel_for_chunks(100, 1, 0) == 8);
}

TEST_CASE("parallel_for below the minimum grain never leaves the calling thread")
{
    const std::thread::id caller = std::this_thread::get_id();
    std::vector<std::thread::id> ran(2 * kParallelForMinGrain - 1);
    parallel_for(ran.size(), [&](size_t i) { ran[i] = std::this_thread::get_id(); });
    for (size_t i = 0; i < ran.size(); ++i)
        CHECK(ran[i] == caller);

    // An explicit grain works the same way.
    std::vector<std::thread::id> ran2(1000);
    parallel_for(ran2.size(), [&](size_t i) { ran2[i] = std::this_thread::get_id(); },
                 "TestGrain", 600);
    for (size_t i = 0; i < ran2.size(); ++i)
        CHECK(ran2[i] == caller);
}

TEST_CASE("parallel_for: caller does all the work when every worker is busy")
{
    // The discriminating case for helping. With every pool thread parked, the old
    // parallel_for ran chunk 0 and then slept in future::get() on chunks no worker
    // could ever start — a deadlock here, and under system load the 8.7 ms p50
    // stall the audit measured. Now the caller claims the chunks itself.
    SaturatedPool busy;

    const std::thread::id caller = std::this_thread::get_id();
    const size_t N = 8 * kParallelForMinGrain;
    std::vector<std::atomic<int>>  hits(N);
    std::vector<std::thread::id>   who(N);
    for (auto& h : hits) h.store(0);

    parallel_for(N, [&](size_t i) {
        hits[i].fetch_add(1);
        who[i] = std::this_thread::get_id();
    }, "TestHelping", 16);

    for (size_t i = 0; i < N; ++i)
    {
        CHECK(hits[i].load() == 1);
        CHECK(who[i] == caller);
    }
    // The helper tasks are still queued behind the blockers; when they run they
    // must find nothing left to claim — the next test checks exactly that.
}

TEST_CASE("parallel_for: helpers that dequeue after the call returned touch nothing")
{
    const size_t N = 4 * kParallelForMinGrain;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0);
    {
        SaturatedPool busy;
        parallel_for(N, [&](size_t i) { hits[i].fetch_add(1); }, "TestLateHelper", 16);
    }   // blockers released: the stale helpers now run against a finished job
    // Drain: the queue is FIFO, and once EVERY worker sits in a blocker queued
    // after the stale helpers, each of those helpers has not just started but
    // finished — so the check below sees their full effect, if any.
    { SaturatedPool drain; }
    for (size_t i = 0; i < N; ++i)
        CHECK(hits[i].load() == 1);
}

TEST_CASE("parallel_for spreads large work over the pool and still hits every index once")
{
    const size_t N = 64 * kParallelForMinGrain;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits) h.store(0);
    std::atomic<long long> sum{ 0 };
    parallel_for(N, [&](size_t i) {
        hits[i].fetch_add(1);
        sum.fetch_add(static_cast<long long>(i));
    }, "TestLarge", 1);
    for (size_t i = 0; i < N; ++i)
        CHECK(hits[i].load() == 1);
    CHECK(sum.load() == static_cast<long long>(N) * (N - 1) / 2);
}

TEST_CASE("parallel_for rethrows a body exception on the caller and the pool survives")
{
    const size_t N = 16 * kParallelForMinGrain;
    std::atomic<int> running{ 0 }, maxAfterThrow{ 0 };
    bool caught = false;
    try
    {
        parallel_for(N, [&](size_t i) {
            running.fetch_add(1);
            if (i == N / 2) { running.fetch_sub(1); throw std::runtime_error("boom"); }
            running.fetch_sub(1);
        }, "TestThrow", 1);
    }
    catch (const std::runtime_error& e)
    {
        caught = std::string(e.what()) == "boom";
        // No chunk may still be running once the exception reached the caller.
        maxAfterThrow.store(running.load());
    }
    CHECK(caught);
    CHECK(maxAfterThrow.load() == 0);

    // The pool is still usable afterwards.
    std::atomic<int> after{ 0 };
    parallel_for(N, [&](size_t) { after.fetch_add(1); }, "TestAfterThrow", 1);
    CHECK(after.load() == static_cast<int>(N));
}

TEST_CASE("parallel_for nested inside a job does not deadlock a saturated pool")
{
    // Every worker runs an outer body that itself calls parallel_for. With the old
    // futures that could exhaust the pool and wait on itself; claiming lets each
    // outer body finish its inner loop alone.
    ThreadPool& pool = globalPool();
    const size_t outer = pool.threadCount() + 1;
    const size_t inner = 4 * kParallelForMinGrain;
    std::atomic<size_t> total{ 0 };
    parallel_for(outer, [&](size_t) {
        parallel_for(inner, [&](size_t) { total.fetch_add(1); }, "TestInner", 16);
    }, "TestOuter", 1);
    CHECK(total.load() == outer * inner);
}

TEST_CASE("ThreadPool::post runs every copy it enqueued")
{
    std::atomic<int> ran{ 0 };
    std::mutex m;
    std::condition_variable cv;
    ThreadPool pool(3);   // declared last: joins its workers before m/cv die
    pool.post([&] {
        ran.fetch_add(1);
        std::lock_guard<std::mutex> lock(m);
        cv.notify_all();
    }, "TestPost", 7);
    std::unique_lock<std::mutex> lock(m);
    CHECK(cv.wait_for(lock, std::chrono::seconds(10), [&] { return ran.load() == 7; }));
}
