#include "doctest.h"
#include <JobSystem/JobSystem.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
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
    // Drain: the helpers are High and the drain blockers Normal, so the helpers
    // leave the queue first; once EVERY worker sits in a blocker, each of those
    // helpers has not just started but finished — so the check below sees their
    // full effect, if any.
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

// ─── Job planning: priorities, dependencies, cancellation (Thema 153, Schritt 2) ─

namespace {

using HE::CancelToken;
using HE::JobDesc;
using HE::JobHandle;
using HE::JobPriority;
using HE::JobStatus;

// A door the test opens: jobs that wait on it hold their worker until then.
struct Gate
{
    std::mutex              m;
    std::condition_variable cv;
    bool                    open = false;
    void wait()
    {
        std::unique_lock<std::mutex> lock(m);
        cv.wait(lock, [this] { return open; });
    }
    void release()
    {
        { std::lock_guard<std::mutex> lock(m); open = true; }
        cv.notify_all();
    }
};

// Poll until pred() holds or the deadline passes. Every wait in these tests goes
// through a deadline: a scheduling bug must turn the suite red, not freeze CI.
template<typename P>
bool eventually(P pred, std::chrono::milliseconds limit = std::chrono::seconds(10))
{
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

// Run fn on a thread of its own and report whether it returned in time. A stuck
// fn is leaked rather than joined — joining it would hang the suite, which is
// precisely what a deadlock test must not do.
template<typename F>
bool returnsWithin(F fn, std::chrono::milliseconds limit = std::chrono::seconds(10))
{
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> f = done->get_future();
    std::thread([fn = std::move(fn), done]() mutable {
        try { fn(); done->set_value(); }
        catch (...) { done->set_exception(std::current_exception()); }
    }).detach();
    if (f.wait_for(limit) != std::future_status::ready) return false;
    f.get();
    return true;
}

// Parks the single worker of a one-thread pool, so everything scheduled after it
// stays queued until open() — the deterministic way to look at queue order.
struct ParkedWorker
{
    Gate              gate;
    std::atomic<bool> parked{ false };
    std::future<void> blocker;
    explicit ParkedWorker(ThreadPool& pool)
    {
        blocker = pool.submit([this] { parked.store(true); gate.wait(); }, "TestParked");
        while (!parked.load()) std::this_thread::yield();
    }
    void open()
    {
        gate.release();
        blocker.get();
    }
    ~ParkedWorker() { if (blocker.valid()) open(); }
};

JobDesc desc(const char* name, JobPriority p = JobPriority::Normal)
{
    JobDesc d;
    d.name     = name;
    d.priority = p;
    return d;
}

JobDesc after(const char* name, std::vector<JobHandle> deps, JobPriority p = JobPriority::Normal)
{
    JobDesc d = desc(name, p);
    d.after   = std::move(deps);
    return d;
}

} // namespace

TEST_CASE("schedule: a higher priority overtakes, one priority stays FIFO")
{
    std::mutex       om;
    std::vector<int> order;
    auto record = [&](int v) { return [&om, &order, v] { std::lock_guard<std::mutex> l(om); order.push_back(v); }; };
    ThreadPool pool(1);   // declared after what its jobs touch: joins first
    ParkedWorker park(pool);

    std::vector<JobHandle> hs;
    hs.push_back(pool.schedule(record(30), desc("TestL1", JobPriority::Low)));
    hs.push_back(pool.schedule(record(20), desc("TestN1", JobPriority::Normal)));
    hs.push_back(pool.schedule(record(10), desc("TestH1", JobPriority::High)));
    hs.push_back(pool.schedule(record(31), desc("TestL2", JobPriority::Low)));
    hs.push_back(pool.schedule(record(11), desc("TestH2", JobPriority::High)));
    pool.post(record(21), "TestN2");                                  // post() shares Normal
    auto lowSubmit = pool.submit(record(32), "TestL3", JobPriority::Low);   // and so can submit()

    CHECK(pool.queuedCount(JobPriority::High) == 2);
    CHECK(pool.queuedCount(JobPriority::Normal) == 2);
    CHECK(pool.queuedCount(JobPriority::Low) == 3);
    CHECK(hs[0].status() == JobStatus::Queued);

    park.open();
    // Not JobHandle::wait(): that would run a still-queued job right here and
    // spoil the very order under test.
    REQUIRE(eventually([&] { std::lock_guard<std::mutex> l(om); return order.size() == 7; }));
    CHECK(order == std::vector<int>{ 10, 11, 20, 21, 30, 31, 32 });
    for (auto& h : hs) CHECK(h.status() == JobStatus::Done);
    lowSubmit.get();
}

TEST_CASE("Low jobs never hold more workers than the cap, and High still gets through")
{
    Gate gate;
    std::atomic<int> lowRunning{ 0 }, lowPeak{ 0 }, lowDone{ 0 };
    std::atomic<bool> highRan{ false };
    ThreadPool pool(4);
    // Default cap for Low: two workers stay free for frame work.
    REQUIRE(pool.concurrencyLimit(JobPriority::Low) == 2);
    CHECK(pool.concurrencyLimit(JobPriority::High) == 4);

    std::vector<JobHandle> lows;
    for (int i = 0; i < 6; ++i)
        lows.push_back(pool.schedule([&] {
            const int n = lowRunning.fetch_add(1) + 1;
            int peak = lowPeak.load();
            while (n > peak && !lowPeak.compare_exchange_weak(peak, n)) {}
            gate.wait();
            lowRunning.fetch_sub(1);
            lowDone.fetch_add(1);
        }, desc("TestLowCapped", JobPriority::Low)));

    REQUIRE(eventually([&] { return lowRunning.load() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(lowRunning.load() == 2);                       // nobody else took a third
    CHECK(pool.queuedCount(JobPriority::Low) == 4);

    // The point of the cap: with every Low worker blocked in a "pak read", a
    // High job is still picked up by a free worker instead of waiting it out.
    pool.schedule([&] { highRan.store(true); }, desc("TestHighThrough", JobPriority::High));
    CHECK(eventually([&] { return highRan.load(); }));

    gate.release();
    REQUIRE(eventually([&] { return lowDone.load() == 6; }));
    CHECK(lowPeak.load() == 2);

    // A raised cap is honoured for what comes next.
    pool.setConcurrencyLimit(JobPriority::Low, 4);
    CHECK(pool.concurrencyLimit(JobPriority::Low) == 4);
    pool.setConcurrencyLimit(JobPriority::Low, 0);       // 0 means 1, never "nothing runs"
    CHECK(pool.concurrencyLimit(JobPriority::Low) == 1);
}

TEST_CASE("schedule: a job starts only after everything it depends on is done")
{
    Gate gateA;
    std::mutex om;
    std::vector<char> order;
    std::atomic<bool> bStarted{ false };
    auto rec = [&](char c) { std::lock_guard<std::mutex> l(om); order.push_back(c); };
    ThreadPool pool(4);

    //      A
    //     / \
    //    B   C        D after B and C; B is High and still waits for A.
    //     \ /
    //      D
    JobHandle a = pool.schedule([&] { gateA.wait(); rec('A'); }, desc("TestDepA"));
    JobHandle b = pool.schedule([&] { bStarted.store(true); rec('B'); },
                                after("TestDepB", { a }, JobPriority::High));
    JobHandle c = pool.schedule([&] { rec('C'); }, after("TestDepC", { a }));
    JobHandle d = pool.schedule([&] { rec('D'); }, after("TestDepD", { b, c }, JobPriority::Low));

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(bStarted.load());
    CHECK(b.status() == JobStatus::Waiting);
    CHECK(d.status() == JobStatus::Waiting);
    // Parked, not queued: a waiting job must not sit in a queue and get re-tried.
    CHECK(pool.queuedCount(JobPriority::High) == 0);

    gateA.release();
    REQUIRE(returnsWithin([d] { d.wait(); }));
    CHECK(d.status() == JobStatus::Done);
    std::lock_guard<std::mutex> l(om);
    REQUIRE(order.size() == 4);
    CHECK(order.front() == 'A');
    CHECK(order.back() == 'D');

    // A dependency that has long finished is no obstacle.
    std::atomic<bool> late{ false };
    JobHandle e = pool.schedule([&] { late.store(true); }, after("TestDepLate", { a, JobHandle{} }));
    REQUIRE(returnsWithin([e] { e.wait(); }));
    CHECK(late.load());
}

TEST_CASE("cancel(): a queued job never runs, and its dependents are cancelled with it")
{
    std::atomic<int> ran{ 0 }, cancelledCalls{ 0 }, depRan{ 0 };
    ThreadPool pool(1);
    ParkedWorker park(pool);

    JobDesc dx = desc("TestCancelX", JobPriority::Low);
    dx.onCancelled = [&] { cancelledCalls.fetch_add(1); };
    JobHandle x = pool.schedule([&] { ran.fetch_add(1); }, dx);
    JobDesc dy = after("TestCancelY", { x });
    dy.onCancelled = [&] { cancelledCalls.fetch_add(1); };
    JobHandle y = pool.schedule([&] { depRan.fetch_add(1); }, dy);
    CHECK(x.status() == JobStatus::Queued);
    CHECK(y.status() == JobStatus::Waiting);

    x.cancel();
    // Finished at once — not when the worker gets round to the queue entry.
    CHECK(x.status() == JobStatus::Cancelled);
    CHECK(y.status() == JobStatus::Cancelled);
    CHECK(cancelledCalls.load() == 2);
    CHECK(returnsWithin([x, y] { x.wait(); y.wait(); }, std::chrono::milliseconds(500)));

    // A job scheduled after a cancelled one is cancelled on the spot.
    JobHandle z = pool.schedule([&] { depRan.fetch_add(1); }, after("TestCancelZ", { x }));
    CHECK(z.status() == JobStatus::Cancelled);

    park.open();
    // The dead queue entry is dropped, the worker carries on with what follows.
    std::atomic<bool> next{ false };
    pool.schedule([&] { next.store(true); }, desc("TestAfterCancel", JobPriority::Low));
    REQUIRE(eventually([&] { return next.load(); }));
    CHECK(ran.load() == 0);
    CHECK(depRan.load() == 0);
    CHECK(cancelledCalls.load() == 2);   // exactly once each, not again on pop
    // Cancelling something already finished changes nothing.
    x.cancel();
    CHECK(x.status() == JobStatus::Cancelled);
}

TEST_CASE("CancelToken: one cancel drops every job that carries it; child tokens")
{
    std::atomic<int> ran{ 0 }, onCancelled{ 0 };
    ThreadPool pool(1);
    ParkedWorker park(pool);

    // One token per "region", one child per "chunk" in it.
    CancelToken region = CancelToken::create();
    CancelToken chunkA = region.child();
    CancelToken chunkB = region.child();
    CancelToken other  = CancelToken::create();

    auto job = [&](const char* name, CancelToken t) {
        JobDesc d = desc(name, JobPriority::Low);
        d.cancel      = std::move(t);
        d.onCancelled = [&] { onCancelled.fetch_add(1); };
        return pool.schedule([&] { ran.fetch_add(1); }, d);
    };
    JobHandle a = job("TestChunkA", chunkA);
    JobHandle b = job("TestChunkB", chunkB);
    JobHandle r = job("TestRegion", region);
    JobHandle o = job("TestOther",  other);

    chunkB.cancel();
    CHECK(chunkB.cancelled());
    CHECK_FALSE(region.cancelled());        // a child does not cancel its parent
    region.cancel();
    CHECK(chunkA.cancelled());              // the parent cancels every child
    CHECK_FALSE(other.cancelled());

    park.open();
    REQUIRE(returnsWithin([=] { a.wait(); b.wait(); r.wait(); o.wait(); }));
    CHECK(a.status() == JobStatus::Cancelled);
    CHECK(b.status() == JobStatus::Cancelled);
    CHECK(r.status() == JobStatus::Cancelled);
    CHECK(o.status() == JobStatus::Done);
    CHECK(ran.load() == 1);
    CHECK(onCancelled.load() == 3);

    // The none token never cancels.
    CancelToken none;
    none.cancel();
    CHECK_FALSE(none.cancelled());
    CHECK_FALSE(none.cancellable());
    CHECK(none.child().cancellable());

    // A token cancelled before scheduling: the job never even gets queued.
    JobHandle pre = job("TestPreCancelled", region);
    CHECK(pre.status() == JobStatus::Cancelled);
}

TEST_CASE("stale(): asked right before the start, so relevance can change while queued")
{
    std::atomic<bool> farAway{ false };
    std::atomic<int>  ran{ 0 }, asked{ 0 };
    ThreadPool pool(1);
    ParkedWorker park(pool);

    // A chunk load that is no longer wanted once the camera has moved away.
    JobDesc d = desc("TestChunkLoad", JobPriority::Low);
    d.stale = [&] { asked.fetch_add(1); return farAway.load(); };
    JobHandle gone = pool.schedule([&] { ran.fetch_add(1); }, d);
    JobHandle kept = pool.schedule([&] { ran.fetch_add(1); }, d);
    CHECK(asked.load() == 0);               // not asked at scheduling time

    farAway.store(true);                    // camera moved: both are stale now...
    park.open();
    REQUIRE(returnsWithin([gone] { gone.wait(); }));
    CHECK(gone.status() == JobStatus::Cancelled);
    REQUIRE(returnsWithin([kept] { kept.wait(); }));
    CHECK(kept.status() == JobStatus::Cancelled);
    CHECK(ran.load() == 0);

    farAway.store(false);                   // ...and back in range: this one runs
    JobHandle back = pool.schedule([&] { ran.fetch_add(1); }, d);
    REQUIRE(returnsWithin([back] { back.wait(); }));
    CHECK(back.status() == JobStatus::Done);
    CHECK(ran.load() == 1);

    // A throwing predicate counts as stale instead of escaping into the worker.
    JobDesc bad = desc("TestBadStale");
    bad.stale = []() -> bool { throw std::runtime_error("stale broke"); };
    JobHandle b = pool.schedule([&] { ran.fetch_add(1); }, bad);
    REQUIRE(returnsWithin([b] { b.wait(); }));
    CHECK(b.status() == JobStatus::Cancelled);
    CHECK(ran.load() == 1);
}

TEST_CASE("schedule: a throwing body fails its job, cancels dependents, spares the pool")
{
    std::atomic<bool> depRan{ false }, laterRan{ false };
    ThreadPool pool(2);
    JobHandle bad = pool.schedule([] { throw std::runtime_error("decode failed"); }, desc("TestThrows"));
    JobHandle dep = pool.schedule([&] { depRan.store(true); }, after("TestAfterThrow", { bad }));

    bool caught = false;
    REQUIRE(returnsWithin([&] {
        try { bad.wait(); }
        catch (const std::runtime_error& e) { caught = std::string(e.what()) == "decode failed"; }
    }));
    CHECK(caught);
    CHECK(bad.status() == JobStatus::Failed);
    REQUIRE(returnsWithin([dep] { dep.wait(); }));   // a cancelled job's wait() does not throw
    CHECK(dep.status() == JobStatus::Cancelled);
    CHECK_FALSE(depRan.load());

    pool.schedule([&] { laterRan.store(true); }, desc("TestStillAlive"));
    CHECK(eventually([&] { return laterRan.load(); }));
}

TEST_CASE("wait() on a saturated pool runs the queued chain itself instead of deadlocking")
{
    std::mutex om;
    std::vector<int> order;
    ThreadPool pool(1);
    ParkedWorker park(pool);   // the only worker is gone for the whole test

    JobHandle a = pool.schedule([&] { std::lock_guard<std::mutex> l(om); order.push_back(1); }, desc("TestChainA", JobPriority::Low));
    JobHandle b = pool.schedule([&] { std::lock_guard<std::mutex> l(om); order.push_back(2); }, after("TestChainB", { a }));
    JobHandle c = pool.schedule([&] { std::lock_guard<std::mutex> l(om); order.push_back(3); }, after("TestChainC", { b }, JobPriority::High));

    // Without help, c.wait() would sleep until a worker appears — never.
    REQUIRE(returnsWithin([c] { c.wait(); }));
    CHECK(c.status() == JobStatus::Done);
    CHECK(order == std::vector<int>{ 1, 2, 3 });
}

TEST_CASE("wait() inside a job on the only worker does not deadlock")
{
    std::atomic<bool> innerRan{ false };
    ThreadPool pool(1);
    // The outer job occupies the single worker and waits for a job that can only
    // ever be run by... the single worker. wait() takes it out of the queue.
    JobHandle outer = pool.schedule([&pool, &innerRan] {
        JobHandle inner = pool.schedule([&innerRan] { innerRan.store(true); }, desc("TestInner"));
        inner.wait();
    }, desc("TestOuter"));
    REQUIRE(returnsWithin([outer] { outer.wait(); }));
    CHECK(innerRan.load());
    CHECK(outer.status() == JobStatus::Done);
}

TEST_CASE("a pool destroyed while its job waits on another pool's job: cancelled, no hang")
{
    std::atomic<bool> ran{ false };
    ThreadPool longLived(1);
    ParkedWorker park(longLived);
    JobHandle dep = longLived.schedule([] {}, desc("TestOtherPoolDep"));
    JobHandle orphan;
    {
        ThreadPool shortLived(1);
        orphan = shortLived.schedule([&] { ran.store(true); }, after("TestOrphan", { dep }));
        CHECK(orphan.status() == JobStatus::Waiting);
    }   // gone, with `orphan` still parked on `dep`
    park.open();
    REQUIRE(returnsWithin([orphan] { orphan.wait(); }));
    CHECK(orphan.status() == JobStatus::Cancelled);
    CHECK_FALSE(ran.load());
    CHECK(dep.status() == JobStatus::Done);
}

TEST_CASE("parallel_for helpers overtake queued streaming jobs in the global pool")
{
    // The baseline's complaint (Thema 153 §4.1): asset loads queued ahead of a
    // frame left the frame computing alone. Fill the global pool's Low share with
    // blocked "loads" and queue more behind them; the frame's helpers are High
    // and the cap keeps workers free, so a worker still takes part.
    ThreadPool& pool = globalPool();
    if (pool.threadCount() < 3) return;   // no free worker by design on a tiny pool
    Gate loads;
    std::atomic<int> loadsRunning{ 0 };
    const size_t cap = pool.concurrencyLimit(JobPriority::Low);
    std::vector<JobHandle> handles;
    for (size_t i = 0; i < cap + 16; ++i)
        handles.push_back(pool.schedule([&] { loadsRunning.fetch_add(1); loads.wait(); loadsRunning.fetch_sub(1); },
                                        desc("TestStreamingLoad", JobPriority::Low)));
    REQUIRE(eventually([&] { return loadsRunning.load() == static_cast<int>(cap); }));

    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> workerRan{ false };
    bool callerGaveUp = false;
    parallel_for(4096, [&](size_t) {
        if (std::this_thread::get_id() != caller) { workerRan.store(true); return; }
        // The caller holds back until a worker has joined in, with a deadline so
        // a regression shows up as a failed CHECK rather than a hang.
        if (!callerGaveUp && !eventually([&] { return workerRan.load(); }, std::chrono::seconds(5)))
            callerGaveUp = true;
    }, "TestFrameHelper", 16);
    CHECK(workerRan.load());
    CHECK_FALSE(callerGaveUp);

    loads.release();
    for (auto& h : handles) REQUIRE(returnsWithin([h] { h.wait(); }));
}

TEST_CASE("random job graphs with cancels and failures always finish, deps respected")
{
    // A cheap stand-in for TSan on this machine: many small DAGs, every feature
    // at once, on several workers. Invariants: everything finishes; a body runs
    // at most once; a body runs only after all its dependencies' bodies did.
    constexpr int kJobs = 1500;
    std::vector<std::atomic<int>> runs(kJobs);
    std::vector<std::atomic<bool>> bodyDone(kJobs);
    for (auto& r : runs) r.store(0);
    for (auto& b : bodyDone) b.store(false);
    std::atomic<int> orderViolations{ 0 };
    std::vector<std::vector<int>> deps(kJobs);
    CancelToken tokens[4] = { CancelToken::create(), CancelToken::create(),
                              CancelToken::create(), CancelToken::create() };
    std::vector<JobHandle> hs(kJobs);

    uint32_t rng = 0x9E3779B9u;
    auto next = [&rng] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };

    ThreadPool pool(4);
    for (int i = 0; i < kJobs; ++i)
    {
        JobDesc d;
        d.name     = "TestRandomGraph";
        const uint32_t pr = next() % 3;
        d.priority = static_cast<JobPriority>(pr);
        const uint32_t nd = (i == 0) ? 0 : next() % 4;
        for (uint32_t k = 0; k < nd; ++k)
        {
            const uint32_t span = static_cast<uint32_t>(std::min(i, 40));
            const int dep = i - 1 - static_cast<int>(next() % span);
            deps[i].push_back(dep);
            d.after.push_back(hs[dep]);
        }
        if (next() % 8 == 0) d.cancel = tokens[next() % 4];
        const bool throws = next() % 50 == 0;
        hs[i] = pool.schedule([&, i, throws] {
            runs[i].fetch_add(1);
            for (int dp : deps[i])
                if (!bodyDone[dp].load()) orderViolations.fetch_add(1);
            if (throws) throw std::runtime_error("random failure");
            bodyDone[i].store(true);
        }, d);
        if (next() % 40 == 0) hs[next() % (i + 1)].cancel();
        if (i == kJobs / 2) tokens[1].cancel();
    }
    tokens[3].cancel();

    REQUIRE(returnsWithin([&] {
        for (auto& h : hs) { try { h.wait(); } catch (const std::runtime_error&) {} }
    }, std::chrono::seconds(30)));
    int done = 0, failed = 0, cancelled = 0;
    for (int i = 0; i < kJobs; ++i)
    {
        CHECK(runs[i].load() <= 1);
        switch (hs[i].status())
        {
        case JobStatus::Done:      ++done; CHECK(runs[i].load() == 1); break;
        case JobStatus::Failed:    ++failed; break;
        case JobStatus::Cancelled: ++cancelled; CHECK(runs[i].load() == 0); break;
        default:                   FAIL("job not finished after wait()");
        }
        // Done means every dependency was Done too.
        if (hs[i].status() == JobStatus::Done)
            for (int dp : deps[i]) CHECK(hs[dp].status() == JobStatus::Done);
    }
    CHECK(orderViolations.load() == 0);
    CHECK(done > 0);
    CHECK(failed > 0);
    CHECK(cancelled > 0);
    MESSAGE("random graph: " << done << " done, " << failed << " failed, " << cancelled << " cancelled");
}
