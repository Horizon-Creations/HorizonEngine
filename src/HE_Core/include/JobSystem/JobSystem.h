#pragma once
#include "Types/Defines.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Fixed-size thread pool. Workers pull tasks from a shared queue.
class HE_API ThreadPool {
public:
    explicit ThreadPool(size_t threadCount);
    ~ThreadPool();

    // `name` labels the task on the profiler's per-thread timeline. It MUST be a
    // string literal / static storage: the profiler stores the pointer in a span
    // and reads it later, at dump or draw time.
    //
    // It exists because the worker lanes were previously a solid wall of
    // "Job::Execute" — technically a timeline, practically unreadable. A lane that
    // says FrustumCull / ExtractMeshes / SkyEnvBake answers "what is the pool
    // doing"; one that says Job::Execute only answers "something".
    template<typename F>
    std::future<void> submit(F&& f, const char* name = "Job::Execute")
    {
        auto task = std::make_shared<std::packaged_task<void()>>(std::forward<F>(f));
        std::future<void> fut = task->get_future();
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push(Task{ [task]{ (*task)(); }, name });
        }
        m_cv.notify_one();
        return fut;
    }

    // Fire-and-forget: enqueue `copies` instances of f under ONE lock, with no
    // packaged_task, future or shared state. That is what parallel_for's helpers
    // need — the per-call allocations of submit() were a measurable share of the
    // frame's allocations (perf audit step 3, B2). f MUST NOT throw: nothing waits
    // on a future here, so an exception reaching the worker loop terminates.
    template<typename F>
    void post(F&& f, const char* name = "Job::Execute", size_t copies = 1)
    {
        if (copies == 0) return;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            std::function<void()> fn(std::forward<F>(f));
            for (size_t i = 1; i < copies; ++i)
                m_queue.push(Task{ fn, name });
            m_queue.push(Task{ std::move(fn), name });
        }
        if (copies >= m_threads.size()) m_cv.notify_all();
        else for (size_t i = 0; i < copies; ++i) m_cv.notify_one();
    }

    size_t threadCount() const { return m_threads.size(); }

private:
    struct Task
    {
        std::function<void()> fn;
        const char*           name;   // static storage — see submit()
    };

    std::vector<std::thread> m_threads;
    std::queue<Task>         m_queue;
    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    bool                              m_stop = false;
};

// Process-wide thread pool (hardware_concurrency threads, created on first use).
HE_API ThreadPool& globalPool();

// Default minimum number of indices per parallel_for chunk. Below 2× this a call
// runs inline on the caller and never touches the pool.
//
// Why a floor at all: the perf audit (step 3, B2) found extract + cull fanning 4
// objects out as 27 jobs per frame, each doing ~0.7 µs of work behind a queue
// push, a worker wake-up and a future — and under system load the render thread
// waited 8.7 ms p50 per frame on work worth 0.07 ms. Handing a chunk to another
// thread costs microseconds even when every core is idle; a chunk has to carry
// at least that much work to pay for itself. Cheap bodies (a frustum test, a
// per-object copy) take tens to hundreds of ns per index, so a few hundred
// indices is where splitting starts to win. Bodies that are expensive per index
// (a whole sky-bake row) pass a smaller grain explicitly.
constexpr size_t kParallelForMinGrain = 256;

// How many chunks parallel_for splits `count` indices into, given `workers` pool
// threads. Every chunk holds at least `minGrain` indices (0 is treated as 1), so
// 1 means "run inline". Capped at 4 × (workers + 1): chunks are claimed
// dynamically, so a few more chunks than threads let the others absorb the rest
// when one thread is preempted mid-chunk, at the cost of one atomic per chunk.
inline size_t parallel_for_chunks(size_t count, size_t minGrain, size_t workers)
{
    const size_t grain = std::max<size_t>(1, minGrain);
    const size_t cap   = 4 * (std::max<size_t>(1, workers) + 1);
    return std::clamp<size_t>(count / grain, 1, cap);
}

// Shared state of one parallel_for call. Lives on the heap (shared_ptr) because
// helper tasks may be dequeued AFTER parallel_for has returned — a late helper
// finds no chunk left to claim and touches nothing but this struct. `body`/`ctx`
// point at the caller's stack and are only followed after a successful claim, and
// every claim completes before the caller can return.
struct ParallelForJob
{
    using Body = void (*)(void* ctx, size_t begin, size_t end);

    Body   body   = nullptr;
    void*  ctx    = nullptr;
    size_t count  = 0;
    size_t chunks = 0;

    std::atomic<size_t> next{ 0 };        // next chunk to claim
    std::atomic<size_t> remaining{ 0 };   // chunks not yet finished
    std::atomic<bool>   failed{ false };  // set once, by the thread that stores `error`
    std::exception_ptr  error;
    std::mutex              doneMutex;
    std::condition_variable doneCv;
};

// Claim and run chunks until none are left. Never throws: the first exception is
// kept in job.error and later chunks are skipped (still counted as finished).
HE_API void parallel_for_run_chunks(ParallelForJob& job);
// Block until every chunk has finished, then rethrow job.error if one was stored.
HE_API void parallel_for_wait(ParallelForJob& job);

// Distribute [0, count) invocations of f across the global pool and block until
// all finish. f(i) is called exactly once for each i in [0, count) — unless f
// throws: then the first exception is rethrown here, after every chunk already
// started has finished, and chunks not yet started are skipped. f is never still
// running on another thread when parallel_for returns or throws.
//
// Work is split into contiguous CHUNKS of at least `minGrain` indices (see
// kParallelForMinGrain) — a call too small for two chunks never leaves the
// calling thread. Chunks are CLAIMED, not assigned: `min(workers, chunks - 1)`
// helper tasks go to the pool, and the caller claims chunks from the same atomic
// counter until none are left. So the caller never sleeps while work of its own
// call is still unstarted; it only waits for chunks a worker is actually running.
// A worker that wakes up late (loaded machine, E-cores) finds nothing to claim
// and costs the caller nothing — and a saturated pool, or a parallel_for nested
// inside a job, degrades to the caller doing everything instead of deadlocking.
//
// The caller helps with ITS OWN chunks only, never with the pool's general queue:
// that queue also carries asset loads, thumbnails and content sync, and a frame
// must not pick up a 50 ms import because it had a microsecond to spare.
//
// `name` labels every helper on the profiler's worker lanes (string literal —
// see ThreadPool::submit). Give each call site its own: on the timeline the name
// IS the identity of the work, and the default tells you nothing.
template<typename F>
void parallel_for(size_t count, F&& f, const char* name = "ParallelFor",
                  size_t minGrain = kParallelForMinGrain)
{
    if (count == 0) return;
    if (count / std::max<size_t>(1, minGrain) < 2)
    {
        for (size_t i = 0; i < count; ++i) f(i);
        return;
    }

    ThreadPool& pool = globalPool();
    const size_t workers = std::max<size_t>(1, pool.threadCount());
    const size_t chunks  = parallel_for_chunks(count, minGrain, workers);

    using Fn = std::remove_reference_t<F>;
    auto job    = std::make_shared<ParallelForJob>();
    job->body   = [](void* ctx, size_t begin, size_t end) {
        Fn& fn = *static_cast<Fn*>(ctx);
        for (size_t i = begin; i < end; ++i) fn(i);
    };
    job->ctx    = const_cast<void*>(static_cast<const void*>(std::addressof(f)));
    job->count  = count;
    job->chunks = chunks;
    job->remaining.store(chunks, std::memory_order_relaxed);

    pool.post([job]{ parallel_for_run_chunks(*job); }, name,
              std::min(workers, chunks - 1));
    parallel_for_run_chunks(*job);
    parallel_for_wait(*job);
}
